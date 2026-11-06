# ocloc: `atomic_and` on `__global ulong*` with an int mask silently resolves to the 32-bit overload — warning-only build success, wrong-width atomic emitted

## Summary

When `atomic_and` is called on a `volatile __global ulong*` with an int-typed
mask expression, overload resolution silently selects the 32-bit built-in
`atomic_and(volatile __global int*, int)`: the `ulong*` argument is passed
through a pointer-type mismatch that is downgraded to a **warning**, and the
compiler emits a 32-bit atomic RMW on 64-bit data. `Build succeeded.`

The generated SPIR-V confirms the wrong-width operation: the mask is computed
in 32 bits, the `ulong*` pointer is `OpBitcast` to `uint*`, and the atomic is
`OpAtomicAnd %uint`. There is no error and no way to detect the semantic
change from the exit code.

## Environment

- ocloc 26.31.39395 (intel-compute-runtime 26.31.39395.13)
- Device target: `-device bmg` (Intel Arc B580 / Battlemage / Xe2)
- OS: CachyOS x86_64, kernel 7.2.3-1-cachyos
- Date tested: 2026-09-05
- Behavior identical with default options and with explicit
  `-options "-cl-std=CL1.2"`, `-cl-std=CL2.0"`, `-cl-std=CL3.0"`

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
        atomic_and(bitmap + (bit >> 6), ~(1 << (bit & 63)));
}
```

(Loop shape: clearing bit `bit` of a 64-bit-word bitmap — a segmented-sieve
mark loop. Any int-typed mask triggers the same resolution.)

Compile:

```sh
ocloc compile -file repro.cl -device bmg -output out
```

## Observed

```
1:7:20: warning: incompatible pointer types passing 'volatile __global u64 *'
    (aka 'volatile __global unsigned long *') to parameter of type
    'volatile __global int *'
    7 |         atomic_and(bitmap + (bit >> 6), ~(1 << (bit & 63)));
      |                    ^~~~~~~~~~~~~~~~~~~~~
./opencl-c.h:13146:46: note: passing argument to parameter here
 13146 | int __ovld atomic_and(volatile __global int *, int);
       |                                              ^

Build succeeded.
```

SPIR-V disassembly of the emitted kernel (`spirv-dis out_bmg.spv`):

```
   %shl_mask = OpBitwiseAnd %uint %sh_prom %uint_31      ; 'bit & 63' truncated to & 31
        %shl = OpShiftLeftLogical %uint %uint_1 %shl_mask
        %not = OpBitwiseXor %uint %shl %uint_4294967295  ; mask computed in 32 bits
         %35 = OpBitcast %_ptr_CrossWorkgroup_uint %add_ptr
       %call = OpAtomicAnd %uint %35 %uint_2 %uint_0 %not
```

## Expected

The call should not silently compile to a different-width atomic. Passing a
`volatile __global ulong*` where `volatile __global int*` is expected while
selecting an atomic built-in overload should be a hard error (the OpenCL C
built-in overload sets are meant to require type-exact matches), or — if the
64-bit overload is available — the call should resolve to
`atomic_and(volatile __global ulong*, ulong)`.

## Why this matters

An int-typed mask is the natural way to write this call, and the failure is
completely silent apart from a pointer-type warning that many builds ignore:
the kernel clears bit `bit & 31` of the *low 32 bits* of the wrong-width word
with a truncated mask. For any concurrent bitset/sieve-style use this is
silent data corruption, not a performance difference.

## Additional context

The exactly-typed form `atomic_and(bitmap + (bit >> 6), ~(1UL << (bit & 63UL)))`
does not compile either — it fails with `error: call to 'atomic_and' is
ambiguous`, listing only the `int*`/`uint*` candidates (no 64-bit overload is
offered despite both `cl_khr_int64_base_atomics` and
`cl_khr_int64_extended_atomics` being enabled). That is filed separately as a
companion issue; together they mean there is currently no correct-by-default
way to write a 64-bit `atomic_and` under this toolchain, and the most natural
fallback compiles to wrong code without an error.

## Notes

The compiler note itself (`opencl-c.h:13146`) documents which overload was
selected, which is how the silent resolution was identified.

---

## CORRECTIONS (2026-09-05 verification pass — apply as a body edit / comment on live #441)

Filed verbatim as intel/intel-graphics-compiler#441. All technical content
re-verified and holds: warning `1:7:20:` text verbatim, `Build succeeded.`
exit 0, and a fresh `spirv-dis` reproduces the quoted block byte-for-byte
(including `bit & 63` computed in ulong, `OpUConvert` to uint, then `& 31`).
Three statements in the filed body are now known WRONG:

1. **Expected, second alternative** ("or — if the 64-bit overload is
   available — the call should resolve to
   `atomic_and(volatile __global ulong*, ulong)`"): DROP. Per the OpenCL C
   spec tables / 1.2 reference pages, `atomic_*` atomics are 32-bit only;
   the 64-bit variants enabled by the int64-atomics extensions are spelled
   `atom_and` / `atom_add`. A 64-bit `atomic_and` overload should NOT exist.
   Replacement: "Note: the spec-conformant 64-bit entry point is the
   `atom_and` spelling — `atom_and(volatile __global ulong*, ulong)`, which
   compiles fine on this target. That makes the silent wrong-width fallback
   harder to defend: a correctly-spelled alternative exists."

2. **"That is filed separately as a companion issue"** — FALSE (the
   companion was withdrawn after verification; it was spec-conformant
   behavior, not a bug). Remove the sentence.

3. **"there is currently no correct-by-default way to write a 64-bit
   `atomic_and` under this toolchain"** — WRONG.
   `atom_and(bitmap + (bit >> 6), ~(1UL << (bit & 63UL)))` compiles on ocloc
   offline AND the online `clCreateProgramWithSource` path (no pragmas
   needed; macros predefined; B580 advertises both int64 extensions).
   Replacement paragraph for Additional context:
   "The exactly-typed form `atomic_and(..., ~(1UL << (bit & 63UL)))` fails
   with `error: call to 'atomic_and' is ambiguous`, listing only the
   `int*`/`uint*` candidates. Per the spec that part is conforming — the
   64-bit variants enabled by cl_khr_int64_{base,extended}_atomics are
   spelled `atom_and`/`atom_add`, and
   `atom_and(volatile __global ulong*, ulong)` compiles fine on this
   target. The diagnostic never hints at this, and the natural fallback
   (the int-mask call above) silently compiles to a 32-bit RMW — the actual
   hazard this issue reports."

Also fix the Environment typo: `` `-cl-std=CL2.0"` `` is missing its opening
quote (should be `` `"-cl-std=CL2.0"` ``).

Optional softening in Expected: "the OpenCL C built-in overload sets are
meant to require type-exact matches" overstates the spec — implicit
conversions do apply to built-in arguments; the pointer mismatch is a
constraint violation requiring *a* diagnostic (the ask: make it an error).
