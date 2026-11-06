# intel/compute-runtime "[Bug Report] Linux GPU Driver Issue" — form fill
# Prepared 2026-09-05. Paste each section into the matching form field.
# Full prose issue text: issue-draft.md (same directory).

## Title (*)

OpenCL: clBuildProgram of a multi-kernel program corrupts host heap — process aborts with glibc "invalid pointer" after the first kernel run (Arc B580, 100% reproducible)

## GPU Hardware (*)

Intel Arc B580 (Battlemage G21, ASRock), PCI 0000:03:00.0

## DRI Devices Information (*)

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
```

(The affected Arc B580 is 0000:03:00.0 → card0/renderD128. The second
controller at 0000:0e:00.0 is the AMD iGPU.)

## GPU Detailed Information (lspci output) (*)

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

(Capabilities truncated — unprivileged lspci. Re-run under sudo for the
full dump if needed.)

## Driver Version (*)

26.31.39395 (package intel-compute-runtime 26.31.39395.13-1.1; clinfo:
Driver Version 26.31.39395, OpenCL 3.0 NEO)

## Installed GPU Driver Packages

```
$ pacman -Q | grep -iE "igc|gmm|opencl|level-zero|ocloc|compute|clinfo|libze"
clinfo 3.0.25.02.14-1.1
intel-compute-runtime 26.31.39395.13-1.1
intel-graphics-compiler 1:2.40.13-1.1
intel-gmmlib 22.10.0-1.1
level-zero-headers 1.32.0-1
level-zero-loader 1.32.0-1
ocl-icd 2.3.5-1.1
opencl-headers 2:2026.05.29-1
opencl-mesa 3:26.2.2-2
```

(ocloc ships inside the intel-compute-runtime package: /usr/bin/ocloc.)

## Driver Installation Details (*)

- Installation method: Distribution repository (CachyOS, Arch-based)
- `sudo pacman -S intel-compute-runtime intel-graphics-compiler` (pulled
  in intel-gmmlib; ocl-icd as the ICD loader)
- No custom kernel parameters, no out-of-tree driver builds, no PPA/Intel repo

## Linux Distribution (*)

Other → CachyOS (Arch-based, rolling), fully updated as of 2026-09-05

## Kernel Version & Boot Parameters (*)

```
$ uname -r
7.2.3-1-cachyos

$ cat /proc/cmdline
quiet nowatchdog splash rw rootflags=subvol=/@ root=UUID=<redacted>

$ grep -E '^(xe|i915) ' /proc/modules
xe 4677632 49 - Live 0x0000000000000000
```

(`xe` in use for the B580; i915 not loaded. No custom GPU-related boot
parameters.)

## Actual Behavior (*)

A program that builds a 10-kernel OpenCL program in-process
(clCreateProgramWithSource + clBuildProgram) and then executes ONE
trivial kernel aborts in the first kernel-run block — at the read-back /
clReleaseMemObject / clReleaseKernel calls, or the first heap operation
right after them — with glibc heap-corruption errors:

munmap_chunk(): invalid pointer   (SIGABRT, exit 134)

Every OpenCL call before the abort returns CL_SUCCESS and the kernel's
read-back values are 100% correct — this is host heap corruption, not
wrong code. Under AddressSanitizer the same block is a SEGV
("unknown-crash"): an 8-byte READ of a stack local whose shadow bytes
are marked addressable, i.e. process state was corrupted earlier by
uninstrumented driver code, most plausibly during the build path.

Fault is on the Intel compute-runtime path: with the Mesa rusticl ICD
excluded from the loader (OCL_ICD_VENDORS=/etc/OpenCL/vendors/intel.icd)
the reproducer still aborts 5/5.

The corruption is heap-layout sensitive: minimized variants of the same
program run clean (0/11) — the FULL attached source must be used
verbatim; a sibling program building the same kernel shapes from a
different host allocation pattern runs clean (0/10); removing the u64
div/rem kernels keeps the 100% abort rate (5/5); a 2-kernel program with
the same host structure is clean over dozens of runs.

## Expected Behavior (*)

The program should run the full builtin sweep, print
"ALL 64-BIT BUILTINS CORRECT (0 mismatches)", release all objects, and
exit 0. No OpenCL error is returned at any step before the abort (every
call returns CL_SUCCESS; the blocking read-back completes with correct
data).

## Reproduction Rate (*)

100% on this system with the attached source: 11/11 runs of one binary,
21/21 across rebuilds, 26/26 including a rebuild from a different
directory using exactly the documented commands — 36/36 total, including
10 runs in the reporter's own terminal (one batch of five under a
controlled uptime window to check for kernel-log side effects: none).

## Steps to Reproduce (*)

1. Save the ATTACHED builtin64.cpp verbatim (exact source required — the
   bug is heap-layout sensitive and minimized variants do NOT reproduce)
2. g++ -O2 -g -std=c++17 builtin64.cpp -o builtin64 -lOpenCL
3. ./builtin64
   → prints "build OK", runs the first kernel and reads it back
     successfully (values verified correct), then aborts with
     "munmap_chunk(): invalid pointer" (SIGABRT, exit 134)
4. Re-running the same binary aborts every time
5. Rebuild with -fsanitize=address: SIGSEGV "unknown-crash" in the same
   block (8-byte read of an addressable stack local)

Observed output (step 3):

```
build OK
S1 start
S1 buf ok
  run2 k_clz: create kernel
  run2 k_clz: create ob
  run2 k_clz: set args
  run2 k_clz: enqueue
  run2 k_clz: read
  run2 k_clz: release
  run2 k_clz: done
