// Debug tool: run make_prime_map + check_numbers for a small segment, copy the
// checked bitset back, and compare against a CPU Goldbach check. Isolates the
// check kernel from the host pipeline.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "l0_context.hpp"
#include "primes.hpp"
#include "config.hpp"

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;

int main(int argc, char** argv) {
    const u64 seg_start = 4;
    const u64 seg_evens = 600000;            // 600k
    const u64 seg_end   = seg_start + (seg_evens - 1) * 2;
    const u64 q_first   = 3;
    const u64 q_last    = seg_end - 3;
    const u64 q_odds    = (q_last - q_first) / 2 + 1;
    const u64 map_words = (q_odds + 63) / 64;
    // The sieve writes WHOLE tiles (one work-group = TILE_WORDS words), so the
    // map buffer must be sized in whole tiles — a partial final tile would
    // write past the end of the buffer and hang the GPU (device lost).
    const u64 map_alloc_words = ((q_odds + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE) * gb::TILE_WORDS;
    const u64 chk_words = (seg_evens + 63) / 64;
    const u64 tiles     = (q_odds + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE;

    try {
        gb::Context ctx;
        gb::Module  mod(ctx, argc > 1 ? argv[1] : "kernels/goldbach_bmg.spv");
        gb::Kernel  ks(mod, "make_prime_map");
        // GB_CHECK_KERNEL selects a single-kernel checker (default check_numbers_w32,
        // phase A of the production two-phase checker; debug_check_hi covers
        // the full two-phase pipeline).
        const char* kname = std::getenv("GB_CHECK_KERNEL") ? std::getenv("GB_CHECK_KERNEL")
                                                          : "check_numbers_w32";
        std::printf("checker kernel: %s\n", kname);
        gb::Kernel  kc(mod, kname);
        ks.set_group_size(gb::WG_SIZE);
        kc.set_group_size(gb::WG_SIZE);

        u64* map     = static_cast<u64*>(ctx.alloc_device(map_alloc_words * 8));
        u64* checked = static_cast<u64*>(ctx.alloc_device(chk_words * 8));
        u32* leftovers = static_cast<u32*>(ctx.alloc_device(size_t(gb::MAX_LEFTOVERS) * 4));
        u32* leftover_n = static_cast<u32*>(ctx.alloc_device(4));

        std::vector<u32> sieve_primes = gb::odd_primes_up_to(1000);
        std::vector<u16> gb_primes;
        for (u32 p : gb::odd_primes_up_to(gb::MAX_GB_PRIME)) gb_primes.push_back(u16(p));
        const u32 sieve_count = u32(sieve_primes.size());
        const u32 gb_count    = u32(gb_primes.size());

        u32* d_sieve = static_cast<u32*>(ctx.alloc_device(sieve_primes.size() * 4));
        u64* d_first = static_cast<u64*>(ctx.alloc_device(sieve_primes.size() * 8));
        u64* d_magic = static_cast<u64*>(ctx.alloc_device(sieve_primes.size() * 8));
        u32* d_shift = static_cast<u32*>(ctx.alloc_device(sieve_primes.size() * 4));
        u16* d_gb    = static_cast<u16*>(ctx.alloc_device(gb_primes.size() * 2));
        {
            void* s1 = ctx.alloc_shared(sieve_primes.size() * 4);
            std::memcpy(s1, sieve_primes.data(), sieve_primes.size() * 4);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_sieve, s1,
                                                   sieve_primes.size() * 4, nullptr, 0, nullptr));
            void* s2 = ctx.alloc_shared(gb_primes.size() * 2);
            std::memcpy(s2, gb_primes.data(), gb_primes.size() * 2);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_gb, s2,
                                                   gb_primes.size() * 2, nullptr, 0, nullptr));
            ctx.host_sync();
            gb::Context::free_mem(ctx.context(), s1);
            gb::Context::free_mem(ctx.context(), s2);
        }

        // Per-prime first markable bits + magic reciprocals (mirrors verifier.cpp).
        std::vector<u64> fb(sieve_primes.size()), mg(sieve_primes.size());
        std::vector<u32> sh(sieve_primes.size());
        for (size_t i = 0; i < sieve_primes.size(); ++i) {
            const u64 p = sieve_primes[i];
            mg[i] = (u64)(((__uint128_t)1 << 64) / p) + 1;
            u32 s = 0;
            for (;;) {
                bool ok = true;
                for (u64 x : {1ull, 63ull, 4095ull, 65535ull, 1048575ull,
                              (1ull << 32) - 1, (1ull << 33) - 1}) {
                    if ((u64)(((__uint128_t)x * mg[i]) >> (64 + s)) != x / p) { ok = false; break; }
                }
                if (ok) break;
                if (++s > 32) { s = 0; break; }
            }
            sh[i] = s;
            if (p > q_last / p) { fb[i] = ~0ull; continue; }
            u64 first = p * p;
            if (first < q_first) {
                first = ((q_first + p - 1) / p) * p;
                if ((first & 1) == 0) first += p;
            }
            if (first > q_last) { fb[i] = ~0ull; continue; }
            fb[i] = (first - q_first) >> 1;
        }
        {
            void* s3 = ctx.alloc_shared(sieve_primes.size() * 8);
            std::memcpy(s3, fb.data(), sieve_primes.size() * 8);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_first, s3,
                                                   sieve_primes.size() * 8, nullptr, 0, nullptr));
            void* s4 = ctx.alloc_shared(sieve_primes.size() * 8);
            std::memcpy(s4, mg.data(), sieve_primes.size() * 8);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_magic, s4,
                                                   sieve_primes.size() * 8, nullptr, 0, nullptr));
            void* s5 = ctx.alloc_shared(sieve_primes.size() * 4);
            std::memcpy(s5, sh.data(), sieve_primes.size() * 4);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_shift, s5,
                                                   sieve_primes.size() * 4, nullptr, 0, nullptr));
            ctx.host_sync();
            gb::Context::free_mem(ctx.context(), s3);
            gb::Context::free_mem(ctx.context(), s4);
            gb::Context::free_mem(ctx.context(), s5);
        }

        ks.set_arg(0, sizeof(map),          &map);
        ks.set_arg(1, sizeof(d_sieve),      &d_sieve);
        ks.set_arg(2, sizeof(d_first),      &d_first);
        ks.set_arg(3, sizeof(d_magic),      &d_magic);
        ks.set_arg(4, sizeof(d_shift),      &d_shift);
        ks.set_arg(5, sizeof(sieve_count),  &sieve_count);
        ks.set_arg(6, sizeof(q_odds),       &q_odds);
        ks.set_local_arg(7, size_t(gb::TILE_WORDS) * 8);
        u32 mask_count = 0;                 // scalar path only (mask table not built here)
        u64 qf = q_first;
        u64 null_tab = 0;
        u32 split = sieve_count;            // tile walk covers ALL primes (old behavior)
        ks.set_arg(8, sizeof(mask_count), &mask_count);
        ks.set_arg(9, sizeof(null_tab),   &null_tab);
        ks.set_arg(10, sizeof(qf),        &qf);
        ks.set_arg(11, sizeof(split),     &split);
        u32* null_tile_first = nullptr;  // legacy reciprocal path
        u32 use_table = 0;
        u64* null_ms32 = nullptr;
        ks.set_arg(12, sizeof(null_tile_first), &null_tile_first);
        ks.set_arg(13, sizeof(use_table), &use_table);
        ks.set_arg(14, sizeof(null_ms32), &null_ms32);
        { const u32 own_end_all = split;   // stage 3m off (thread-owned walk only)
          ks.set_arg(15, sizeof(own_end_all), &own_end_all); }
        { const void* no_grp = nullptr; const u32 n_grp = 0;   // per-prime mask path
          ks.set_arg(16, sizeof(no_grp), &no_grp); ks.set_arg(17, sizeof(no_grp), &no_grp);
          ks.set_arg(18, sizeof(n_grp), &n_grp); }
        {   // class-mode args (v1.11.0): cm = 0 -> dense map; the rest unused
            const u32 z = 0; const u64 z8 = 0;
            for (u32 a : {19u, 20u, 21u, 24u, 25u}) ks.set_arg(a, 4, &z);
            ks.set_arg(22, 8, &z8); ks.set_arg(23, 8, &z8);
            const void* no_gy = nullptr; ks.set_arg(26, sizeof(no_gy), &no_gy);   // group_y: none
        }

        u32 zero = 0;
        ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), leftover_n, &zero, 1, 4,
                                               nullptr, 0, nullptr));

        kc.set_arg(0, sizeof(map),        &map);
        kc.set_arg(1, sizeof(checked),    &checked);
        kc.set_arg(2, sizeof(d_gb),       &d_gb);
        kc.set_arg(3, sizeof(gb_count),   &gb_count);
        kc.set_arg(4, sizeof(leftovers),  &leftovers);
        kc.set_arg(5, sizeof(leftover_n), &leftover_n);
        kc.set_arg(6, sizeof(q_first),    &q_first);
        kc.set_arg(7, sizeof(q_odds),     &q_odds);
        kc.set_arg(8, sizeof(seg_start),  &seg_start);
        kc.set_arg(9, sizeof(seg_evens),  &seg_evens);

        ks.launch(ctx.list(), u32(tiles));
        ctx.host_sync();   // ensure the map is fully written before checking
        // check_numbers is the ILP variant: each work-item covers 4 x 64 evens.
        kc.launch(ctx.list(), u32((chk_words + 3) / 4));
        ctx.host_sync();

        void* out = ctx.alloc_shared(chk_words * 8);
        ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), out, checked, chk_words * 8,
                                               nullptr, 0, nullptr));
        ctx.host_sync();

        const u64* got = static_cast<const u64*>(out);
        u64 miss = 0;
        for (u64 w = 0; w < chk_words; ++w) {
            u64 valid_bits = (w == chk_words - 1) ? (seg_evens - w * 64) : 64;
            u64 m = (~got[w]) & ((valid_bits >= 64) ? ~0ULL : ((1ULL << valid_bits) - 1));
            miss += __builtin_popcountll(m);
        }
        std::printf("GPU check misses: %llu / %llu evens\n",
                    (unsigned long long)miss, (unsigned long long)seg_evens);

        // Show first few misses.
        u32 shown = 0;
        for (u64 w = 0; w < chk_words && shown < 8; ++w) {
            u64 valid_bits = (w == chk_words - 1) ? (seg_evens - w * 64) : 64;
            u64 m = (~got[w]) & ((valid_bits >= 64) ? ~0ULL : ((1ULL << valid_bits) - 1));
            while (m && shown < 8) {
                u32 b = __builtin_ctzll(m); m &= m - 1;
                u64 n = seg_start + 2 * (w * 64 + b);
                std::printf("  miss n=%llu\n", (unsigned long long)n);
                ++shown;
            }
        }
        if (miss == 1) std::printf("(expected: exactly n=4 via p=2)\n");

        gb::Context::free_mem(ctx.context(), map);
        gb::Context::free_mem(ctx.context(), checked);
        gb::Context::free_mem(ctx.context(), leftovers);
        gb::Context::free_mem(ctx.context(), leftover_n);
        gb::Context::free_mem(ctx.context(), d_sieve);
        gb::Context::free_mem(ctx.context(), d_gb);
        gb::Context::free_mem(ctx.context(), out);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
