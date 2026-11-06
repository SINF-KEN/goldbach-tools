// Minimal: known-good atomic64 harness + one clz kernel added.
#include <CL/cl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
static const char* SRC = R"CLC(
__kernel void t_add32(volatile __global int* c){ atomic_add(c, 1); }
__kernel void k_clz(__global const ulong* in, __global ulong* out){
    ulong x = in[get_global_id(0)]; if (x) out[get_global_id(0)] = clz(x); else out[get_global_id(0)] = 0xDEAD;
}
)CLC";
#define CK(x) do { cl_int e_=(x); if(e_!=CL_SUCCESS){ printf("OPENCL ERROR %d at %s\n",e_,#x); return 1;} } while(0)
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
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
    CK(clBuildProgram(prog, 1, &dev, nullptr, nullptr, nullptr));
    printf("build OK\n");

    // 1) atomic add (known-good pattern from atomic64)
    {
        cl_kernel k = clCreateKernel(prog, "t_add32", nullptr);
        cl_int zero = 0, out = 0;
        cl_mem buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, 4, &zero, nullptr);
        CK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
        size_t N = 4096;
        CK(clEnqueueNDRangeKernel(q, k, 1, nullptr, &N, nullptr, 0, nullptr, nullptr));
        CK(clEnqueueReadBuffer(q, buf, CL_TRUE, 0, 4, &out, 0, nullptr, nullptr));
        printf("t_add32 -> %d (want 4096) %s\n", out, out == 4096 ? "OK" : "FAIL");
        clReleaseMemObject(buf); clReleaseKernel(k);
    }
    // 2) clz kernel, identical pattern to builtin64's run2
    {
        printf("clz: create kernel\n");
        cl_kernel k = clCreateKernel(prog, "k_clz", nullptr);
        printf("clz: create buffers\n");
        uint64_t in[15] = {1, 2, 3, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFF00000001ull,
            0x8000000000000000ull, 0x8000000000000001ull, 0xFFFFFFFFFFFFFFFEull,
            0xFFFFFFFFFFFFFFFFull, 0x123456789ABCDEFull, 0xFEDCBA9876543210ull,
            0x5555555555555555ull, 0x00000000FFFFFFFFull, 0x0000000100000000ull};
        uint64_t out[15];
        cl_mem ib = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof in, in, nullptr);
        cl_mem ob = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof out, nullptr, nullptr);
        printf("clz: set args\n");
        CK(clSetKernelArg(k, 0, sizeof(cl_mem), &ib));
        CK(clSetKernelArg(k, 1, sizeof(cl_mem), &ob));
        printf("clz: enqueue\n");
        size_t M = 15;
        CK(clEnqueueNDRangeKernel(q, k, 1, nullptr, &M, nullptr, 0, nullptr, nullptr));
        printf("clz: read\n");
        CK(clEnqueueReadBuffer(q, ob, CL_TRUE, 0, sizeof out, out, 0, nullptr, nullptr));
        printf("clz: release\n");
        clReleaseMemObject(ib); clReleaseMemObject(ob); clReleaseKernel(k);
        printf("clz: released\n");
        for (int i = 0; i < 15; i++) {
            uint64_t want = in[i] ? (uint64_t)__builtin_clzll(in[i]) : 0xDEAD;
            printf("  clz(0x%016llx)=%llu want=%llu %s\n", (unsigned long long)in[i],
                   (unsigned long long)out[i], (unsigned long long)want, out[i] == want ? "OK" : "FAIL");
        }
    }
    printf("SURVIVED\n");
    return 0;
}
