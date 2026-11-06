# ocloc atomic_and investigation — 2026-09-05

Environment: ocloc 26.31.39395, Intel Arc B580 (Battlemage/Xe2), `-device bmg`.
Command form: `env -u LD_LIBRARY_PATH ocloc compile -file X.cl -device bmg -output out`
(exit 245 in the shell = ocloc's "error code: -11" = generic BUILD FAILURE,
not a signal/SIGSEGV.)

## Finding 1 — the "static inline atomic_and -11 bug" DOES NOT EXIST

The historical "-11" was a parse error: the helper's half-word pointer was
named `half`, which is a RESERVED WORD in OpenCL C (the 16-bit float type).

- `reserved_word_half.cl` — reconstruction: `volatile __global u32* half` →
  `error: expected identifier or '('` → `Build failed with error code: -11`.
- `static_inline_ok.cl` — same kernel, variable renamed `w32` (as in the
  production `goldbach.cl`): **Build succeeded**. `static inline` +
  `volatile` cast + `atomic_and` compiles fine.

The production docs' rule "the atomic must stay spelled out per mark" is
unnecessary; factoring into a helper is fine if you don't name things `half`.

## Finding 2 — 64-bit atomic_and: overload missing, misleading diagnostic (REPORTABLE)

With `cl_khr_int64_base_atomics` + `cl_khr_int64_extended_atomics` enabled,
under every `-cl-std` (CL1.2/2.0/3.0):

- `bug64_ambiguous.cl` — exact-typed `atomic_and(volatile __global ulong*,
  ulong)` fails: "call to 'atomic_and' is ambiguous", with only the two
  32-bit candidates (`int*`, `uint*`) listed. The 64-bit overloads are
  absent from the header set; the error never says so. This is the origin
  of the project's 32-bit half-word workaround.
- `bug64_silent32.cl` — passing an int literal mask (`~(1 << (bit & 63))`)
  with a u64* pointer: **Build succeeded with only a warning** — the
  overload resolver silently selects the 32-bit `atomic_and(int*, int)`,
  truncating the mask and pointing a 32-bit atomic at a 64-bit word.
  A silent-wrong-semantic compile; arguably the more severe report.

## What to file with Intel (intel/intel-graphics-compiler)

1. Missing 64-bit atomic_and overload with both int64-atomics extensions
   enabled → misleading "ambiguous between int*/uint*" diagnostic
   (bug64_ambiguous.cl).
2. Silent selection of the 32-bit overload on incompatible pointer type,
   warning-only (bug64_silent32.cl).

Do NOT file the static-inline story — refuted, see Finding 1.
