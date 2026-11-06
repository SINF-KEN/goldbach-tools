# L0: zeEventPoolCreate with desc.count = 0 aborts the process in gmm_helper/resource_info.cpp instead of returning an error

## Summary

Creating an event pool from a zero-initialized `ze_event_pool_desc_t` (the
`count` field left at 0) **aborts the process** inside the driver:

```
Abort was called at 15 line in file:
/usr/src/debug/intel-compute-runtime/compute-runtime-26.31.39395.13/shared/source/gmm_helper/resource_info.cpp
```

instead of returning `ZE_RESULT_ERROR_INVALID_SIZE` /
`ZE_RESULT_ERROR_INVALID_ARGUMENT`. With the validation layer disabled
(the common deployment), there is no guard at all between the invalid
argument and the abort.

## Environment

- intel-compute-runtime 26.31.39395.13, Intel Arc B580 (Battlemage / Xe2)
- Level Zero via the distro loader (`libze_loader`), headers from
  `/usr/include/level_zero`
- OS: CachyOS x86_64, kernel 7.2.3-1-cachyos
- Date tested: 2026-09-05

## Steps to reproduce

`repro.cpp`:

```cpp
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
```

Build and run:

```sh
g++ -O2 -std=c++17 -I /usr/include/level_zero repro.cpp -o repro -lze_loader
./repro     # process aborts (SIGABRT) inside zeEventPoolCreate
```

The abort reproduces with `pd.flags = 0` and with
`ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP`; it disappears when `pd.count = 1`.

## Expected

An error return (`ZE_RESULT_ERROR_INVALID_SIZE` or similar). The Level Zero
error-handling contract should hold regardless of the validation layer: an
invalid descriptor must not take down the calling process.

## Mechanism (traced in the 26.31.39395.13 sources)

- `EventPool::create` (`level_zero/core/source/event/event.cpp:252`) performs
  no validation of `desc->count` and goes straight to `initialize()`.
- `EventPool::initialize` (`event.cpp:42`) already returns
  `ZE_RESULT_ERROR_INVALID_ARGUMENT` for other invalid descriptor
  combinations (e.g. IPC + counter-based flags) — the validation style and
  location for a `count == 0` check already exist there; count is simply not
  among the checked fields.
- `initializeSizeParameters` (`event.cpp:226`) computes
  `eventPoolSize = alignUp(numEvents * eventSize, 64 KiB)`. With
  `count == 0` this is a zero-sized `AllocationProperties` (`event.cpp:91`)
  handed to the memory manager.
- The zero-sized GMM resource creation fails and returns a null handle;
  `GmmResourceInfo::create` wraps that in
  `UNRECOVERABLE_IF(resourceInfo->peekHandle() == 0)`
  (`shared/source/gmm_helper/resource_info.cpp:15`) — the abort seen above.
  `UNRECOVERABLE_IF` is NEO's internal-invariant trap: an application-
  controllable input reaching it turns a recoverable invalid-argument case
  into process death.

This also explains the flags-independence: `eventSize` differs between plain
and `KERNEL_TIMESTAMP` pools, but any `eventSize * 0` is still 0.

Suggested fix: reject early in `EventPool::create`/`initialize`, matching the
existing flag validations —
`if (desc->count == 0) { result = ZE_RESULT_ERROR_INVALID_SIZE; return nullptr; }`.
(The Level Zero headers already document the constraint:
`ze_api.h` on `ze_event_pool_desc_t::count` — "number of events within the
pool; must be greater than 0".)

## Related

- intel/compute-runtime#922 — same `resource_info.cpp:15` abort site
  (different trigger: multi-rank `zeInit` race). Not a duplicate; shows this
  abort line is a generic backstop that surfaces many root causes as an
  opaque process abort.

## Notes

Context for how this mistake happens naturally: this header generation
carries the event count in the descriptor (`pd.count`), while older Level
Zero code passed the count as a `zeEventPoolCreate` argument (the third
argument is now the device-list length). Code migrated between header
versions can leave `count` at its zero default — the same shape as the
reproducer. The report is about the failure mode (process abort vs. error
return), not about the invalid input itself.

---

## DO NOT PASTE — local verification notes (2026-09-05)

- Reproduced with draft-verbatim code: count=0 -> SIGABRT exit 134 at
  resource_info.cpp:15 (both flags=0 and KERNEL_TIMESTAMP); count=1 ->
  zeEventPoolCreate returns, "unreachable" printed, exit 0.
- Validation layer observation (kept OUT of the report body to stay
  focused): libze_validation_layer.so.1.32.0 installed here; with
  ZE_ENABLE_VALIDATION_LAYER=1 the repro SEGSEGVs (exit 139) on BOTH
  count=0 and count=1 — the layer itself crashes on this stack, so it
  could not be evaluated as a guard. The report's claim stays conditional
  ("with the validation layer disabled ... there is no guard").
- Duplicate scan (web): no match for EventPoolCreate+count0; closest is
  intel/compute-runtime#922 (same abort line, zeInit race, different bug).
- Sources fetched from the tag: event.cpp, resource_info.cpp (scratch
  /tmp/bugverify, volatile).
