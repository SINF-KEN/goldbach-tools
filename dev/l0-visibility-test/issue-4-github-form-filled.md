<!-- Filled 2026-09-05 for the [Bug Report] Linux GPU Driver Issue template.
     Copy each field into the matching form box. Verified live on this machine. -->

## Title

```
zeEventPoolCreate with desc.count = 0 aborts the process instead of returning an error
```

## Pre-submission Checklist

- ✅ I am using the latest GPU driver version — `intel-compute-runtime 26.31.39395.13-1.1` (matches the newest release tag `26.31.39395.13`)
- ✅ I have searched for similar issues and found none — closest is #922 (same abort line `resource_info.cpp:15`, but a different trigger: multi-rank `zeInit` race)

## System Information

### GPU Hardware *

```
Intel Arc B580 (Battlemage G21 / BMG, device 8086:e20b, ASRock board)
```

### DRI Devices Information *

```
$ ls -ls /dev/dri/*
0 crw-rw----+ 1 root video  226,   0  5 sep 14:51 /dev/dri/card0
0 crw-rw----+ 1 root video  226,   1  5 sep 13:58 /dev/dri/card1
0 crw-rw-rw-  1 root render 226, 128  4 sep 13:03 /dev/dri/renderD128
0 crw-rw-rw-  1 root render 226, 129  4 sep 13:03 /dev/dri/renderD129

$ ls -la /dev/dri/by-path/
total 0
0 lrwxrwxrwx 1 root root  8  4 sep 13:03 pci-0000:03:00.0-card -> ../card0
0 lrwxrwxrwx 1 root root 13  4 sep 13:03 pci-0000:03:00.0-render -> ../renderD128
0 lrwxrwxrwx 1 root root  8  4 sep 13:03 pci-0000:0e:00.0-card -> ../card1
0 lrwxrwxrwx 1 root root 13  4 sep 13:03 pci-0000:0e:00.0-render -> ../renderD129

The Intel Arc B580 is at PCI 0000:03:00.0 (card0/renderD128).
The second adapter (0000:0e:00.0) is the AMD Raphael iGPU, not involved in this report.
```

### GPU Detailed Information (lspci output) *

```
$ lspci -vvv -k -s 0000:03:00.0
03:00.0 VGA compatible controller: Intel Corporation Battlemage G21 [Arc B580] (prog-if 00 [VGA controller])
	Subsystem: ASRock Incorporation Device 6021
	Control: I/O- Mem+ BusMaster+ SpecCycle- MemWINV- VGASnoop- ParErr- Stepping- SERR- FastB2B- DisINTx-
	Status: Cap+ 66MHz- UDF- FastB2B- ParErr- DEVSEL=fast >TAbort- <TAbort- <MAbort- >SERR- <PERR- INTx-
	Latency: 0, Cache Line Size: 64 bytes
	Interrupts: unknown pin routed to IRQ 89, MSI(X) routed to IRQ 89
	IOMMU group: 15
	Region 0: Memory at f4000000 (64-bit, non-prefetchable) [size=16M]
	Region 2: Memory at f800000000 (64-bit, prefetchable) [size=16G]
	Expansion ROM at f5000000 [disabled] [size=2M]
	Capabilities: <access denied>
	Kernel driver in use: xe
	Kernel modules: xe
```

(Designed capabilities require root on this system; session was unprivileged. Note the abort reported here is entirely userspace — in `libze_intel_gpu.so.1`.)

### Driver Version *

```
26.31.39395.13
```

(`clinfo` reports OpenCL Driver Version 26.31.39395; Level Zero implementation `libze_intel_gpu.so.1` from the same `intel-compute-runtime` package.)

### Installed GPU Driver Packages

```
$ pacman -Q | grep -iE 'igc|gmm|opencl|level-zero|ocloc|libze|compute-runtime|graphics-compiler|gmmlib|igsc|metrics'
intel-compute-runtime 26.31.39395.13-1.1
intel-gmmlib 22.10.0-1.1
intel-graphics-compiler 1:2.40.13-1.1
level-zero-headers 1.32.0-1
level-zero-loader 1.32.0-1
lib32-opencl-mesa 3:26.2.2-2
opencl-headers 2:2026.05.29-1
opencl-mesa 3:26.2.2-2
```

