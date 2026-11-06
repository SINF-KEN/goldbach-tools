// vis_test.cpp — Level Zero store->atomic visibility testcase (B580/Xe2).
//
// Question under test (goldbach-l0 env-quirk claim): does an in-order
// queue succession or an event wait make kernel A's regular global stores
// visible to kernel B's atomics on the same words — or is an explicit
// zeCommandListAppendBarrier required?
//
// Per iteration:
//   K_fill writes pattern P (alternating 0xAAAAAAAA / 0x55555555)
//   K_atomic ANDs a fixed mask M into every word
//   expected word value = P & M; anything else = visibility loss:
//     0            -> atomic read pre-fill zeros (stale-zero)
//     prevExpected -> atomic read the PREVIOUS iteration's data (stale-prev)
//     other        -> partially visible word
//
// Modes:
//   inorder    immediate IN_ORDER list: K_fill, K_atomic (no barrier)
//   barrier    immediate IN_ORDER list: K_fill, AppendBarrier, K_atomic
//   event      two command lists, K2 list waits on plain K1 event
//   event_ts   same, but the event pool is KERNEL_TIMESTAMP-flagged
//
// Usage: vis_test <mode> [iters] [nwords]

#include <level_zero/ze_api.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#define ZE_CHECK(x) do { ze_result_t _r = (x); if (_r != ZE_RESULT_SUCCESS) { \
    fprintf(stderr, "L0 error %d at %s:%d\n", (int)_r, __FILE__, __LINE__); exit(1); } } while (0)

static const uint32_t WG = 256;
static const uint32_t M = 0x77777777u;

static ze_driver_handle_t driver;
static ze_device_handle_t device;
static ze_context_handle_t ctx;
static ze_command_list_handle_t imm;   // immediate in-order list
static ze_command_queue_handle_t queue; // for regular-list modes
static ze_command_list_handle_t l1, l2; // regular lists (event modes)
static ze_module_handle_t module;
static ze_kernel_handle_t k_fill, k_atomic;
static void* dev_buf = nullptr;
static void* sh_buf = nullptr;
static size_t nbytes = 0;

static void launch(ze_command_list_handle_t list, ze_kernel_handle_t k,
                   void* buf, uint32_t n, uint32_t v2,
                   ze_event_handle_t signal) {
    void* a0 = buf; uint32_t a1 = n; uint32_t a2 = v2;
    ZE_CHECK(zeKernelSetArgumentValue(k, 0, sizeof(void*), &a0));
    ZE_CHECK(zeKernelSetArgumentValue(k, 1, sizeof(uint32_t), &a1));
    ZE_CHECK(zeKernelSetArgumentValue(k, 2, sizeof(uint32_t), &a2));
    ze_group_count_t g{ (n + WG - 1) / WG, 1, 1 };
    ZE_CHECK(zeCommandListAppendLaunchKernel(list, k, &g, signal, 0, nullptr));
}

