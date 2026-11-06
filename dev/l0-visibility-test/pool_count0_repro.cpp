// Issue #4 reproducer — zeEventPoolCreate with desc.count = 0 aborts the
// process in gmm_helper/resource_info.cpp instead of returning an error.
// Verified 2026-09-05 on compute-runtime 26.31.39395.13 / Arc B580:
//   count = 0                -> SIGABRT, "Abort was called at 15 line in file:
//                                .../gmm_helper/resource_info.cpp"
//   count = 0 + KERNEL_TIMESTAMP flag -> same abort
//   count = 1                -> zeEventPoolCreate returns, "unreachable" printed
#include <level_zero/ze_api.h>
#include <cstdio>
static ze_driver_handle_t drv; static ze_device_handle_t dev;
static ze_context_handle_t ctx;
#define CK(x) do{ ze_result_t r=(x); printf("%-30s -> %d\n", #x, (int)r); if(r) return 1; }while(0)
int main() {
    CK(zeInit(ZE_INIT_FLAG_GPU_ONLY));
    uint32_t n = 0; CK(zeDriverGet(&n, nullptr));
    ze_driver_handle_t ds[4]; CK(zeDriverGet(&n, ds));
    uint32_t m = 0; CK(zeDeviceGet(ds[0], &m, nullptr));
    ze_device_handle_t dv[4]; CK(zeDeviceGet(ds[0], &m, dv)); dev = dv[0];
    ze_context_desc_t cd{}; cd.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC;
    CK(zeContextCreate(ds[0], &cd, &ctx));
    ze_event_pool_desc_t pd{};
    pd.stype = ZE_STRUCTURE_TYPE_EVENT_POOL_DESC;
    pd.count = 0;                      // <-- the invalid input (left default)
    ze_event_pool_handle_t pool = nullptr;
    zeEventPoolCreate(ctx, &pd, 0, nullptr, &pool);   // never returns
    printf("unreachable\n");
    return 0;
}
