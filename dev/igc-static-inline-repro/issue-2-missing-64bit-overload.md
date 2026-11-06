# VERDICT 2026-09-05 (verification pass): NOT A VALID BUG — DO NOT FILE

The drafted report below reproduces exactly as written — ocloc offline AND
the online `clCreateProgramWithSource` path produce the identical "ambiguous"
error, under default options and `-cl-std=CL1.2/2.0/3.0` — but its "Expected"
section misstates the OpenCL C specification:

- Per the official OpenCL 1.2 reference pages, `atomic_add`/`atomic_and` are
  **32-bit only**. The 64-bit variants enabled by
  `cl_khr_int64_base_atomics` / `cl_khr_int64_extended_atomics` are spelled
  **`atom_add` / `atom_and`** ("A 64-bit version of this function,
  atom_add(3clc), is enabled by cl_khr_int64_base_atomics(3clc)" —
  https://manpages.debian.org/testing/opencl-1.2-man-doc/atomic_add.3clc.en.html).
  Only the 32-bit extensions were renamed to `atomic_*` when promoted to
  OpenCL C 1.1 core; the int64 extensions kept the `atom_*` spelling.
- `atom_and(volatile __global ulong*, ulong)` **compiles** on
  `ocloc -device bmg` — even with NO `#pragma OPENCL EXTENSION` at all (the
  guard macros are predefined for this target, and clinfo confirms the B580
  device advertises both int64-atomics extensions).
- Root cause of the "ambiguous" error is stock upstream clang's `opencl-c.h`
  (verified identical in llvm release/22.x and
  `/usr/lib/clang/22/include/opencl-c.h`): the int64-guarded overloads exist
  only under the `atom_*` spellings (`atom_add` ulong at :12914, `atom_and`
  ulong at :13166) — which matches the spec. `atomic_add(ulong*)` fails the
  same way (verified).

Residual (diagnostic quality only, not conformance): the "call to
'atomic_and' is ambiguous" diagnostic listing two 32-bit candidates gives no
hint that the 64-bit entry point is spelled `atom_and`. If anything, mention
this as a side note in a comment on #441 — together with the real workaround
for #441: write the 64-bit clear as `atom_and(ptr, ~(1UL << (bit & 63UL)))`,
which type-checks exactly and needs no int-literal mask.

---

## Original draft (superseded — kept for the record)

# ocloc: no 64-bit atomic_and overload with both int64-atomics extensions enabled — exact-typed call fails with a misleading "ambiguous" error

## Summary

With `cl_khr_int64_base_atomics` and `cl_khr_int64_extended_atomics` both
enabled (pragmas accepted without any "unsupported extension" warning), the
64-bit `atomic_and` overloads do not exist in the OpenCL C header set. An
exactly-typed call — `volatile __global ulong*` pointer, `ulong` mask — fails
to compile with `error: call to 'atomic_and' is ambiguous`, and the candidate
list names only the two 32-bit overloads. The diagnostic never mentions that
no 64-bit overload is available, which is the actual problem.

## Environment

- ocloc 26.31.39395 (intel-compute-runtime 26.31.39395.13)
- Device target: `-device bmg` (Intel Arc B580 / Battlemage / Xe2)
- OS: CachyOS x86_64, kernel 7.2.3-1-cachyos
- Date tested: 2026-09-05
- Behavior identical with default options and with explicit
  `-options "-cl-std=CL1.2"`, `"-cl-std=CL2.0"`, `"-cl-std=CL3.0"`

## Steps to reproduce

`repro.cl`:

```c
#pragma OPENCL EXTENSION cl_khr_int64_base_atomics : enable
#pragma OPENCL EXTENSION cl_khr_int64_extended_atomics : enable

typedef unsigned long u64;

__kernel void mark(volatile __global u64* restrict bitmap,
                   u64 first, u64 stride, u64 nbits)
{
    for (u64 bit = first; bit < nbits; bit += stride)
        atomic_and(bitmap + (bit >> 6), ~(1UL << (bit & 63UL)));
}
```

Compile:

```sh
ocloc compile -file repro.cl -device bmg -output out
```

## Observed

```
1:7:9: error: call to 'atomic_and' is ambiguous
    7 |         atomic_and(bitmap + (bit >> 6), ~(1UL << (bit & 63UL)));
      |         ^~~~~~~~~~
./opencl-c.h:13146:12: note: candidate function
 13146 | int __ovld atomic_and(volatile __global int *, int);
       |            ^
./opencl-c.h:13147:13: note: candidate function
 13147 | uint __ovld atomic_and(volatile __global uint *, uint);
       |             ^

Build failed with error code: -11
```

No 64-bit candidate appears in the list, and no diagnostic says the 64-bit
overloads are unavailable or that the extensions were not honored.
