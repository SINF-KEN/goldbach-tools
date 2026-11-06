// IGC bug reproducer: ocloc exits with signal 11 (SIGSEGV, exit code -11)
// when an atomic_and on __global memory sits inside a `static inline`
// helper function.  The same atomic, spelled directly in the kernel body,
// compiles fine (see control_d.cl).
//
// Test:  ocloc compile -file repro.cl -device bmg -output out
// ocloc 26.31.39395 / Intel Arc B580 (Battlemage / Xe2).

typedef unsigned int  u32;
typedef unsigned long u64;

// ------------------------- the trigger -------------------------------------
static inline void clear_bit(volatile __global u64* words, u64 bit)
{
    volatile __global u32* w32 =
        (volatile __global u32*)(words + (bit >> 6));
    atomic_and(w32 + ((bit >> 5) & 1UL), ~(1u << (bit & 31u)));
}
// ---------------------------------------------------------------------------

// Generic bitmap sieve: clear every `stride`-th bit starting at `first`
// (the shape of a segmented sieve marking multiples of an odd prime).
__kernel void mark_composites(volatile __global u64* restrict bitmap,
                              u64                 first,
                              u64                 stride,
                              u64                 nbits)
{
    for (u64 bit = first; bit < nbits; bit += stride)
        clear_bit(bitmap, bit);
}
