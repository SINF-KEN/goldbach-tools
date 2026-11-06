// Ladder probe: how far does a trivial L0 program get with the validation layer on?
// Run 1: ZE_ENABLE_VALIDATION_LAYER=1 (expect SIGSEGV at some step)
// Run 2: no layer (expect all steps OK)
#include <level_zero/ze_api.h>
#include <cstdio>
#include <cstdlib>
static void step(const char* name, ze_result_t r) {
    printf("STEP %-28s -> 0x%x\n", name, (unsigned)r);
    fflush(stdout);
    if (r != ZE_RESULT_SUCCESS) exit(2);
}
int main() {
    step("zeInit", zeInit(ZE_INIT_FLAG_GPU_ONLY));
    uint32_t n = 0; step("zeDriverGet(count)", zeDriverGet(&n, nullptr));
    ze_driver_handle_t ds[4]; step("zeDriverGet", zeDriverGet(&n, ds));
    uint32_t m = 0; step("zeDeviceGet(count)", zeDeviceGet(ds[0], &m, nullptr));
    ze_device_handle_t dv[4]; step("zeDeviceGet", zeDeviceGet(ds[0], &m, dv));
    ze_context_desc_t cd{}; cd.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC;
    ze_context_handle_t ctx;
    step("zeContextCreate", zeContextCreate(ds[0], &cd, &ctx));
    ze_event_pool_desc_t pd{};
    pd.stype = ZE_STRUCTURE_TYPE_EVENT_POOL_DESC;
    pd.count = 1;   // VALID
    ze_event_pool_handle_t pool;
    step("zeEventPoolCreate(count=1)", zeEventPoolCreate(ctx, &pd, 0, nullptr, &pool));
    ze_event_desc_t ed{}; ed.stype = ZE_STRUCTURE_TYPE_EVENT_DESC; ed.index = 0;
    ed.signal = ZE_EVENT_SCOPE_FLAG_HOST; ed.wait = 0u;
    ze_event_handle_t ev;
    step("zeEventCreate", zeEventCreate(pool, &ed, &ev));
    printf("ALL STEPS OK\n");
    return 0;
}