// launch64: second arg is a u64 (pattern) — for the SLM-staged fill kernel
static void launch64(ze_command_list_handle_t list, ze_kernel_handle_t k,
                     void* buf, uint32_t n, uint64_t v2,
                     ze_event_handle_t signal) {
    void* a0 = buf; uint32_t a1 = n; uint64_t a2 = v2;
    ZE_CHECK(zeKernelSetArgumentValue(k, 0, sizeof(void*), &a0));
    ZE_CHECK(zeKernelSetArgumentValue(k, 1, sizeof(uint32_t), &a1));
    ZE_CHECK(zeKernelSetArgumentValue(k, 2, sizeof(uint64_t), &a2));
    ze_group_count_t g{ (n + WG - 1) / WG, 1, 1 };
    ZE_CHECK(zeCommandListAppendLaunchKernel(list, k, &g, signal, 0, nullptr));
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <inorder|barrier|event|event_ts> [iters] [nwords]\n", argv[0]); return 2; }
    std::string mode = argv[1];
    int iters = argc > 2 ? atoi(argv[2]) : 25;
    uint32_t n = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 10) : 2u * 1024 * 1024;
    int nbytes_factor = 4;
    bool use_event = (mode == "event" || mode == "event_ts");
    bool use_barrier = (mode == "barrier");
    bool slm64 = (mode == "slm_inorder" || mode == "slm_barrier");
    bool slm_barrier = (mode == "slm_barrier");
    if (mode != "inorder" && !use_barrier && !use_event && !slm64) { fprintf(stderr, "bad mode\n"); return 2; }
    if (slm64) { n = (n + 1) / 2; nbytes_factor = 8; } // interpret n as u64 words

    ZE_CHECK(zeInit(ZE_INIT_FLAG_GPU_ONLY));
    uint32_t nd = 0; ZE_CHECK(zeDriverGet(&nd, nullptr));
    std::vector<ze_driver_handle_t> drivers(nd);
    ZE_CHECK(zeDriverGet(&nd, drivers.data()));
    for (auto d : drivers) {
        uint32_t ndev = 0;
        if (zeDeviceGet(d, &ndev, nullptr) == ZE_RESULT_SUCCESS && ndev) {
            std::vector<ze_device_handle_t> devs(ndev);
            ZE_CHECK(zeDeviceGet(d, &ndev, devs.data()));
            for (auto dv : devs) {
                ze_device_properties_t p{}; p.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
                ZE_CHECK(zeDeviceGetProperties(dv, &p));
                if (p.type == ZE_DEVICE_TYPE_GPU) { driver = d; device = dv; break; }
            }
        }
        if (device) break;
    }
    if (!device) { fprintf(stderr, "no GPU\n"); return 1; }
    ze_context_desc_t cd{}; cd.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC;
    ZE_CHECK(zeContextCreate(driver, &cd, &ctx));

    ze_command_queue_desc_t qd{};
    qd.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC;
    qd.flags = ZE_COMMAND_QUEUE_FLAG_IN_ORDER;
    qd.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
    ZE_CHECK(zeCommandListCreateImmediate(ctx, device, &qd, &imm));

    if (use_event) {
        ZE_CHECK(zeCommandQueueCreate(ctx, device, &qd, &queue));
        ze_command_list_desc_t ld{};
        ld.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC;
        ZE_CHECK(zeCommandListCreate(ctx, device, &ld, &l1));
        ZE_CHECK(zeCommandListCreate(ctx, device, &ld, &l2));
    }

    // module
    FILE* f = fopen("vis_bmg.spv", "rb");
    if (!f) { fprintf(stderr, "cannot open vis_bmg.spv (run ocloc first)\n"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> ilv(sz);
    if (fread(ilv.data(), 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    ze_module_desc_t md{};
    md.stype = ZE_STRUCTURE_TYPE_MODULE_DESC;
    md.format = ZE_MODULE_FORMAT_IL_SPIRV;
    md.inputSize = ilv.size();
    md.pInputModule = ilv.data();
    ZE_CHECK(zeModuleCreate(ctx, device, &md, &module, nullptr));
    ze_kernel_desc_t kd{};
    kd.stype = ZE_STRUCTURE_TYPE_KERNEL_DESC;
    kd.pKernelName = "k_fill";
    ZE_CHECK(zeKernelCreate(module, &kd, &k_fill));
    kd.pKernelName = "k_atomic";
    ZE_CHECK(zeKernelCreate(module, &kd, &k_atomic));
    ZE_CHECK(zeKernelSetGroupSize(k_fill, WG, 1, 1));
    ZE_CHECK(zeKernelSetGroupSize(k_atomic, WG, 1, 1));
    ze_kernel_handle_t kF = k_fill, kA = k_atomic;
    if (slm64) {
        kd.pKernelName = "k_fill_slm64";
        ZE_CHECK(zeKernelCreate(module, &kd, &kF));
        kd.pKernelName = "k_atomic_half";
        ZE_CHECK(zeKernelCreate(module, &kd, &kA));
        ZE_CHECK(zeKernelSetGroupSize(kF, WG, 1, 1));
        ZE_CHECK(zeKernelSetGroupSize(kA, WG, 1, 1));
    }

    // buffers
    nbytes = (size_t)n * nbytes_factor;
    ze_device_mem_alloc_desc_t dd{}; dd.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
    ZE_CHECK(zeMemAllocDevice(ctx, &dd, nbytes, 4096, device, &dev_buf));
    ze_host_mem_alloc_desc_t hd{}; hd.stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC;
    ze_device_mem_alloc_desc_t dd2{}; dd2.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
    ZE_CHECK(zeMemAllocShared(ctx, &dd2, &hd, nbytes, 4096, device, &sh_buf));

    // event for the event modes
    ze_event_handle_t e1 = nullptr;
    ze_event_pool_handle_t pool = nullptr;
    if (use_event) {
        ze_event_pool_desc_t pd{};
        pd.stype = ZE_STRUCTURE_TYPE_EVENT_POOL_DESC;
        pd.count = 1;
        pd.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE |
                   ((mode == "event_ts") ? ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP : 0);
        ZE_CHECK(zeEventPoolCreate(ctx, &pd, 0, nullptr, &pool));
        ze_event_desc_t ed{};
        ed.stype = ZE_STRUCTURE_TYPE_EVENT_DESC;
        ed.index = 0;
        ed.signal = ZE_EVENT_SCOPE_FLAG_HOST;
        ed.wait = ZE_EVENT_SCOPE_FLAG_DEVICE;
        ZE_CHECK(zeEventCreate(pool, &ed, &e1));
    }

    // deterministic zero start, fully synchronized
    if (slm64) launch64(imm, kF, dev_buf, n, 0ULL, nullptr);
    else       launch(imm, kF, dev_buf, n, 0u, nullptr);
    ZE_CHECK(zeCommandListHostSynchronize(imm, UINT64_MAX));

    const uint64_t M64 = 0x7777777777777777ULL;
    uint32_t pat = 0xAAAAAAAAu;
    uint64_t pat64 = 0xAAAAAAAAAAAAAAAAULL;
    long total_bad = 0, total_zero = 0, total_prev = 0, total_other = 0;
    int iters_bad = 0, worst_iter_bad = 0;
    uint64_t prev_expected = 0;

    for (int it = 0; it < iters; ++it) {
        uint64_t expected = slm64 ? (pat64 & M64) : (uint64_t)(pat & M);
        if (use_event) {
            ZE_CHECK(zeEventHostReset(e1));
            ze_command_list_handle_t ls[1] = { l1 };
            zeCommandListReset(l1); zeCommandListReset(l2);
            launch(l1, k_fill, dev_buf, n, pat, e1);
            ZE_CHECK(zeCommandListClose(l1));
            ZE_CHECK(zeCommandQueueExecuteCommandLists(queue, 1, ls, nullptr));
            ZE_CHECK(zeCommandListAppendWaitOnEvents(l2, 1, &e1));
            launch(l2, k_atomic, dev_buf, n, M, nullptr);
            ZE_CHECK(zeCommandListClose(l2));
            ls[0] = l2;
            ZE_CHECK(zeCommandQueueExecuteCommandLists(queue, 1, ls, nullptr));
            ZE_CHECK(zeCommandQueueSynchronize(queue, UINT64_MAX));
        } else {
            if (slm64) launch64(imm, kF, dev_buf, n, pat64, nullptr);
            else       launch(imm, kF, dev_buf, n, pat, nullptr);
            if (use_barrier || slm_barrier)
                ZE_CHECK(zeCommandListAppendBarrier(imm, nullptr, 0, nullptr));
            launch(imm, kA, dev_buf, n, M, nullptr);
            ZE_CHECK(zeCommandListHostSynchronize(imm, UINT64_MAX));
        }

        // readback through the immediate list (in-order after the kernels)
        ZE_CHECK(zeCommandListAppendMemoryCopy(imm, sh_buf, dev_buf, nbytes, nullptr, 0, nullptr));
        ZE_CHECK(zeCommandListHostSynchronize(imm, UINT64_MAX));

        long bad = 0, z = 0, pv = 0, ot = 0;
        if (slm64) {
            const volatile uint64_t* w = (const volatile uint64_t*)sh_buf;
            for (uint32_t i = 0; i < n; ++i) {
                uint64_t v = w[i];
                if (v == expected) continue;
                ++bad;
                if (v == 0) ++z;
                else if (it > 0 && v == prev_expected) ++pv;
                else ++ot;
            }
        } else {
            const volatile uint32_t* w = (const volatile uint32_t*)sh_buf;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t v = w[i];
                if (v == expected) continue;
                ++bad;
                if (v == 0) ++z;
                else if (it > 0 && v == (uint32_t)prev_expected) ++pv;
                else ++ot;
            }
        }
        if (bad) { ++iters_bad; if (bad > worst_iter_bad) worst_iter_bad = (int)bad; }
        total_bad += bad; total_zero += z; total_prev += pv; total_other += ot;
        if (bad) printf("  iter %d: bad=%ld (zero=%ld prev=%ld other=%ld)\n", it, bad, z, pv, ot);
        prev_expected = expected;
        pat = ~pat; pat64 = ~pat64;
    }

    printf("mode=%s iters=%d nwords=%u -> corrupt_iters=%d total_bad=%ld "
           "(zero=%ld prev=%ld other=%ld) worst_iter=%d\n",
           mode.c_str(), iters, n, iters_bad, total_bad,
           total_zero, total_prev, total_other, worst_iter_bad);
    return iters_bad ? 1 : 0;
}