munmap_chunk(): invalid pointer
```

## Regression Information

Leave "Is this a regression?" UNCHECKED — older driver versions were not
tested. First observed on 26.31.39395.13.

## API Call Logs

Not captured (OpenCL API tracing not enabled). All calls up to the abort
return CL_SUCCESS; a trace would show nothing but successes until the
glibc abort fires outside any API error path.

## strace Logs

Not captured; a gdb backtrace is provided below instead.

## System Logs / dmesg Output

```
$ sudo dmesg | grep -iE 'xe|drm'
[21293.706580] amdgpu 0000:0e:00.0: [drm] PCIE GART of 1024M enabled (table at 0x000000F41FC00000).
[21293.800100] amdgpu 0000:0e:00.0: [drm] DMUB hardware initialized: version=0x05003300
[21293.805852] amdgpu 0000:0e:00.0: [drm] Cannot find any crtc or sizes
[21293.805861] amdgpu 0000:0e:00.0: [drm] Cannot find any crtc or sizes
[21297.732055] mei_lb xe.mei-gscfi.768-e2c2afa2-3817-4d19-9d95-06b16b588a5d: bound 0000:03:00.0 (ops xe_late_bind_component_ops [xe])
[22944.761050] amdgpu 0000:0e:00.0: [drm] PCIE GART of 1024M enabled (table at 0x000000F41FC00000).
[22944.853488] amdgpu 0000:0e:00.0: [drm] DMUB hardware initialized: version=0x05003300
[22944.858105] amdgpu 0000:0e:00.0: [drm] Cannot find any crtc or sizes
[22944.858116] amdgpu 0000:0e:00.0: [drm] Cannot find any crtc or sizes
[22948.783747] mei_lb xe.mei-gscfi.768-e2c2afa2-3817-4d19-9d95-06b16b588a5d: bound 0000:03:00.0 (ops xe_late_bind_component_ops [xe])
[57172.131426] xe 0000:03:00.0: [drm] Tile0: GT0: log buffer overflow
[57172.131470] xe 0000:03:00.0: [drm] Tile0: GT0:
[57172.131475] xe 0000:03:00.0: [drm] Tile0: GT0: Fault response: Unsuccessful -ENOENT
[57716.597913] xe 0000:03:00.0: [drm] Tile0: GT0:
[57716.597920] xe 0000:03:00.0: [drm] Tile0: GT0: Fault response: Unsuccessful -ENOENT
```

(The amdgpu lines are the second GPU in the system, an AMD iGPU at
0000:0e:00.0, resuming from suspend — unrelated.)

Interpretation and honest caveat: the two `xe 0000:03:00.0 ... Fault
response: Unsuccessful -ENOENT` events occurred during the same session
and time window as the reproducer runs (kernel timestamps corrected for
suspend place them within ±30 min of the repro batches). They are
consistent with a GPU page fault whose fault-handling response could not
be delivered because the target context/exec queue was already torn
down — i.e., a process dying while GPU work or fault handling is in
flight, which is exactly the reproducer's SIGABRT scenario. There are NO
device-reset / GT-hang / firmware-error messages. Per-run correlation
was not confirmed (no dmesg check immediately after an individual run).
These kernel events do NOT explain the reported corruption — the host
heap corruption is proven host-side (glibc allocator metadata + ASan
fault on the host stack) and stands independently.

Follow-up controlled test (2026-09-06): five consecutive reproducer runs
(uptime window 126804.15 → 126807.19, five SIGABRTs) produced ZERO new
kernel fault entries. A third identical fault ([59894]) appeared once
during the earlier session and never again across 10+ subsequent runs.
No per-run causal link between the crashes and these fault events could
be established; the host-side corruption evidence stands on its own.

## Backtrace (if crash or hang occurred)

```
$ gdb ./builtin64
(gdb) run
...
Thread 1 "builtin64" received signal SIGABRT, Aborted.
#0  pthread_kill () from /usr/lib/libc.so.6
#1  raise () from /usr/lib/libc.so.6
#2  abort () from /usr/lib/libc.so.6
#3  malloc_printerr () from /usr/lib/libc.so.6     # "munmap_chunk(): invalid pointer"
#4-#6 glibc free/munmap_chunk internals
#7+ frames above are corrupted garbage — consistent with the heap
    corruption glibc is reporting