### Driver Installation Details *

```
- Installation method: Distribution repository (CachyOS / Arch Linux official repos)
- Installed via: pacman -S intel-compute-runtime level-zero-loader level-zero-headers
  (pulled in intel-graphics-compiler and intel-gmmlib as dependencies)
- No custom kernel parameters; kernel module `xe` shipped with the distro kernel
- No out-of-tree DKMS components
```

### Linux Distribution *

Selected: **Other Linux Distribution**

```
CachyOS (Arch-based, rolling) — NAME="CachyOS Linux", ID=cachyos, ID_LIKE=arch, BUILD_ID=rolling
```

### Kernel Version & Boot Parameters *

```
$ uname -r
7.2.3-1-cachyos

$ cat /proc/cmdline
quiet nowatchdog splash rw rootflags=subvol=/@ root=UUID=<redacted>

$ grep -E '^xe' /proc/modules
xe 4677632 47 - Live 0x0000000000000000
```

## Issue Description

### Actual Behavior *

Calling `zeEventPoolCreate` with a `ze_event_pool_desc_t` whose `count` field
is left at 0 (a zero-initialized descriptor — the invalid input; the
installed header documents `count` as "must be greater than 0") **aborts the
calling process** inside the driver:

```
Abort was called at 15 line in file:
/usr/src/debug/intel-compute-runtime/compute-runtime-26.31.39395.13/shared/source/gmm_helper/resource_info.cpp
```

This happens with `pd.flags = 0` and with `ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP`;
i.e. the abort is independent of the pool flags. The API call never returns,
so the application cannot recover.

### Expected Behavior *

An error return — `ZE_RESULT_ERROR_INVALID_SIZE` (or
`ZE_RESULT_ERROR_INVALID_ARGUMENT`) — per the Level Zero error-handling
contract: an invalid descriptor must not take down the calling process.
The invalid input itself is our bug (the count now lives in the descriptor;
older code passed it as the third `zeEventPoolCreate` argument, so migrated
code can leave `count` at 0); the report is about the failure mode.

### Reproduction Rate *

100% — every run of the reproducer (deterministic).

### Steps to Reproduce *

```
1. Save the reproducer below as pool0.cpp
2. g++ -O2 -std=c++17 -I /usr/include/level_zero pool0.cpp -o pool0 -lze_loader
3. ./pool0
4. Observe the SIGABRT message from resource_info.cpp:15 (exit code 134)
5. Change pd.count to 1, rebuild, rerun — zeEventPoolCreate succeeds and the
   program prints "unreachable" and exits 0
```

### Regression Information

Not known to be a regression — first observed on 26.31.39395.13; no older
version was systematically tested, so the checkbox is left unchecked.

## Diagnostics & Logs

### API Call Logs

(Tracer layer not used; this is the reproducer's own per-call logging. Every
call before `zeEventPoolCreate` returns `ZE_RESULT_SUCCESS`; the failing call
never returns.)

```
zeInit(ZE_INIT_FLAG_GPU_ONLY)          -> 0
zeDriverGet(&n, nullptr)               -> 0
zeDriverGet(&n, ds)                    -> 0
zeDeviceGet(ds[0], &m, nullptr)        -> 0
zeDeviceGet(ds[0], &m, dv)             -> 0
zeContextCreate(ds[0], &cd, &ctx)      -> 0
zeEventPoolCreate(ctx, &pd, 0, nullptr, &pool)
Abort was called at 15 line in file:
/usr/src/debug/intel-compute-runtime/compute-runtime-26.31.39395.13/shared/source/gmm_helper/resource_info.cpp
```

### strace Logs

Not attached — the failure is in userspace driver code (an explicit abort),
not in a syscall path; strace adds no information here.

### System Logs / dmesg Output

```
$ dmesg | grep -i -E 'xe |drm|gpu' | tail -n 20
(no relevant kernel messages — the abort is in userspace; no GPU reset,
 page fault, or driver error appears in the kernel log)
```

### Backtrace (if crash or hang occurred)

