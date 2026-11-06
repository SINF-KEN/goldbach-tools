// Debug tool: run ONLY prep_large_fb for a set of segment geometries, copy
// the compact first-mark offsets back, and compare bit-for-bit against the
// host reference computation (the exact math the verifier used before the
// computation moved to the GPU). This isolates the offset kernel from the
// sieve/check kernels.
//
// The cases deliberately include: the start of the range (q_first = 3),
// the near-4e18 benchmark regime, tiny spans where most primes have no
// multiple in range (UINT32_MAX sentinel path), an odd-position segment,
// and a split that makes large_count indivisible by LARGE_PRIMES_PER_ITEM.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cmath>

#include "l0_context.hpp"
#include "primes.hpp"
#include "config.hpp"

using u64 = uint64_t;
using u32 = uint32_t;

namespace {

struct Case {
    u64 start;
    u64 limit;
    u64 segment;   // integer span
    u32 split;
    const char* name;
};

// Host reference: mirrors the former verifier.cpp computation exactly.
std::vector<u32> host_reference(const u32* sieve_primes, u32 split, u32 count,
                                u64 q_first, u64 q_last) {
    std::vector<u32> ref(size_t(count - split), 0xffffffffu);
    for (u32 j = split; j < count; ++j) {
        const u64 p = sieve_primes[j];
        u64 first = p * p;
        if (first < q_first) {
            u64 multiplier = q_first / p;
            if (multiplier * p < q_first) ++multiplier;
            if ((multiplier & 1) == 0) ++multiplier;
            first = multiplier * p;
        }
        ref[j - split] = (first > q_last) ? 0xffffffffu
                                          : u32((first - q_first) >> 1);
    }
    return ref;
}

} // namespace

