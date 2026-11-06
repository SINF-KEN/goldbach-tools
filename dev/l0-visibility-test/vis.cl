// vis.cl — kernels for the L0 store->atomic visibility testcase.
// k_fill: plain volatile whole-word global stores (stands in for the tile
//         sieve's whole-word map stores)
// k_atomic: one atomic_and RMW per word (stands in for the large-prime
//         sieve's mark-clearing atomics); the mask keeps it a real RMW the
//         compiler cannot elide.
typedef unsigned int uint_t;

__kernel void k_fill(volatile __global uint_t* buf, const uint_t n, const uint_t pat)
{
    size_t i = get_global_id(0);
    if (i < n) buf[i] = pat;
}

__kernel void k_atomic(volatile __global uint_t* buf, const uint_t n, const uint_t mask)
{
    size_t i = get_global_id(0);
    if (i < n) atomic_and(buf + i, mask);
}

// Production-shaped variant: fill stages the pattern in SLM and emits
// whole-u64-word global stores (like make_prime_map), then a second kernel
// clears bits via 32-bit atomic_and on the containing half-words (like
// sieve_large_primes).
typedef unsigned long ulong_t;

__kernel void k_fill_slm64(volatile __global ulong_t* buf, const uint_t nwords, const ulong_t pat)
{
    __local ulong_t tile[256];
    tile[get_local_id(0)] = pat;
    barrier(CLK_LOCAL_MEM_FENCE);
    size_t i = get_global_id(0);
    if (i < nwords) buf[i] = tile[get_local_id(0)];
}

__kernel void k_atomic_half(volatile __global ulong_t* buf, const uint_t nwords, const uint_t mask)
{
    size_t i = get_global_id(0);
    if (i < nwords) {
        volatile __global uint_t* w32 = (volatile __global uint_t*)(buf + i);
        atomic_and(w32, mask);
        atomic_and(w32 + 1, mask);
    }
}