```
$ gdb ./pool0
(gdb) run
Program received signal SIGABRT, Aborted.
0x00007ffff76af2eb in pthread_kill () from /usr/lib/libc.so.6
#0  0x00007ffff76af2eb in pthread_kill () from /usr/lib/libc.so.6
#1  0x00007ffff7644b88 in raise () from /usr/lib/libc.so.6
#2  0x00007ffff76257a7 in abort () from /usr/lib/libc.so.6
#3  0x00007ffff38139a6 in ?? () from /usr/lib/libze_intel_gpu.so.1
#4  0x00007ffff390bf79 in ?? () from /usr/lib/libze_intel_gpu.so.1
#5  0x00007ffff420bec1 in ?? () from /usr/lib/libze_intel_gpu.so.1
#6  0x00007ffff421aabf in ?? () from /usr/lib/libze_intel_gpu.so.1
#7  0x00007ffff4148fef in ?? () from /usr/lib/libze_intel_gpu.so.1
#8  0x00007ffff3b03964 in ?? () from /usr/lib/libze_intel_gpu.so.1
#9  0x00007ffff3a2ddf0 in ?? () from /usr/lib/libze_intel_gpu.so.1
#10 0x000055555555525a in main ()
```

(Distro binary has stripped internal frames; the abort message itself pins the
exact source line: `shared/source/gmm_helper/resource_info.cpp:15`, i.e.
`UNRECOVERABLE_IF(resourceInfo->peekHandle() == 0)` in
`GmmResourceInfo::create`.)

## Reproducer Information

### Source Code / Reproducer

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

- Compilation command: `g++ -O2 -std=c++17 -I /usr/include/level_zero pool0.cpp -o pool0 -lze_loader`

### Command Line / Application Details

```
./pool0
```

Aborts (SIGABRT, exit 134) at `zeEventPoolCreate`. With `pd.count = 1` the
same binary path prints `unreachable` and exits 0.

## Additional Information

### oneAPI Version (if applicable)

Not used — the reproducer links only `libze_loader` and uses the distro
Level Zero headers.

### Screenshots / Video

Not applicable (console abort; terminal output captured above).

### Additional Notes

Mechanism (traced in the `26.31.39395.13` sources):

- `EventPool::create` (`level_zero/core/source/event/event.cpp:252`) does not
  validate `desc->count` before calling `initialize()`.
- `EventPool::initialize` (`event.cpp:42`) already returns
  `ZE_RESULT_ERROR_INVALID_ARGUMENT` for other invalid descriptor
  combinations (e.g. IPC combined with counter-based flags) — `count` is
  simply not among the checked fields.
- `initializeSizeParameters` (`event.cpp:226`) computes
  `eventPoolSize = alignUp(numEvents * eventSize, 64 KiB)`; with `count == 0`
  a zero-sized `AllocationProperties` (`event.cpp:91`) reaches the memory
  manager, GMM resource creation fails, and
  `UNRECOVERABLE_IF(resourceInfo->peekHandle() == 0)`
  (`shared/source/gmm_helper/resource_info.cpp:15`) aborts the process.
- Flags-independence follows from the same formula: any `eventSize * 0` is 0.

Suggested fix (matching the existing validation style in
`EventPool::initialize`): reject early, e.g.

```cpp
if (desc->count == 0) {
    result = ZE_RESULT_ERROR_INVALID_SIZE;
    return nullptr;
}
```

Header documentation of the constraint
(`/usr/include/level_zero/ze_api.h`, `ze_event_pool_desc_t`):
`uint32_t count; ///< [in] number of events within the pool; must be greater than 0`

Related: #922 reports the same abort line (`resource_info.cpp:15`) from a
different root cause (multi-rank `zeInit` race) — this abort site appears to
act as a generic backstop that turns many distinct root causes into an
opaque process abort.

Unrelated side observation (different component, mentioned only for
awareness): with `ZE_ENABLE_VALIDATION_LAYER=1` (level-zero-loader 1.32.0)
the same reproducer segfaults inside the validation layer even with the
valid `count = 1` configuration, so the layer could not be evaluated as a
guard on this stack.
