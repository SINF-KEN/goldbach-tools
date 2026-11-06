__kernel void foo(__global uint* out, uint flag) {
    if (flag) {
        __local uint scratch[64];
        scratch[get_local_id(0) & 63u] = get_global_id(0);
        barrier(CLK_LOCAL_MEM_FENCE);
        out[get_global_id(0)] = scratch[get_local_id(0) & 63u];
    }
}
