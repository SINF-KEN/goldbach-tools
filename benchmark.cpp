// benchmark.cpp — B580 Level Zero Goldbach verifier benchmark.
//
// Runs the full verification pipeline multiple times, discards the first run
// (warm-up: driver JIT caches, allocator pools, clock ramp-up), and prints a
// report in the standard format:
//
//   Hardware / Driver / Compute Runtime / Level Zero
//   Benchmark: N, range, even count, piece size, candidate bound, fallbacks
//   initialization / verification / total
//   runs, first run discarded, median
//
// usage: goldbach_bench [--limit N] [--segment N] [--runs R] [--spv FILE]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ze_api.h>

#include "config.hpp"
#include "verifier.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

std::string fmt_u64(uint64_t v) {
    // Thousands separators for readability.
    std::string s = std::to_string(v);
    for (int pos = int(s.size()) - 3; pos > 0; pos -= 3)
        s.insert(size_t(pos), ",");
    return s;
}

std::string fmt_bytes(uint64_t bytes) {
    char buf[64];
    if (bytes >= (1ULL << 30))
        std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ULL << 30));
    else if (bytes >= (1ULL << 20))
        std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / double(1ULL << 20));
    else
        std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
    return buf;
}

std::string fmt_seconds(double s) {
    char buf[64];
    if (s >= 3600.0)
        std::snprintf(buf, sizeof buf, "%.2f h", s / 3600.0);
    else if (s >= 60.0)
        std::snprintf(buf, sizeof buf, "%.2f min", s / 60.0);
    else
        std::snprintf(buf, sizeof buf, "%.3f s", s);
    return buf;
}

std::string fmt_rate(double evens_per_sec) {
    char buf[64];
    if (evens_per_sec >= 1e12)
        std::snprintf(buf, sizeof buf, "%.2f T evens/s", evens_per_sec / 1e12);
    else if (evens_per_sec >= 1e9)
        std::snprintf(buf, sizeof buf, "%.2f G evens/s", evens_per_sec / 1e9);
    else if (evens_per_sec >= 1e6)
        std::snprintf(buf, sizeof buf, "%.2f M evens/s", evens_per_sec / 1e6);
    else
        std::snprintf(buf, sizeof buf, "%.3g evens/s", evens_per_sec);
    return buf;
}

// ---------------------------------------------------------------- hardware --

struct EnvInfo {
    std::string device_name;
    uint64_t    vram_bytes   = 0;
    std::string vram_name;
    uint32_t    slices       = 0;
    uint32_t    subslices    = 0;
    uint32_t    eus          = 0;
    uint32_t    threads_per_eu = 0;
    uint32_t    clock_mhz    = 0;
    uint32_t    eu_count     = 0;
    std::string driver_version;
    uint32_t    l0_major = 0, l0_minor = 0;
    std::string compute_runtime;   // best-effort: from driver name string
};

EnvInfo query_environment() {
    EnvInfo env;

    ze_result_t r = zeInit(ZE_INIT_FLAG_GPU_ONLY);
    if (r != ZE_RESULT_SUCCESS) return env;

    uint32_t nd = 0;
    zeDriverGet(&nd, nullptr);
    std::vector<ze_driver_handle_t> drivers(nd);
    zeDriverGet(&nd, drivers.data());

    for (ze_driver_handle_t d : drivers) {
        uint32_t nv = 0;
        zeDeviceGet(d, &nv, nullptr);
        std::vector<ze_device_handle_t> devs(nv);
        zeDeviceGet(d, &nv, devs.data());

        for (ze_device_handle_t dev : devs) {
            ze_device_properties_t p{};
            p.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
            if (zeDeviceGetProperties(dev, &p) != ZE_RESULT_SUCCESS) continue;
            if (p.type != ZE_DEVICE_TYPE_GPU) continue;

            env.device_name = p.name;
            env.clock_mhz  = p.coreClockRate;
            env.slices     = p.numSlices;
            env.subslices  = p.numSubslicesPerSlice;
            env.eus        = p.numEUsPerSubslice;
            env.threads_per_eu = p.numThreadsPerEU;
            env.eu_count   = p.numSlices * p.numSubslicesPerSlice * p.numEUsPerSubslice;

            // VRAM
            uint32_t nmem = 0;
            zeDeviceGetMemoryProperties(dev, &nmem, nullptr);
            std::vector<ze_device_memory_properties_t> mems(nmem);
            zeDeviceGetMemoryProperties(dev, &nmem, mems.data());
            if (nmem > 0) {
                env.vram_bytes = mems[0].totalSize;
                env.vram_name  = mems[0].name;
            }

            // Driver / API version. NEO driverVersion encodes the release
            // as e.g. 17013219 -> 1.7.13219 (major, minor, build).
            ze_driver_properties_t dp{};
            dp.stype = ZE_STRUCTURE_TYPE_DRIVER_PROPERTIES;
            if (zeDriverGetProperties(d, &dp) == ZE_RESULT_SUCCESS) {
                const uint32_t v = dp.driverVersion;
                char buf[64];
                std::snprintf(buf, sizeof buf, "%u.%u.%u",
                              (v / 10000000) % 100, (v / 100000) % 100, v % 100000);
                env.driver_version = buf;
            }
            ze_api_version_t api = ZE_API_VERSION_1_0;
            zeDriverGetApiVersion(d, &api);
            env.l0_major = ZE_MAJOR_VERSION(api);
            env.l0_minor = ZE_MINOR_VERSION(api);

            // Compute Runtime version is not exposed via L0; the driver
            // version string (e.g. 1.3.32xxx) encodes the NEO release.
            env.compute_runtime = "intel-level-zero-gpu " + env.driver_version;

            return env;   // first GPU is enough
        }
    }
    return env;
}

