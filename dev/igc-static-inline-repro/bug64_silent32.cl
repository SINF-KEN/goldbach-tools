#pragma OPENCL EXTENSION cl_khr_int64_base_atomics : enable
#pragma OPENCL EXTENSION cl_khr_int64_extended_atomics : enable
typedef unsigned long u64;
__kernel void mark(volatile __global u64* restrict bitmap, u64 first, u64 stride, u64 nbits)
{
    for (u64 bit = first; bit < nbits; bit += stride)
        atomic_and(bitmap + (bit >> 6), ~(1 << (bit & 63)));
}