All driver worker threads idle at abort time, e.g.:
Thread 5 "b:clctxworker0" ... pthread_cond_wait () from libigdrcl.so
(no NEO thread is executing during the abort — the corruption happened
earlier, most plausibly during clBuildProgram)

Under -fsanitize=address (separate build):
==41796==ERROR: AddressSanitizer: unknown-crash ... READ of size 8 ...
Address is located in stack of thread T0 ... 'ob' ...
shadow byte 00 (addressable) — i.e. a valid-looking stack object read
through a corrupted pointer/path.
```

## Source Code / Reproducer

- Attached: builtin64.cpp — FULL source, use verbatim (10 kernels:
  clz/ctz/popcount/mul_hi/mad_hi/u64 div/rem/shl/shr/sshr; runs k_clz
  first; aborts deterministically). Minimized variants do NOT reproduce.
- Compilation command: g++ -O2 -g -std=c++17 builtin64.cpp -o builtin64 -lOpenCL
- Also attached on request: builtin64_nodiv.cpp (8-kernel crasher, no
  u64 div/rem), m_k6.cpp (clean sibling, same kernel shapes, different
  host layout), merge_test.cpp (clean 2-kernel base)

## Command Line / Application Details

./builtin64

(no options; GPU idle during all runs, nothing else running)

## oneAPI Version (if applicable)

Not used — plain g++ + Khronos OpenCL headers (opencl-headers
2:2026.05.29-1) against the ocl-icd loader.

## Screenshots / Video

N/A (terminal abort; console transcript included above).

## Additional Notes

- The corrupting write was not directly observed (no valgrind on this
  system); the evidence is the deterministic abort signature, the ASan
  SEGV reading a valid stack local, the clean layout-differential
  siblings, and the correctness of all read-back data.
- Within one build the abort point is identical every run, which should
  make the corrupting write easy to catch with internal instrumentation.
- Workaround that avoids the issue: offline compile (ocloc) +
  clCreateProgramWithBinary/SPIR-V — our long-running production
  workload uses that path without any such abort (not a like-for-like
  claim for this reproducer — disclosed).
- Only the Linux/online-compile path was tested; Windows, ocloc/AOT for
  this reproducer, Level Zero, and non-Arc Intel GPUs were not.
- Duplicate scan: no matching open or closed issue found in
  intel/compute-runtime or intel/intel-graphics-compiler (searched
  "clBuildProgram heap corruption", "invalid pointer clRelease",
  "munmap_chunk") as of 2026-09-05.