void print_environment(const EnvInfo& env) {
    std::printf("Hardware:\n");
    std::printf("  GPU: %s\n", env.device_name.c_str());
    std::printf("  VRAM: %s (%s)\n", fmt_bytes(env.vram_bytes).c_str(),
                env.vram_name.c_str());
    if (env.eu_count)
        std::printf("  Execution units: %u (%u slices x %u x %u)\n",
                    env.eu_count, env.slices, env.subslices, env.eus);
    if (env.clock_mhz)
        std::printf("  Core clock: %u MHz\n", env.clock_mhz);
    std::printf("\n");

    std::printf("Driver:\n");
    std::printf("  intel-level-zero-gpu: %s\n", env.driver_version.c_str());
    std::printf("\n");

    std::printf("Compute Runtime:\n");
    std::printf("  %s\n", env.compute_runtime.c_str());
    std::printf("\n");

    std::printf("Level Zero:\n");
    std::printf("  API version: %u.%u\n", env.l0_major, env.l0_minor);
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    uint64_t limit   = gb::DEFAULT_LIMIT;
    uint64_t segment = gb::DEFAULT_SEGMENT_EVEN;
    int      runs    = 5;
    std::string spv  = "kernels/goldbach_bmg.spv";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next_u64 = [&](uint64_t& out) {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); return 2; }
            out = std::strtoull(argv[++i], nullptr, 0);
            return 0;
        };
        int rc = 0;
        if      (a == "--limit")   rc = next_u64(limit);
        else if (a == "--segment") rc = next_u64(segment);
        else if (a == "--runs") {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for --runs\n"); return 2; }
            runs = std::atoi(argv[++i]);
        }
        else if (a == "--spv") {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for --spv\n"); return 2; }
            spv = argv[++i];
        }
        else if (a == "--help" || a == "-h") {
            std::printf("usage: goldbach_bench [--limit N] [--segment N] [--runs R] [--spv FILE]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
        if (rc) return rc;
    }
    if (runs < 2) {
        std::fprintf(stderr, "need at least 2 runs (first is discarded)\n");
        return 2;
    }

    try {
        const EnvInfo env = query_environment();

        // ------------------------------------------------ timed runs --------
        std::vector<double> init_times, gpu_times, total_times;
        uint64_t total_segments  = 0;
        uint64_t total_leftovers = 0;

        for (int run = 0; run < runs; ++run) {
            const gb::RunStats s = gb::run_gpu(limit, segment, spv, false);
            init_times.push_back(s.init_seconds);
            gpu_times.push_back(s.gpu_seconds);
            total_times.push_back(s.total_seconds);
            total_segments  = s.segments;
            total_leftovers = s.leftovers;

            std::printf("run %d/%d: init %.3f s, verify %.3f s, total %.3f s\n",
                        run + 1, runs, s.init_seconds, s.gpu_seconds, s.total_seconds);
            std::fflush(stdout);
        }

        // First run discarded (warm-up).
        std::vector<double> med = gpu_times;
        std::sort(med.begin() + 1, med.end());
        const double median_gpu = med[1 + (runs - 1) / 2];
        const double best_gpu   = *std::min_element(med.begin() + 1, med.end());

        const uint64_t evens = (limit - 2) / 2;

        // ------------------------------------------------------ report ------
        std::printf("\n");
        std::printf("B580 Level Zero Goldbach verifier\n");
        std::printf("\n");
        print_environment(env);
        std::printf("Benchmark:\n");
        {
            const double lg = std::log10(double(limit));
            if (std::fabs(lg - std::round(lg)) < 1e-9)
                std::printf("  N = 10^%d\n", int(std::round(lg)));
            else
                std::printf("  N = %s\n", fmt_u64(limit).c_str());
        }
        std::printf("  range = [4, %s]\n", fmt_u64(limit).c_str());
        std::printf("  even n = %s\n", fmt_u64(evens).c_str());
        std::printf("  piece = %s even n\n", fmt_u64(segment).c_str());
        std::printf("  candidate p bound = %u\n", gb::MAX_GB_PRIME);
        std::printf("  batch = %s odd q per tile (SLM tile)\n",
                    fmt_u64(gb::BITS_PER_TILE).c_str());
        std::printf("  fallbacks = %llu\n", (unsigned long long)total_leftovers);
        std::printf("\n");
        std::printf("initialization = %s\n", fmt_seconds(init_times.back()).c_str());
        std::printf("verification  = %s\n", fmt_seconds(median_gpu).c_str());
        std::printf("total         = %s\n",
                    fmt_seconds(init_times.back() + median_gpu).c_str());
        std::printf("throughput    = %s (median)\n", fmt_rate(evens / median_gpu).c_str());
        std::printf("\n");
        std::printf("runs = %d\n", runs);
        std::printf("first run discarded\n");
        std::printf("median = %s\n", fmt_seconds(median_gpu).c_str());
        std::printf("best   = %s\n", fmt_seconds(best_gpu).c_str());
        std::printf("segments = %llu\n", (unsigned long long)total_segments);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