int main(int argc, char** argv) {
    const Case cases[] = {
        {4,                    1'000'000'000'000,         400'000'000,  4000, "start-of-range, large primes exist"},
        {4,                    1'000'000'000,             400'000'000,  4000, "start-of-range q_first=3"},
        {3999999999000000000,  4000000000000000000,       400'000'000,  4000, "4e18 window, 1e9 span"},
        {3999999990000000000,  4000000000000000000,       400'000'000,  4000, "4e18 window, 1e10 span"},
        {3999999999999990000,  4000000000000000000,       10'000,       4000, "4e18, tiny span (mostly sentinel)"},
        {4000000000000000000 - 40'000'000, 4000000000000000000, 40'000'000, 4000, "4e18, small span"},
        {1234567890123456788,  1234567890123456788 + 1'000'000'000, 500'000'000, 4000, "odd-position segment"},
        {4,                    100'000'000'000,           50'000'000'000, 4001, "split makes large_count%4!=0"},
        {999999999999999876,   1'000'000'000'000'000'000, 400'000'000,  4000, "near 1e18"},
        {4999999999000000000,  5000000000000000000,       400'000'000, 30000, "5e18 window (largest q_first), split 30000"},
        {4999999999000000000,  5000000000000000000,       400'000'000,  4000, "5e18 window, split 4000"},
    };

    int failures = 0;
    try {
        gb::Context ctx;
        gb::Module  mod(ctx, argc > 1 ? argv[1] : "kernels/goldbach_bmg.spv");
        gb::Kernel  k(mod, "prep_large_fb");
        k.set_group_size(gb::WG_SIZE);

        u64 loaded_limit = 0;
        std::vector<u32> primes;

        for (const Case& c : cases) {
            const u32 sqrt_limit = u32(std::sqrt(double(c.limit)) + 16);
            if (loaded_limit != sqrt_limit) {
                gb::PrimeCacheView view = gb::PrimeCacheView::load(
                    std::max<u64>(sqrt_limit, 64), "base_primes.cache");
                primes.assign(view.data(), view.data() + view.size());
                loaded_limit = sqrt_limit;
            }
            const u32 count = u32(primes.size());
            if (c.split >= count) {
                std::printf("[%s] skip: split >= sieve_count\n", c.name);
                continue;
            }
            const u32 large_count = count - c.split;

            const u64 seg_start = c.start & ~u64(1);
            const u64 remaining = c.limit - seg_start;
            const u64 seg_end = remaining <= c.segment
                              ? c.limit : seg_start + c.segment - 2;
            u64 qf = seg_start - gb::MAX_GB_PRIME;
            if (qf >= seg_start) qf -= 1;
            const u64 q_first = (seg_start >= gb::MAX_GB_PRIME + 2) ? qf : 3;
            const u64 q_last = seg_end - 3;

            // Upload primes (device) once per cache.
            u32* d_primes = static_cast<u32*>(ctx.alloc_device(size_t(count) * 4));
            u32* d_fb = static_cast<u32*>(ctx.alloc_device(std::max<size_t>(large_count * 4, 64)));
            {
                // Two staging buffers: the immediate command list runs the
                // copies asynchronously, so re-using one buffer for the
                // poison pattern let the host overwrite the primes before the
                // first DMA read them (intermittent all-sentinel "FAILED" —
                // the kernel then saw 0xAAAAAAAA primes).
                void* stage = ctx.alloc_shared(size_t(count) * 4);
                void* poison = ctx.alloc_shared(std::max<size_t>(large_count * 4, 64));
                std::memcpy(stage, primes.data(), size_t(count) * 4);
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_primes, stage,
                                                       size_t(count) * 4, nullptr, 0, nullptr));
                // Poison the output buffer: every entry must be rewritten.
                std::memset(poison, 0xAA, std::max<size_t>(large_count * 4, 64));
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_fb, poison,
                                                       std::max<size_t>(large_count * 4, 64), nullptr, 0, nullptr));
                ctx.host_sync();
                gb::Context::free_mem(ctx.context(), stage);
                gb::Context::free_mem(ctx.context(), poison);
            }

            u32 split = c.split;
            u32 cnt = count;
            k.set_arg(0, sizeof(d_primes), &d_primes);
            k.set_arg(1, sizeof(d_fb),     &d_fb);
            k.set_arg(2, sizeof(split),    &split);
            k.set_arg(3, sizeof(cnt),      &cnt);
            k.set_arg(4, sizeof(q_first),  &q_first);
            k.set_arg(5, sizeof(q_last),   &q_last);

            const u64 large_items =
                (u64(large_count) + gb::LARGE_PRIMES_PER_ITEM - 1) /
                gb::LARGE_PRIMES_PER_ITEM;
            const u32 groups = u32((large_items + gb::WG_SIZE - 1) / gb::WG_SIZE);
            k.launch(ctx.list(), groups);
            ctx.host_sync();

            std::vector<u32> got(size_t(large_count), 0);
            {
                void* out = ctx.alloc_shared(std::max<size_t>(large_count * 4, 64));
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), out, d_fb,
                                                       std::max<size_t>(large_count * 4, 64), nullptr, 0, nullptr));
                ctx.host_sync();
                std::memcpy(got.data(), out, size_t(large_count) * 4);
                gb::Context::free_mem(ctx.context(), out);
            }

            const std::vector<u32> ref =
                host_reference(primes.data(), c.split, count, q_first, q_last);

            u64 diffs = 0, sentinel_ref = 0, sentinel_gpu = 0;
            u32 first_bad = 0xffffffffu;
            for (u64 i = 0; i < large_count; ++i) {
                if (ref[i] == 0xffffffffu) ++sentinel_ref;
                if (got[i] == 0xffffffffu) ++sentinel_gpu;
                if (ref[i] != got[i]) {
                    if (first_bad == 0xffffffffu) first_bad = u32(i);
                    ++diffs;
                }
            }
            const bool ok = diffs == 0;
            std::printf("[%s] %s  entries=%u sentinels ref=%llu gpu=%llu diffs=%llu\n",
                        c.name, ok ? "OK" : "FAILED", large_count,
                        (unsigned long long)sentinel_ref,
                        (unsigned long long)sentinel_gpu,
                        (unsigned long long)diffs);
            if (!ok) {
                ++failures;
                for (u32 k = 0; k < 5 && first_bad + k < large_count; ++k) {
                    const u32 i = first_bad + k;
                    if (ref[i] != got[i])
                        std::printf("  idx %u (p=%u): gpu=%u ref=%u\n", i,
                                    primes[c.split + i], got[i], ref[i]);
                }
            }
            gb::Context::free_mem(ctx.context(), d_primes);
            gb::Context::free_mem(ctx.context(), d_fb);
        }
        // ---- packed large-prime list: decode_large4 vs the host list ----
        // Every large prime of the full 5e18 list, at several splits
        // (group/item alignments, tiny and ragged tails).
        {
            gb::Kernel ku(mod, "unpack_large_test");
            ku.set_group_size(gb::WG_SIZE);
            gb::PrimeCacheView view = gb::PrimeCacheView::load(2'236'067'993ull, "base_primes.cache");
            const u32 count = u32(view.size());
            for (u32 split : {40000u, 4001u, 17u, count - 1u, count - 63u, count - 64u, count - 65u}) {
                const u32 lc = count - split;
                const u64 items = (u64(lc) + gb::LARGE_PACK_PPI - 1) / gb::LARGE_PACK_PPI;
                std::vector<uint8_t> gaps(std::max<u64>(items, 1) * gb::LARGE_PACK_PPI, 0);
                std::vector<u32> bases(std::max<u64>((u64(lc) + gb::LARGE_PACK_GROUP - 1) / gb::LARGE_PACK_GROUP, 1), 0);
                for (u64 i = 0; i < lc; ++i) {
                    const u32 p = view.data()[split + i];
                    if (i % gb::LARGE_PACK_GROUP == 0) { bases[i / gb::LARGE_PACK_GROUP] = p; continue; }
                    gaps[i] = uint8_t((p - view.data()[split + i - 1]) / 2);
                }
                u32* d_b = static_cast<u32*>(ctx.alloc_device(bases.size() * 4));
                u32* d_g = static_cast<u32*>(ctx.alloc_device(gaps.size()));
                u32* d_o = static_cast<u32*>(ctx.alloc_device(size_t(lc) * 4));
                void* sb = ctx.alloc_shared(bases.size() * 4);
                void* sg = ctx.alloc_shared(gaps.size());
                void* so = ctx.alloc_shared(size_t(lc) * 4);
                std::memcpy(sb, bases.data(), bases.size() * 4);
                std::memcpy(sg, gaps.data(), gaps.size());
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_b, sb, bases.size() * 4, nullptr, 0, nullptr));
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_g, sg, gaps.size(), nullptr, 0, nullptr));
                ctx.host_sync();
                ku.set_arg(0, sizeof(d_b), &d_b);
                ku.set_arg(1, sizeof(d_g), &d_g);
                ku.set_arg(2, sizeof(lc), &lc);
                ku.set_arg(3, sizeof(d_o), &d_o);
                ku.launch(ctx.list(), u32((items + gb::WG_SIZE - 1) / gb::WG_SIZE));
                ctx.host_sync();
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), so, d_o, size_t(lc) * 4, nullptr, 0, nullptr));
                ctx.host_sync();
                const u32* got = static_cast<const u32*>(so);
                u64 diffs = 0;
                for (u64 i = 0; i < lc; ++i) diffs += got[i] != view.data()[split + i];
                std::printf("[packed decode, split %u] %s large=%u diffs=%llu\n", split,
                            diffs ? "FAILED" : "OK", lc, (unsigned long long)diffs);
                if (diffs) ++failures;
                for (void* ptr : {(void*)d_b, (void*)d_g, (void*)d_o, sb, sg, so})
                    gb::Context::free_mem(ctx.context(), ptr);
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    if (failures) {
        std::printf("FB FAILED: %d case(s)\n", failures);
        return 1;
    }
    std::printf("FB OK\n");
    return 0;
}
