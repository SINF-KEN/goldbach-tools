# OpenCL host-state corruption around clBuildProgram — B580, CR 26.31.39395.13

**FILED 2026-09-06 as [intel/compute-runtime#991](https://github.com/intel/compute-runtime/issues/991)**
(reproducer attached there as builtin64.cpp). Closed after triage traced it to
the reproducer; see the re-verification below.

Found 2026-09-05 during a builtins-correctness sweep. **Not related to our
production pipeline** (ocloc/AOT path, different program shapes) — pure
upstream-hunt artifact.

## The bug

A program that (1) builds a multi-kernel OpenCL program in-process via
`clCreateProgramWithSource` + `clBuildProgram`, then (2) runs ONE trivial
kernel and reads it back, **aborts at the subsequent `clReleaseMemObject`/
`clReleaseKernel`** with glibc heap-corruption errors (`munmap_chunk():
invalid pointer` / `free(): invalid pointer`, SIGABRT exit 134). Under ASan
the same point is a SIGSEGV ("unknown-crash") reading a *valid* stack local
at the release call — i.e. process state (heap and/or stack) was corrupted
earlier, most plausibly during the build/enqueue path of the uninstrumented
driver, and the abort surfaces at the next release of the affected chunk.

The data read back is 100% CORRECT — this is not wrong-code, it is
host-state corruption.

## Files

- `builtin64_test.cpp` — **the deterministic reproducer** (11/11 crashes:
  3x original, 5x rerun unchanged, 3x `-nodiv` variant). Builds 8–10
  kernels (clz/ctz/popcount/mul_hi/mad_hi/shl/shr/sshr ± u64 div/rem),
  runs one clz kernel over 15 edge inputs, aborts at its first
  `clReleaseMemObject`.
- `builtin64_nodiv.cpp` — same with u64 div/rem kernels removed: **still
  crashes 3/3** (division exonerated).
- `m_k6.cpp` — near-identical sibling: same 8 kernel shapes built, same
  clz kernel run first (plus a t_add32 run), from a different host
  structure — **0/5 crashes**. Heap-layout sensitivity: the driver's OOB
  write lands on a victim chunk only in certain layouts.
- `merge_test.cpp` — 2-kernel base program: 0 crashes ever (dozens of runs).

## Evidence matrix

| Program | Kernels built | Runs | Result |
|---|---|---|---|
| builtin64_test.cpp | 10 | 3 | crash 3/3 (free/munmap_chunk invalid pointer, exit 134) |
| builtin64_test.cpp (rerun unchanged) | 10 | 5 | crash 5/5 — deterministic |
| builtin64_nodiv (no u64 div/rem) | 8 | 3 | crash 3/3 |
| builtin64 under ASan | 10 | 1 | SIGSEGV "unknown-crash" at the release line, reading valid stack local |
| merge_div (adds u64 div+rem, never run) | 4 | 5 | crash 1/5 (intermittent!) |
| merge_test (2 kernels) | 2 | many | 0 crashes |
| m_k6 (same 8 kernel shapes, other host layout) | 8 | 5 | 0 crashes |
| single-kernel-add variants (ctz/popcount/mul_hi/madhi/shl/sshr) | 3 | 5 each | 0 crashes |

Interpretation: corruption probability rises with compile volume; the
provided reproducer hits it deterministically on this system. No single
builtin is the trigger (nodiv still crashes; single-adds are clean).

## Run

```sh
g++ -O2 -g -std=c++17 builtin64_test.cpp -o builtin64 -lOpenCL
./builtin64          # expect: build OK → first kernel OK → abort at release
# ASan variant shows SIGSEGV at the clReleaseMemObject line instead
```

## Environment

- Intel Arc B580 (Battlemage G21, 8086:e20b), kernel `xe`, kernel 7.2.3-1-cachyos
- intel-compute-runtime 26.31.39395.13-1.1, intel-graphics-compiler 1:2.40.13-1.1
- CachyOS (Arch-based), OpenCL via ocl-icd `libOpenCL.so.1`
- Online path: clCreateProgramWithSource + clBuildProgram (in-process IGC)
- GPU idle during all runs; `env -u LD_LIBRARY_PATH` (ZCode shell quirk, unrelated)

## Caveats (disclosed up front)

- Exact-source sensitive: semantically identical programs with different
  host-side layout do not crash. This is expected for an OOB-write class
  bug and is why the sibling `m_k6.cpp` is included as the non-crashing
  differential pair.
- The corrupting write itself was not directly observed (no valgrind on
  this system); evidence is the deterministic abort signature + ASan SEGV
  at the release call + correctness of all readback data.
- Not tested: Windows, older drivers, ocloc/AOT path, L0 path.

## Re-verification 2026-09-05 (verify2/) — full independent re-run

Fresh rebuild from these sources, same compiler/flags as the issue. All
artifacts (sources, binaries, logs) in `verify2/`.

| Program | Kernels | Fresh runs | Result |
|---|---|---|---|
| builtin64_test.cpp (exact issue reproducer) | 10 | 11 | abort 11/11 (munmap_chunk, exit 134) |
| builtin64, Intel-ICD-only (`OCL_ICD_VENDORS=intel.icd`) | 10 | 5 | abort 5/5 — Rusticl never loaded; fault is NEO's. ASan leak trace also shows libigdrcl.so |
| builtin64 under ASan | 10 | 1 | unknown-crash: 8-byte READ at a stack local, shadow 00 (addressable), inside live run2 frame — at line 92 (read-back `out.data()`), NOT the release line in this build |
| builtin64_nodiv.cpp | 8 | 5 | abort 5/5 (div/rem still exonerated) |
| **issue-text abridged reconstruction** (verify2/issue_abridged.cpp) | 8 | **11** | **CLEAN 11/11** — the issue's abridged snippet does NOT reproduce |
| m_k6.cpp (sibling) | 8 | 5 | clean 5/5 (cumulative 0/10) |
| merge_test.cpp | 2 | 10 | clean 10/10 |
| merge_div reconstruction (verify2/merge_div.cpp) | 4 | 10 | clean 10/10 — original 1/5-crash variant was never preserved; claim not re-verifiable, DROPPED from issue-draft |

Key findings vs the original draft (issue-draft.md corrected accordingly):

1. **The abridged reproducer as printed in the issue does not crash** —
   full source must be attached and used verbatim; minimization destroys
   the reproduction (consistent with layout sensitivity).
2. **Abort surface point is layout-dependent even across rebuilds**: this
   rebuild's last stdout is `run2 k_clz: done` (5/5) — the first release
   pair completes and glibc aborts at the next heap operation; ASan fires
   at line 92 instead of the release line. Same class, same determinism.
   Draft wording softened from "at the clReleaseMemObject call" to "in the
   first kernel-run block".
3. "no valgrind on this system" re-checked: correct, still not installed.
4. Determinism claim re-confirmed: 21/21 aborts across three batches.
5. Correlation test vs kernel log (user-run sudo, 09-06): 5 more aborts
   (36/36 total for this source incl. the user's own terminal) inside
   uptime 126804→126807, ZERO new dmesg entries. The pre-existing
   `xe ... Fault response: Unsuccessful -ENOENT` lines (~18 h old,
   desktop display noise) are NOT correlated. dmesg field in
   github-form-filled.md updated to state this as verified fact.


## Resolution (2026-09-22)

Intel triage reproduced, traced, and refuted: **the corruption was in the
reproducer, not the driver.** Four defects: (1) run2/run3 lambdas deduce
`int` via the CK macro's `return 1` and fall off the end on success — UB,
the deterministic "heap corruption"; (2) k_mad_hi needs 4 args, run3 bound
3 (silent CL_INVALID_KERNEL_ARGS, discarded); (3) k_sshr dispatches N3=960
work-items over a 15-element buffer — a real GPU OOB read in the harness,
likely the dmesg ENOENT source; (4) ref_mad_hi computed high64(a*b+c)
instead of mul_hi(a,b)+c.

`builtin64_fixed.cpp` = the four fixes applied (original kept verbatim as
the #991 attachment). On B580 + 26.35.39758.10: `-Werror=return-type`
clean, 3/3 exit 0, **ALL 64-BIT BUILTINS CORRECT (0 mismatches)**.

Keep builtin64_fixed.cpp as the working 64-bit builtin validator.
Lesson: a "deterministic driver bug" whose reproducer emits
-Wreturn-type warnings is a reproducer bug first.

## Re-verification 2026-09-24 (post-closure)

Full chain re-run on B580 + 26.35.39758.10, fresh /tmp/rv991 build, issue
compile flags (`g++ -O2 -g -std=c++17 -lOpenCL`):

- **orig verbatim:** build log carries `warning: control reaches end of
  non-void function [-Wreturn-type]` at lines 96/107 (the UB, named by the
  compiler); crash **3/3** (exit 134, `munmap_chunk(): invalid pointer`) —
  the filed symptom is real and honest.
- **single-fix (NEW — the causal link, previously untested):** ONLY the
  lambda returns fixed (4-line diff: `-> int` + `return 0;` on run2/run3),
  defects 2–4 left in → **NO crash 3/3** (runs to completion; exit 1 from
  the remaining harness defects exactly as Intel described: 1×
  `OPENCL ERROR` = k_mad_hi CL_INVALID_KERNEL_ARGS, 753 mismatch lines =
  stale mad_hi vs wrong ref + OOB sshr reads). Removing only the UB
  removes only the crash.
- **fixed (all 4):** 3/3 exit 0, `ALL 64-BIT BUILTINS CORRECT
  (0 mismatches)`, `-Werror=return-type` clean.
- **#990 side re-check:** `zeEventPoolCreate(count=0)` → `0x78000008`
  (ZE_RESULT_ERROR_INVALID_SIZE — the exact code the report proposed), no
  abort, exit 0 (2/2).

Conclusion: closing #991 as a reproducer bug is CORRECT — keep it closed.
Artifacts in /tmp/rv991 (volatile; regenerate from this recipe if needed).
