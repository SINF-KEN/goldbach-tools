// 64-bit builtin correctness sweep on B580 vs CPU reference.
// Ops: clz, ctz, popcount, mul_hi, mad_hi, udiv, urem, shl, shr (u64+long).
// Edge inputs; shift amounts kept < 64 (>=64 is UB in OpenCL C).
#include <CL/cl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

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
__kernel void k_div(__global const ulong* a, __global const ulong* b, __global ulong* out){
    if (b[get_global_id(0)]) out[get_global_id(0)] = a[get_global_id(0)] / b[get_global_id(0)]; else out[get_global_id(0)] = 0xDEAD;
}
__kernel void k_rem(__global const ulong* a, __global const ulong* b, __global ulong* out){
    if (b[get_global_id(0)]) out[get_global_id(0)] = a[get_global_id(0)] % b[get_global_id(0)]; else out[get_global_id(0)] = 0xDEAD;
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

static inline uint64_t ref_mul_hi(uint64_t a, uint64_t b){ return (uint64_t)(((__uint128_t)a * b) >> 64); }
static inline uint64_t ref_mad_hi(uint64_t a, uint64_t b, uint64_t c){
    __uint128_t t = (__uint128_t)a * b + c; return (uint64_t)(t >> 64);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<uint64_t> edge = {
        1, 2, 3, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFF00000001ull,
        0x8000000000000000ull, 0x8000000000000001ull, 0xFFFFFFFFFFFFFFFEull,
        0xFFFFFFFFFFFFFFFFull, 0x123456789ABCDEFull, 0xFEDCBA9876543210ull,
        0x5555555555555555ull, 0x00000000FFFFFFFFull, 0x0000000100000000ull
    };
    const size_t M = edge.size();
    std::vector<uint64_t> shifts; for (uint64_t i = 0; i < 64; i++) shifts.push_back(i);
    const size_t S = shifts.size();

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
    err = clBuildProgram(prog, 1, &dev, nullptr, nullptr, nullptr);
    if (err != CL_SUCCESS) { char log[16384] = {};
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof log, log, nullptr);
        printf("BUILD FAILED %s\n", log); return 1; }
    printf("build OK\n");

    int fails = 0;
    auto run2 = [&](const char* kn, cl_mem in, size_t n, std::vector<uint64_t>& out) {
        printf("  run2 %s: create kernel\n", kn);
        cl_kernel k = clCreateKernel(prog, kn, nullptr);
        printf("  run2 %s: create ob\n", kn);
        cl_mem ob = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n * 8, nullptr, nullptr);
        printf("  run2 %s: set args\n", kn);
        CK(clSetKernelArg(k, 0, sizeof(cl_mem), &in));
        CK(clSetKernelArg(k, 1, sizeof(cl_mem), &ob));
        printf("  run2 %s: enqueue\n", kn);
        CK(clEnqueueNDRangeKernel(q, k, 1, nullptr, &n, nullptr, 0, nullptr, nullptr));
        out.resize(n);
        printf("  run2 %s: read\n", kn);
        CK(clEnqueueReadBuffer(q, ob, CL_TRUE, 0, n * 8, out.data(), 0, nullptr, nullptr));
        printf("  run2 %s: release\n", kn);
        clReleaseMemObject(ob); clReleaseKernel(k);
        printf("  run2 %s: done\n", kn);
    };
    auto run3 = [&](const char* kn, cl_mem a, cl_mem b, size_t n, std::vector<uint64_t>& out) {
        cl_kernel k = clCreateKernel(prog, kn, nullptr);
        cl_mem ob = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n * 8, nullptr, nullptr);
        CK(clSetKernelArg(k, 0, sizeof(cl_mem), &a));
        CK(clSetKernelArg(k, 1, sizeof(cl_mem), &b));
        CK(clSetKernelArg(k, 2, sizeof(cl_mem), &ob));
        CK(clEnqueueNDRangeKernel(q, k, 1, nullptr, &n, nullptr, 0, nullptr, nullptr));
        out.resize(n);
        CK(clEnqueueReadBuffer(q, ob, CL_TRUE, 0, n * 8, out.data(), 0, nullptr, nullptr));
        clReleaseMemObject(ob); clReleaseKernel(k);
    };
    auto mkbuf = [&](const std::vector<uint64_t>& v) {
        return clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, v.size() * 8, (void*)v.data(), nullptr);
    };

    // clz / ctz / popcount
    {
        printf("S1 start\n");
        cl_mem in = mkbuf(edge); std::vector<uint64_t> out;
        printf("S1 buf ok\n");
        run2("k_clz", in, M, out);
        printf("S1 clz run ok\n");
        for (size_t i = 0; i < M; i++) { uint64_t want = edge[i] ? (uint64_t)__builtin_clzll(edge[i]) : 0xDEAD;
            if (out[i] != want) { fails++; printf("clz(0x%016llx) got %llu want %llu FAIL\n",(unsigned long long)edge[i],(unsigned long long)out[i],(unsigned long long)want);} }
        run2("k_ctz", in, M, out);
        for (size_t i = 0; i < M; i++) { uint64_t want = edge[i] ? (uint64_t)__builtin_ctzll(edge[i]) : 0xDEAD;
            if (out[i] != want) { fails++; printf("ctz(0x%016llx) got %llu want %llu FAIL\n",(unsigned long long)edge[i],(unsigned long long)out[i],(unsigned long long)want);} }
        run2("k_popcount", in, M, out);
        for (size_t i = 0; i < M; i++) { uint64_t want = (uint64_t)__builtin_popcountll(edge[i]);
            if (out[i] != want) { fails++; printf("popcount(0x%016llx) got %llu want %llu FAIL\n",(unsigned long long)edge[i],(unsigned long long)out[i],(unsigned long long)want);} }
        clReleaseMemObject(in);
        printf("clz/ctz/popcount done (%d fails so far)\n", fails);
    }
    // mul_hi / mad_hi
    {
        cl_mem a = mkbuf(edge), b = mkbuf(edge); std::vector<uint64_t> out;
        run3("k_mul_hi", a, b, M, out);
        for (size_t i = 0; i < M; i++) { uint64_t want = ref_mul_hi(edge[i], edge[i]);
            if (out[i] != want) { fails++; printf("mul_hi(0x%016llx) got %016llx want %016llx FAIL\n",(unsigned long long)edge[i],(unsigned long long)out[i],(unsigned long long)want);} }
        // mad_hi with c = b (reuse)
        run3("k_mad_hi", a, b, M, out);
        for (size_t i = 0; i < M; i++) { uint64_t want = ref_mad_hi(edge[i], edge[i], edge[i]);
            if (out[i] != want) { fails++; printf("mad_hi(0x%016llx) got %016llx want %016llx FAIL\n",(unsigned long long)edge[i],(unsigned long long)out[i],(unsigned long long)want);} }
        clReleaseMemObject(a); clReleaseMemObject(b);
        printf("mul_hi/mad_hi done (%d fails so far)\n", fails);
    }
    // div / rem (divisors include odd primes, powers of two, near/max)
    {
        std::vector<uint64_t> divs = {1,2,3,5,7,61,64,97,255,65537,0xFFFFFFFFull,0x100000000ull,
            0x7FFFFFFFFFFFFFFFull,0x8000000000000000ull,0xFFFFFFFFFFFFFFFFull, 0x987654321ull};
        std::vector<uint64_t> num;
        for (auto d : divs) { num.push_back(d - 1); num.push_back(d); num.push_back(d + 1); num.push_back(0xFFFFFFFFFFFFFFFFull / d * d + d - 1); }
        num.push_back(0x8000000000000000ull); num.push_back(0xFFFFFFFFFFFFFFFFull);
        std::vector<uint64_t> dens; for (size_t i = 0; i < num.size(); i++) dens.push_back(divs[i % divs.size()]);
        const size_t N2 = num.size();
        cl_mem a = mkbuf(num), b = mkbuf(dens); std::vector<uint64_t> out;
        run3("k_div", a, b, N2, out);
        for (size_t i = 0; i < N2; i++) { uint64_t want = num[i] / dens[i];
            if (out[i] != want) { fails++; printf("div(0x%016llx / 0x%016llx) got %016llx want %016llx FAIL\n",(unsigned long long)num[i],(unsigned long long)dens[i],(unsigned long long)out[i],(unsigned long long)want);} }
        run3("k_rem", a, b, N2, out);
        for (size_t i = 0; i < N2; i++) { uint64_t want = num[i] % dens[i];
            if (out[i] != want) { fails++; printf("rem(0x%016llx %% 0x%016llx) got %016llx want %016llx FAIL\n",(unsigned long long)num[i],(unsigned long long)dens[i],(unsigned long long)out[i],(unsigned long long)want);} }
        clReleaseMemObject(a); clReleaseMemObject(b);
        printf("div/rem done (%d fails so far)\n", fails);
    }
    // shifts: u64 shl/shr, long shr (arithmetic)
    {
        cl_mem a = mkbuf(edge), sm = mkbuf(shifts);
        // cross product: use index i -> a[i%M], s[i%S]
        std::vector<uint64_t> A, SH; const size_t N3 = M * S;
        for (size_t i = 0; i < N3; i++) { A.push_back(edge[i % M]); SH.push_back(shifts[i % S]); }
        clReleaseMemObject(a); clReleaseMemObject(sm);
        a = mkbuf(A); sm = mkbuf(SH);
        std::vector<uint64_t> out;
        run3("k_shl", a, sm, N3, out);
        for (size_t i = 0; i < N3; i++) { uint64_t want = A[i] << SH[i];
            if (out[i] != want) { fails++; printf("shl(0x%016llx<<%llu) got %016llx want %016llx FAIL\n",(unsigned long long)A[i],(unsigned long long)SH[i],(unsigned long long)out[i],(unsigned long long)want);} }
        run3("k_shr", a, sm, N3, out);
        for (size_t i = 0; i < N3; i++) { uint64_t want = A[i] >> SH[i];
            if (out[i] != want) { fails++; printf("shr(0x%016llx>>%llu) got %016llx want %016llx FAIL\n",(unsigned long long)A[i],(unsigned long long)SH[i],(unsigned long long)out[i],(unsigned long long)want);} }
        // arithmetic shift on negative longs
        std::vector<int64_t> SA; for (auto x : edge) SA.push_back((int64_t)x); SA[0] = -1; SA[1] = -2;
        std::vector<uint64_t> SAv(SA.begin(), SA.end());
        cl_mem sa = mkbuf(SAv);
        cl_kernel k = clCreateKernel(prog, "k_sshr", nullptr);
        cl_mem ob = clCreateBuffer(ctx, CL_MEM_READ_WRITE, N3 * 8, nullptr, nullptr);
        CK(clSetKernelArg(k,0,sizeof(cl_mem),&sa));
        CK(clSetKernelArg(k,1,sizeof(cl_mem),&sm));
        CK(clSetKernelArg(k,2,sizeof(cl_mem),&ob));
        CK(clEnqueueNDRangeKernel(q,k,1,nullptr,&N3,nullptr,0,nullptr,nullptr));
        std::vector<uint64_t> souts(N3);
        CK(clEnqueueReadBuffer(q,ob,CL_TRUE,0,N3*8,souts.data(),0,nullptr,nullptr));
        clReleaseMemObject(ob); clReleaseKernel(k);
        for (size_t i = 0; i < N3; i++) { int64_t want = SA[i % M] >> SH[i % S];
            int64_t got = (int64_t)souts[i];
            if (got != want) { fails++; printf("sshr(%lld>>%llu) got %lld want %lld FAIL\n",(long long)SA[i%M],(unsigned long long)SH[i%S],(long long)got,(long long)want);} }
        clReleaseMemObject(a); clReleaseMemObject(sm); clReleaseMemObject(sa);
        printf("shifts done (%d fails total)\n", fails);
    }

    printf("\n%s (%d mismatches)\n", fails ? "MISMATCHES FOUND - investigate before filing" : "ALL 64-BIT BUILTINS CORRECT", fails);
    return fails ? 1 : 0;
}
