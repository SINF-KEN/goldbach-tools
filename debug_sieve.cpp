// Debug tool: run ONLY make_prime_map for a small segment, copy the prime map
// back, and compare bit-for-bit against a CPU sieve. This isolates whether the
// sieve kernel or the check kernel is at fault.
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

int main(int argc, char** argv) {
    const u64 q_first = 3;
    const u64 q_odds  = 2499998;              // 3 tiles
    const u64 words   = ((q_odds + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE) * gb::TILE_WORDS;
    const u64 tiles   = (q_odds + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE;

    try {
        gb::Context ctx;
        gb::Module  mod(ctx, argc > 1 ? argv[1] : "kernels/goldbach_bmg.spv");
        gb::Kernel  k(mod, "make_prime_map");
        k.set_group_size(gb::WG_SIZE);

        u64* map = static_cast<u64*>(ctx.alloc_device(words * 8));
        u32  sieve_count = 0;
        std::vector<u32> sieve_primes;

        {
            const u32 sqrt_limit = 2500;
            sieve_primes = gb::odd_primes_up_to(sqrt_limit);
            sieve_count  = u32(sieve_primes.size());
        }
        u32* d_primes = static_cast<u32*>(ctx.alloc_device(sieve_primes.size() * 4));
        u64* d_first  = static_cast<u64*>(ctx.alloc_device(sieve_primes.size() * 8));
        u64* d_magic  = static_cast<u64*>(ctx.alloc_device(sieve_primes.size() * 8));
        u32* d_shift  = static_cast<u32*>(ctx.alloc_device(sieve_primes.size() * 4));
        void* stage   = ctx.alloc_shared(sieve_primes.size() * 4);
        std::memcpy(stage, sieve_primes.data(), sieve_primes.size() * 4);
        ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_primes, stage,
                                               sieve_primes.size() * 4, nullptr, 0, nullptr));
        ctx.host_sync();
        gb::Context::free_mem(ctx.context(), stage);

        // Host-side per-prime first bits + reciprocals (mirrors verifier.cpp).
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
            const u64 tile_num0 = q_first;
            const u64 tile_high = q_first + 2 * gb::BITS_PER_TILE - 2;
            if (p > tile_high / p) { fb[i] = ~0ull; continue; }
            u64 first = p * p;
            if (first < tile_num0) {
                first = ((tile_num0 + p - 1) / p) * p;
                if ((first & 1) == 0) first += p;
            }
            if (first > tile_high) { fb[i] = ~0ull; continue; }
            fb[i] = (first - q_first) >> 1;
        }
        // Segment-global first-mark table (mirrors the new verifier): one
        // entry per prime, then a SINGLE launch covering all tiles. The
        // kernel derives each tile's start with the magic reciprocal.
        {
            const u64 q_last = q_first + 2 * q_odds - 2;
            for (size_t i = 0; i < sieve_primes.size(); ++i) {
                const u64 p = sieve_primes[i];
                if (p > q_last / p) { fb[i] = ~0ull; continue; }
                u64 first = p * p;
                if (first < q_first) {
                    first = ((q_first + p - 1) / p) * p;
                    if ((first & 1) == 0) first += p;
                }
                if (first > q_last) { fb[i] = ~0ull; continue; }
                fb[i] = (first - q_first) >> 1;
            }
        }
        k.set_arg(0, sizeof(map),         &map);
        k.set_arg(1, sizeof(d_primes),    &d_primes);
        k.set_arg(2, sizeof(d_first),     &d_first);
        k.set_arg(3, sizeof(d_magic),     &d_magic);
        k.set_arg(4, sizeof(d_shift),     &d_shift);
        k.set_arg(5, sizeof(sieve_count), &sieve_count);
        k.set_arg(6, sizeof(q_odds),      &q_odds);
        k.set_local_arg(7, size_t(gb::TILE_WORDS) * 8);
        u32 mask_count = 0;                 // scalar path only (mask table not built here)
        u64 qf = q_first;
        u64 null_tab = 0;
        u32 split = sieve_count;            // tile walk covers ALL primes (old behavior)
        k.set_arg(8, sizeof(mask_count), &mask_count);
        k.set_arg(9, sizeof(null_tab),   &null_tab);
        k.set_arg(10, sizeof(qf),        &qf);
        k.set_arg(11, sizeof(split),     &split);
        // Stage 3m coverage: GB_MED_P=N walks primes >= N prime-parallel with
        // SLM atomics (production default gb::MED_MIN_P); unset = own_end =
        // split (pure thread-owned walk, the historical behavior).
        u32 own_end = split;
        if (const char* e = std::getenv("GB_MED_P"))
            own_end = std::max<u32>(u32(std::lower_bound(sieve_primes.data(), sieve_primes.data() + split,
                                                     u32(std::strtoul(e, nullptr, 0))) - sieve_primes.data()), 0u);
        k.set_arg(15, sizeof(own_end), &own_end);
        { const void* no_grp = nullptr; const u32 n_grp = 0;   // per-prime mask path
          k.set_arg(16, sizeof(no_grp), &no_grp); k.set_arg(17, sizeof(no_grp), &no_grp);
          k.set_arg(18, sizeof(n_grp), &n_grp); }
        {   // class-mode args (v1.11.0): cm = 0 -> dense map; the rest unused
            const u32 z = 0; const u64 z8 = 0;
            for (u32 a : {19u, 20u, 21u, 24u, 25u}) k.set_arg(a, 4, &z);
            k.set_arg(22, 8, &z8); k.set_arg(23, 8, &z8);
            const void* no_gy = nullptr; k.set_arg(26, sizeof(no_gy), &no_gy);   // group_y: none
        }
        u32* null_tile_first = nullptr;  // legacy reciprocal path
        u32 use_table = 0;
        u64* null_ms32 = nullptr;
        k.set_arg(12, sizeof(null_tile_first), &null_tile_first);
        k.set_arg(13, sizeof(use_table), &use_table);
        k.set_arg(14, sizeof(null_ms32), &null_ms32);

        {   // upload segment-independent tables once
            void* s1 = ctx.alloc_shared(sieve_primes.size() * 8);
            std::memcpy(s1, fb.data(), sieve_primes.size() * 8);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_first, s1,
                                                   sieve_primes.size() * 8, nullptr, 0, nullptr));
            void* s2 = ctx.alloc_shared(sieve_primes.size() * 8);
            std::memcpy(s2, mg.data(), sieve_primes.size() * 8);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_magic, s2,
                                                   sieve_primes.size() * 8, nullptr, 0, nullptr));
            void* s3 = ctx.alloc_shared(sieve_primes.size() * 4);
            std::memcpy(s3, sh.data(), sieve_primes.size() * 4);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_shift, s3,
                                                   sieve_primes.size() * 4, nullptr, 0, nullptr));
            ctx.host_sync();
            gb::Context::free_mem(ctx.context(), s1);
            gb::Context::free_mem(ctx.context(), s2);
            gb::Context::free_mem(ctx.context(), s3);
        }
        k.launch(ctx.list(), u32(tiles));
        ctx.host_sync();

        // Copy back via shared staging.
        void* out = ctx.alloc_shared(words * 8);
        ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), out, map, words * 8,
                                               nullptr, 0, nullptr));
        ctx.host_sync();

        // CPU reference.
        std::vector<u64> ref(words, 0);
        for (u64 i = 0; i < q_odds; ++i) {
            const u64 q = q_first + 2 * i;
            if (gb::is_prime_trial(q)) ref[i >> 6] |= (1ULL << (i & 63));
        }

        const u64* got = static_cast<const u64*>(out);
        u64 diff_words = 0, ones = 0;
        for (u64 w = 0; w < words; ++w) {
            ones += __builtin_popcountll(got[w]);
            if (got[w] != ref[w]) ++diff_words;
        }
        std::printf("GPU popcount=%llu  CPU popcount=%llu  diff_words=%llu / %llu\n",
                    (unsigned long long)ones,
                    (unsigned long long)[&]{ u64 c=0; for (auto w : ref) c += __builtin_popcountll(w); return c; }(),
                    (unsigned long long)diff_words, (unsigned long long)words);

        // Show first few differing bits.
        u32 shown = 0;
        for (u64 w = 0; w < words && shown < 8; ++w) {
            if (got[w] != ref[w]) {
                u64 x = got[w] ^ ref[w];
                while (x && shown < 8) {
                    u32 b = __builtin_ctzll(x); x &= x - 1;
                    u64 bit = w * 64 + b;
                    u64 q = q_first + 2 * bit;
                    std::printf("  bit %llu (q=%llu): gpu=%d cpu=%d\n",
                                (unsigned long long)bit, (unsigned long long)q,
                                (int)((got[w] >> b) & 1), (int)((ref[w] >> b) & 1));
                    ++shown;
                }
            }
        }
        if (diff_words == 0) std::printf("SIEVE OK\n");

        // ---- second launch: per-tile-table path (use_table=1) -------------
        // Mirrors verifier.cpp: host builds tile_first[t*split + j] (u32,
        // UINT32_MAX = none) plus 32-bit reciprocals for the within-tile jump,
        // then relaunches and compares against the same CPU reference.
        {
            const u32 tiles_u32 = u32(tiles);
            std::vector<u32> tbl(size_t(tiles) * std::max<u32>(own_end, 1), 0xffffffffu);  // stride = own_end
            std::vector<u64> ms32v(sieve_count, 0ull);
            for (size_t i = 0; i < sieve_primes.size(); ++i) {
                const u64 p = sieve_primes[i];
                if (p < gb::BITS_PER_TILE) {
                    u32 s = 0;
                    while ((u64(1) << s) <= gb::BITS_PER_TILE * p) ++s;
                    const u64 m = ((u64(1) << s) + p - 1) / p;
                    ms32v[i] = m | (u64(s) << 32);
                }
            }
            for (u64 t = 0; t < tiles; ++t) {
                u32* row = tbl.data() + t * own_end;
                const u64 tb0 = t * gb::BITS_PER_TILE;
                for (size_t j = 0; j < own_end; ++j) {
                    const u64 f = fb[j];
                    if (f == ~0ull) continue;                 // stays 0xffffffff
                    if (f >= tb0) {
                        if (f - tb0 < gb::BITS_PER_TILE)
                            row[j] = u32(f - tb0);
                        continue;                              // else none
                    }
                    const u64 p = sieve_primes[j];
                    const u64 d = tb0 - f;
                    u64 k = u64(((__uint128_t)d * mg[j]) >> (64 + sh[j]));
                    k += (k * p < d) ? 1ull : 0ull;
                    const u64 bit = f + k * p - tb0;
                    if (bit < gb::BITS_PER_TILE) row[j] = u32(bit);
                }
            }
            u32* d_tbl = static_cast<u32*>(ctx.alloc_device(tbl.size() * 4));
            u64* d_ms32 = static_cast<u64*>(ctx.alloc_device(ms32v.size() * 8));
            {
                void* s6 = ctx.alloc_shared(tbl.size() * 4);
                std::memcpy(s6, tbl.data(), tbl.size() * 4);
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_tbl, s6,
                                                       tbl.size() * 4, nullptr, 0, nullptr));
                void* s7 = ctx.alloc_shared(ms32v.size() * 8);
                std::memcpy(s7, ms32v.data(), ms32v.size() * 8);
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_ms32, s7,
                                                       ms32v.size() * 8, nullptr, 0, nullptr));
                ctx.host_sync();
                gb::Context::free_mem(ctx.context(), s6);
                gb::Context::free_mem(ctx.context(), s7);
            }
            u32 use_table = 1;
            k.set_arg(12, sizeof(d_tbl), &d_tbl);
            k.set_arg(13, sizeof(use_table), &use_table);
            k.set_arg(14, sizeof(d_ms32), &d_ms32);
            k.launch(ctx.list(), u32(tiles));
            ctx.host_sync();

            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), out, map, words * 8,
                                                   nullptr, 0, nullptr));
            ctx.host_sync();

            u64 diff2 = 0;
            for (u64 w = 0; w < words; ++w)
                if (got[w] != ref[w]) ++diff2;
            if (diff2 == 0) std::printf("SIEVE OK (table path)\n");
            else {
                std::printf("SIEVE FAILED (table path): diff_words=%llu\n",
                            (unsigned long long)diff2);
                u32 shown2 = 0;
                for (u64 w = 0; w < words && shown2 < 8; ++w) {
                    if (got[w] != ref[w]) {
                        u64 x = got[w] ^ ref[w];
                        while (x && shown2 < 8) {
                            u32 b = __builtin_ctzll(x); x &= x - 1;
                            u64 bit = w * 64 + b;
                            u64 q = q_first + 2 * bit;
                            std::printf("  bit %llu (q=%llu): table=%d cpu=%d\n",
                                        (unsigned long long)bit,
                                        (unsigned long long)q,
                                        (int)((got[w] >> b) & 1),
                                        (int)((ref[w] >> b) & 1));
                            ++shown2;
                        }
                    }
                }
            }
            gb::Context::free_mem(ctx.context(), d_tbl);
            gb::Context::free_mem(ctx.context(), d_ms32);
        }

        gb::Context::free_mem(ctx.context(), map);
        gb::Context::free_mem(ctx.context(), d_primes);
        gb::Context::free_mem(ctx.context(), out);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
