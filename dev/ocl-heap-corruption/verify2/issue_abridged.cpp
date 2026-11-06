// Faithful reconstruction of the ABRIDGED reproducer as printed in the issue
// text: 8 kernels (clz/ctz/popcount/mul_hi/mad_hi/shl/shr/sshr), run ONE clz
// kernel over 15 edge inputs, verify readback, then release.
#include <CL/cl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

static const char* SRC = R"CLC(
__kernel void k_clz(__global const ulong* in, __global ulong* out){
    ulong x = in[get_global_id(0)]; if (x) out[get_global_id(0)] = clz(x); else out[get_global_id(0)] = 0xDEAD;
}
__kernel void k_ctz(__global const ulong* in, __global ulong* out){
    ulong x = in[get_global_id(0)]; if (x) out[get_global_id(0)] = ctz(x); else out[get_global_id(0)] = 0xDEAD;
}
__kernel void k_popcount(__global const ulong* in, __global ulong* out){
    out[get_global_id(0)] = popcount(in[get_global_id(0)]);
}
__kernel void k_mul_hi(__global const ulong* a, __global const ulong* b, __global ulong* out){
    out[get_global_id(0)] = mul_hi(a[get_global_id(0)], b[get_global_id(0)]);
}
__kernel void k_mad_hi(__global const ulong* a, __global const ulong* b, __global const ulong* c, __global ulong* out){
    out[get_global_id(0)] = mad_hi(a[get_global_id(0)], b[get_global_id(0)], c[get_global_id(0)]);
}
__kernel void k_shl(__global const ulong* a, __global const ulong* s, __global ulong* out){
    out[get_global_id(0)] = a[get_global_id(0)] << (s[get_global_id(0)] & 63);
}
__kernel void k_shr(__global const ulong* a, __global const ulong* s, __global ulong* out){
    out[get_global_id(0)] = a[get_global_id(0)] >> (s[get_global_id(0)] & 63);
}
__kernel void k_sshr(__global const long* a, __global const ulong* s, __global long* out){
    out[get_global_id(0)] = a[get_global_id(0)] >> (s[get_global_id(0)] & 63);
}
)CLC";

#define CK(x) do { cl_int e_=(x); if(e_!=CL_SUCCESS){ printf("OPENCL ERROR %d at %s\n",e_,#x); return 1;} } while(0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    uint64_t edgeInputs[15] = {
        1, 2, 3, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFF00000001ull,
        0x8000000000000000ull, 0x8000000000000001ull, 0xFFFFFFFFFFFFFFFEull,
        0xFFFFFFFFFFFFFFFFull, 0x123456789ABCDEFull, 0xFEDCBA9876543210ull,
        0x5555555555555555ull, 0x00000000FFFFFFFFull, 0x0000000100000000ull
    };
    uint64_t out[15];

    // ... clCreateContext / clCreateCommandQueue / clCreateProgramWithSource ...
    cl_platform_id plats[4]; cl_uint np = 0; CK(clGetPlatformIDs(4, plats, &np));
    cl_platform_id intel = nullptr;
    for (cl_uint i = 0; i < np; i++) {
        char nm[256] = {}; clGetPlatformInfo(plats[i], CL_PLATFORM_NAME, sizeof nm, nm, nullptr);
        if (strstr(nm, "Intel")) { intel = plats[i]; break; }
    }
    cl_device_id dev; CK(clGetDeviceIDs(intel, CL_DEVICE_TYPE_GPU, 1, &dev, nullptr));
    cl_int err; cl_context ctx = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &err); CK(err);
    cl_command_queue q = clCreateCommandQueue(ctx, dev, 0, &err); CK(err);
    const char* s = SRC;
    cl_program prog = clCreateProgramWithSource(ctx, 1, &s, nullptr, &err); CK(err);
    clBuildProgram(prog, 1, &dev, nullptr, nullptr, nullptr);          // succeeds
    printf("build OK\n");

    cl_mem in = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                               15 * 8, edgeInputs, nullptr);
    cl_mem ob = clCreateBuffer(ctx, CL_MEM_READ_WRITE, 15 * 8, nullptr, nullptr);
    cl_kernel k = clCreateKernel(prog, "k_clz", nullptr);
    clSetKernelArg(k, 0, sizeof(cl_mem), &in);
    clSetKernelArg(k, 1, sizeof(cl_mem), &ob);
    size_t g = 15;
    clEnqueueNDRangeKernel(q, k, 1, nullptr, &g, nullptr, 0, nullptr, nullptr);
    clEnqueueReadBuffer(q, ob, CL_TRUE, 0, 15 * 8, out, 0, nullptr, nullptr); // values all correct
    int bad = 0;
    for (int i = 0; i < 15; i++) {
        uint64_t want = edgeInputs[i] ? (uint64_t)__builtin_clzll(edgeInputs[i]) : 0xDEAD;
        if (out[i] != want) bad++;
    }
    printf("readback: %d/15 correct\n", 15 - bad);
    clReleaseMemObject(ob);   // <-- process aborts here (glibc "invalid pointer")
    clReleaseKernel(k);
    printf("released cleanly\n");
    return 0;
}
