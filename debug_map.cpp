// Debug tool: validate the COMPLETE prime map produced by the GPU pipeline —
// make_prime_map (mask path + per-tile-table path, exactly as the verifier
// drives it) followed by sieve_large_primes (fused inline offsets) — against
// a fully INDEPENDENT reference: deterministic Miller-Rabin per odd q.
//
// debug_sieve tests the tile kernel alone with mask_count=0 and
// split = sieve_count (no large kernel at all). This tool closes the loop on
// everything that tool cannot see: the small-prime mask path, the q=p
// restore, and the large-prime atomic marking with inline first-bit math.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

#include "l0_context.hpp"
#include "primes.hpp"
#include "config.hpp"

using u64 = uint64_t;
using u32 = uint32_t;
using u8  = uint8_t;

namespace {

struct Case {
    u64 q_first;
    u64 q_odds;
    u32 split;
    const char* name;
};

// Independent reference: bit i <-> q = q_first + 2*i, set iff q is prime
// (deterministic Miller-Rabin — shares no logic with the sieve).
std::vector<u64> reference_map(u64 q_first, u64 q_odds) {
    std::vector<u64> ref((q_odds + 63) / 64, 0);
    // Parallel over word ranges (each thread owns whole words): the
    // full-segment cases run Miller-Rabin on ~125M odd numbers.
    const u64 words = ref.size();
    const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; ++t) {
        const u64 w0 = words * t / nt, w1 = words * (t + 1) / nt;
        th.emplace_back([&, w0, w1] {
            for (u64 w = w0; w < w1; ++w) {
                u64 v = 0;
                for (u64 b = 0; b < 64; ++b) {
                    const u64 i = w * 64 + b;
                    if (i < q_odds && gb::is_prime_u64(q_first + 2 * i)) v |= 1ull << b;
                }
                ref[w] = v;
            }
        });
    }
    for (auto& x : th) x.join();
    return ref;
}

} // namespace

int main(int argc, char** argv) {
    // Defaults cover: the start of the range (mask restore path active), an
    // interior segment near 1e18, and a near-4e18 segment. q_odds kept small
    // enough that the Miller-Rabin reference stays seconds, not minutes.
    Case cases[] = {
        {3ull,                              500000, 100,  "start-of-range, split 100"},
        {999999999000000001ull,             500000, 100,  "near 1e18, split 100"},
        {3999999999999999997ull - 2*499999, 500000, 100,  "near 4e18, split 100"},
        {3999999999999999997ull - 2*499999, 500000, 4000, "near 4e18, split 4000"},
        {3999999999999999997ull - 2*999,    1000,   4000, "near 4e18, tiny span"},
        {3999999999999999997ull - 2*1499999, 1500000, 60000, "near 4e18, split 60000 (stage 3m)"},
        {4999999999999999997ull - 2*1499999, 1500000, 60000, "near 5e18, split 60000 (stage 3m)"},
        // One FULL production segment (250M-integer span, q_odds 125,004,889)
        // at each end of the range with the production split: every large
        // prime marks through the whole segment, including the wheel steps
        // after a prime's first hit (the u32-step overflow class of bug is
        // invisible in the small windows above). GB_MAP_FAST=1 skips them.
        {4000000000000000000ull - 9781, 125004889, 30000, "FULL segment at 4e18, split 30000"},
        {4999999999750000000ull - 9781, 125004889, 30000, "FULL segment below 5e18, split 30000"},
        // v1.11.0 default span (350M integers, q_odds 175,004,889)
        {4000000000000000000ull - 9781, 175004889, 32000, "FULL 350M segment at 4e18, split 32000"},
        {4999999999650000000ull - 9781, 175004889, 32000, "FULL 350M segment below 5e18, split 32000"},
        // v1.12.0 default span (425M integers, q_odds 212,504,889)
        {4000000000000000000ull - 9781, 212504889, 32000, "FULL 425M segment at 4e18, split 32000"},
        {4999999999575000000ull - 9781, 212504889, 32000, "FULL 425M segment below 5e18, split 32000"},
    };
    if (argc > 1 && argc < 4) {
        std::fprintf(stderr, "usage: debug_map [q_first q_odds split [spv]]  (GB_MAP_SPV=file for the defaults)\n");
        return 2;
    }
    if (argc > 1) {
        // Override: q_first q_odds split — for targeted reruns.
        cases[0] = {std::strtoull(argv[1], nullptr, 0),
                    std::strtoull(argv[2], nullptr, 0),
                    u32(std::strtoul(argv[3], nullptr, 0)), "custom"};
    }

    int failures = 0;
    try {
        gb::Context ctx;
        // SPIR-V: argv[4] (with a custom case) or GB_MAP_SPV (with the defaults).
        const char* spv = argc > 4 ? argv[4]
                        : std::getenv("GB_MAP_SPV") ? std::getenv("GB_MAP_SPV")
                                                    : "kernels/goldbach_bmg.spv";
        gb::Module  mod(ctx, spv);
        gb::Kernel  k_sieve(mod, "make_prime_map");
        // GB_MAP_LARGE_PLAIN=1 validates sieve_large_primes; default is the
        // production sieve_large_packed (1 B/prime) exactly as verifier.cpp.
        const bool large_plain = std::getenv("GB_MAP_LARGE_PLAIN") != nullptr;
        gb::Kernel  k_large(mod, large_plain ? "sieve_large_primes" : "sieve_large_packed");
        k_sieve.set_group_size(gb::WG_SIZE);
        k_large.set_group_size(gb::LARGE_WG_SIZE);

        ze_event_pool_handle_t pool = nullptr;
        ze_event_handle_t ev_sieve_dbg = nullptr;
        {
            ze_event_pool_desc_t pd{};
            pd.stype = ZE_STRUCTURE_TYPE_EVENT_POOL_DESC;
            pd.count = 1;
            pd.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
            ZE_CHECK(zeEventPoolCreate(ctx.context(), &pd, 0, nullptr, &pool));
            ze_event_desc_t ed{};
            ed.stype = ZE_STRUCTURE_TYPE_EVENT_DESC;
            ed.index = 0;
            ed.signal = ZE_EVENT_SCOPE_FLAG_DEVICE;
            ed.wait = ZE_EVENT_SCOPE_FLAG_DEVICE;
            ZE_CHECK(zeEventCreate(pool, &ed, &ev_sieve_dbg));
        }

        // One cache load covers all cases: sqrt(4e18) = 2e9.
        gb::PrimeCacheView view = gb::PrimeCacheView::load(2'236'067'993ull,  // sqrt(5e18)+16
                                                           "base_primes.cache");
        const u32 sieve_count = u32(view.size());

        // Map layouts: dense (bit i <-> q = q_first + 2i) and the class map
        // (v1.11.0 production: M1 / M5 = the q = 1 / 5 (mod 6), each tpc whole
        // tiles; kernels/goldbach.cl map_clear). GB_MAP_MODE=dense|class.
        // Layouts: 0 dense, 6 = mod-6 class map (v1.11.0), 30 = mod-30 class
        // map (v1.12.0 production). GB_MAP_MODE=dense|class|class30.
        std::vector<u32> modes = {0u, 6u, 30u};
        if (const char* m = std::getenv("GB_MAP_MODE")) {
            const std::string ms = m;
            modes = {ms == "class30" ? 30u : ms == "class" ? 6u : 0u};
        }
        for (const Case& c : cases)
        for (const u32 mode : modes) {
            const bool cls = mode != 0;
            const u32 K = mode == 30 ? 8u : 2u, M = mode == 30 ? 30u : 6u, DM = M / 2;
            static const u32 RES30[8] = {1, 7, 11, 13, 17, 19, 23, 29};
            auto res = [&](u32 k) { return M == 30 ? RES30[k] : (k ? 5u : 1u); };
            if (std::getenv("GB_MAP_FAST") && c.q_odds > 10000000) continue;
            const u64 q_last = c.q_first + 2 * (c.q_odds - 1);
            // class geometry, derived independently of verifier.cpp
            u32 phi[8] = {}; u64 Qc[8] = {}, nc[8] = {}, nmax = 0;
            for (u32 k = 0; k < K; ++k) {
                while ((c.q_first + 2 * phi[k]) % M != res(k)) ++phi[k];
                Qc[k] = c.q_first + 2 * phi[k];
                for (u64 t = (c.q_odds > phi[k] ? (c.q_odds - phi[k]) / DM : 0); ; ++t) {
                    if (DM * t + phi[k] >= c.q_odds) { nc[k] = t; break; }
                }
                nmax = std::max(nmax, nc[k]);
            }
            const u32 tpc = u32(std::max<u64>((nmax + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE, 1));
            const u64 tiles = cls ? u64(K) * tpc : (c.q_odds + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE;
            const u64 map_words = tiles * gb::TILE_WORDS;
            const u32 c5w = tpc * gb::TILE_WORDS, off5 = c5w * 64u;
            const u32 cm = mode == 30 ? 2u : cls ? 1u : 0u;
            u32 phis = phi[0] | (phi[1] << 8), ga1 = phi[0], ga2 = phi[1];
            if (mode == 30) {                  // phase nibbles; phase -> class table
                phis = 0; u64 ph2k = ~0ull;
                for (u32 k = 0; k < 8; ++k) {
                    phis |= phi[k] << (4 * k);
                    ph2k = (ph2k & ~(0xFull << (4 * phi[k]))) | (u64(k) << (4 * phi[k]));
                }
                ga1 = u32(ph2k); ga2 = u32(ph2k >> 32);
            }
            // mirrors verifier.cpp: primes strictly below MASK_MAX_P=64
            // (3..61 = 17 primes; the 18th odd prime is 67 >= 64!)
            const u32 mask_count = 17;
            (void)mask_count;
            const u32 split = (c.split > mask_count) ? c.split : mask_count;

            // ---- host tables, mirroring verifier.cpp --------------------
            std::vector<u64> fb(split, ~0ull);
            std::vector<u64> mg(split), ms32v(split, 0ull);
            std::vector<u32> sh(split);
            for (u32 j = 0; j < split; ++j) {
                const u64 p = view.data()[j];
                mg[j] = (u64)(((__uint128_t)1 << 64) / p) + 1;
                u32 s = 0;
                for (;;) {
                    bool ok = true;
                    for (u64 x : {1ull, 63ull, 4095ull, 65535ull, 1048575ull,
                                  (1ull << 32) - 1, (1ull << 33) - 1}) {
                        if ((u64)(((__uint128_t)x * mg[j]) >> (64 + s)) != x / p) { ok = false; break; }
                    }
                    if (ok) break;
                    if (++s > 32) { s = 0; break; }
                }
                sh[j] = s;
                if (p > q_last / p) continue;
                u64 first = p * p;
                if (first < c.q_first) {
                    first = ((c.q_first + p - 1) / p) * p;
                    if ((first & 1) == 0) first += p;
                }
                if (first > q_last) continue;
                fb[j] = (first - c.q_first) >> 1;
                if (p < gb::BITS_PER_TILE) {
                    u32 s2 = 0;
                    while ((u64(1) << s2) <= gb::BITS_PER_TILE * p) ++s2;
                    const u64 m = ((u64(1) << s2) + p - 1) / p;
                    ms32v[j] = m | (u64(s2) << 32);
                }
            }
            // Stage 3m (medium primes, SLM atomics) exactly as the verifier:
            // primes >= MED_MIN_P (GB_MED_P overrides) walk prime-parallel;
            // the table holds only the thread-owned columns (stride own_end).
            const u32 mask_count_eff = std::getenv("GB_MAP_NOMASK") ? 0 : 17;
            u32 med_p = cls ? gb::MED_MIN_P_CLASS : gb::MED_MIN_P;   // as verifier.cpp
            if (const char* e = std::getenv("GB_MED_P")) med_p = u32(std::strtoul(e, nullptr, 0));
            const u32 own_end = std::clamp<u32>(u32(std::lower_bound(
                view.data(), view.data() + split, med_p) - view.data()), mask_count_eff, split);
            // Class mode: fb[k * split + j] = class-k index of the first
            // multiple of p in class k that is >= max(p*p, q_first) (brute
            // force over the next six multipliers; p = 3 has none).
            if (cls) {
                std::vector<u64> fbc(size_t(K) * split, ~0ull);
                for (u32 j = 0; j < split; ++j) {
                    const u64 p = view.data()[j];
                    if (M % p == 0 || p > q_last / p) continue;
                    const u64 first = std::max(p * p, c.q_first);
                    for (u32 k = 0; k < K; ++k)
                        for (u64 m = (first + p - 1) / p; ; ++m) {
                            if ((m * p) % M != res(k)) continue;
                            if (m * p <= q_last) fbc[size_t(k) * split + j] = (m * p - Qc[k]) / M;
                            break;
                        }
                }
                fb = fbc;
            }
            std::vector<u32> tile_first(size_t(tiles) * std::max<u32>(own_end, 1), 0xffffffffu);
            for (u64 t = 0; t < tiles; ++t) {
                u32* row = tile_first.data() + t * own_end;
                const u32 k = cls ? u32(t / tpc) : 0u;
                const u64 tb0 = (t - k * tpc) * gb::BITS_PER_TILE;
                for (u32 j = 0; j < own_end; ++j) {
                    const u64 f = fb[size_t(k) * split + j];
                    if (f == ~0ull) continue;
                    if (f >= tb0) {
                        if (f - tb0 < gb::BITS_PER_TILE) row[j] = u32(f - tb0);
                        continue;
                    }
                    const u64 p = view.data()[j];
                    const u64 d = tb0 - f;
                    u64 k = u64(((__uint128_t)d * mg[j]) >> (64 + sh[j]));
                    k += (k * p < d) ? 1ull : 0ull;
                    const u64 bit = f + k * p - tb0;
                    if (bit < gb::BITS_PER_TILE) row[j] = u32(bit);
                }
            }
            std::vector<u64> mask_table =
                gb::small_prime_mask_table(view.data(), mask_count, c.q_first);
            if (cls) {                       // per-class tables back to back
                mask_table.clear();
                for (u32 k = 0; k < K; ++k) {
                    const auto mk = gb::small_prime_mask_table_step(view.data(), mask_count, Qc[k], M);
                    mask_table.insert(mask_table.end(), mk.begin(), mk.end());
                }
            }

            // GB_MAP_TILE_ONLY=1 skips the large-prime kernel (bisecting).
            const bool tile_only = std::getenv("GB_MAP_TILE_ONLY") != nullptr;
            // ---- GPU buffers --------------------------------------------
            // The tile kernel needs primes[0..split); the large kernel needs
            // primes up to sqrt(q_last) (multiples of larger primes all fall
            // beyond q_last... they start at p*p > q_last). So upload only
            // primes[0..needed) — the MR reference stays exact because every
            // composite below q_last has a factor below sqrt(q_last).
            u32 primes_uploaded;
            if (tile_only) {
                primes_uploaded = split;
            } else {
                const u64 sqrt_q_last = (u64)std::sqrt((double)q_last) + 2;
                primes_uploaded = u32(std::upper_bound(
                    view.data(), view.data() + sieve_count, u32(sqrt_q_last)) -
                    view.data());
                if (primes_uploaded < split) primes_uploaded = split;
                if (primes_uploaded > sieve_count) primes_uploaded = sieve_count;
            }
            u64* map = static_cast<u64*>(ctx.alloc_device(map_words * 8));
            u32* d_primes = static_cast<u32*>(ctx.alloc_device(size_t(primes_uploaded) * 4));
            u64* d_first = static_cast<u64*>(ctx.alloc_device(fb.size() * 8));
            u64* d_magic = static_cast<u64*>(ctx.alloc_device(size_t(split) * 8));
            u32* d_shift = static_cast<u32*>(ctx.alloc_device(size_t(split) * 4));
            u64* d_ms32 = static_cast<u64*>(ctx.alloc_device(size_t(split) * 8));
            u32* d_tbl = static_cast<u32*>(ctx.alloc_device(tile_first.size() * 4));
            u64* d_mask = static_cast<u64*>(ctx.alloc_device(mask_table.size() * 8));
            {
                // chunked upload: small reusable staging buffer. Sync per
                // chunk — the next host memcpy into s1 must not overwrite a
                // chunk whose DMA copy is still pending.
                constexpr size_t CHUNK = 1u << 20;  // 1 MB
                void* s1 = ctx.alloc_shared(CHUNK);
                for (size_t off = 0; off < size_t(primes_uploaded) * 4; off += CHUNK) {
                    const size_t n = std::min(CHUNK, size_t(primes_uploaded) * 4 - off);
                    std::memcpy(s1, reinterpret_cast<const char*>(view.data()) + off, n);
                    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(),
                        reinterpret_cast<char*>(d_primes) + off, s1, n, nullptr, 0, nullptr));
                    ctx.host_sync();
                }
                struct Up { void* stage; const void* src; void* dst; size_t bytes; };
                const Up ups[] = {
                    {nullptr, fb.data(), d_first, fb.size() * 8},
                    {nullptr, mg.data(), d_magic, size_t(split) * 8},
                    {nullptr, sh.data(), d_shift, size_t(split) * 4},
                    {nullptr, ms32v.data(), d_ms32, size_t(split) * 8},
                    {nullptr, tile_first.data(), d_tbl, tile_first.size() * 4},
                    {nullptr, mask_table.data(), d_mask, mask_table.size() * 8},
                };
                // Enqueue all uploads first, sync ONCE, only then free the
                // staging buffers: freeing shared memory whose DMA copy has
                // not executed yet lets the driver reuse the pages and
                // corrupts the upload (intermittent false_primes — the
                // staging race fixed on 2026-09-03, see PROJECT-REFERENCE.md §14.4).
                std::vector<void*> stages;
                stages.reserve(std::size(ups));
                for (const Up& u : ups) {
                    void* st = ctx.alloc_shared(u.bytes);
                    std::memcpy(st, u.src, u.bytes);
                    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), u.dst, st,
                                                           u.bytes, nullptr, 0, nullptr));
                    stages.push_back(st);
                }
                ctx.host_sync();
                for (void* st : stages) gb::Context::free_mem(ctx.context(), st);
                gb::Context::free_mem(ctx.context(), s1);
            }

            // ---- run: tile kernel (mask + table path), then large kernel -
            // GB_MAP_NOMASK=1 runs the tile kernel with mask_count=0.
            u32 use_table = 1;
            u32 count = primes_uploaded;
            u64 q_odds_arg = c.q_odds;
            u64 null_tab = 0;
            void* grp_bufs[2] = {nullptr, nullptr};
            k_sieve.set_arg(0, sizeof(map), &map);
            k_sieve.set_arg(1, sizeof(d_primes), &d_primes);
            k_sieve.set_arg(2, sizeof(d_first), &d_first);
            k_sieve.set_arg(3, sizeof(d_magic), &d_magic);
            k_sieve.set_arg(4, sizeof(d_shift), &d_shift);
            k_sieve.set_arg(5, sizeof(count), &count);
            k_sieve.set_arg(6, sizeof(q_odds_arg), &q_odds_arg);
            k_sieve.set_local_arg(7, size_t(gb::TILE_WORDS) * 8);
            k_sieve.set_arg(8, sizeof(mask_count_eff), &mask_count_eff);
            k_sieve.set_arg(9, sizeof(d_mask), &d_mask);
            k_sieve.set_arg(10, sizeof(c.q_first), &c.q_first);
            k_sieve.set_arg(11, sizeof(split), &split);
            k_sieve.set_arg(12, sizeof(d_tbl), &d_tbl);
            k_sieve.set_arg(13, sizeof(use_table), &use_table);
            k_sieve.set_arg(14, sizeof(d_ms32), &d_ms32);
            k_sieve.set_arg(15, sizeof(own_end), &own_end);
            // Fused grouped-mask stage exactly as the verifier (primes.cpp
            // small_prime_group_masks); GB_MASK_GROUPS=0 -> per-prime loop.
            const bool use_groups = mask_count_eff > 0 &&
                !(std::getenv("GB_MASK_GROUPS") && std::string(std::getenv("GB_MASK_GROUPS")) == "0");
            gb::SmallPrimeGroups grp = use_groups
                ? gb::small_prime_group_masks(view.data(), mask_count_eff, c.q_first)
                : gb::SmallPrimeGroups{};
            u32 mask_coff = u32(grp.words.size());
            // Class modes, as verifier.cpp: ONE canonical group table set plus
            // per-class word offsets (group_y). GB_MAP_EXPLICIT=1 instead
            // uploads explicit per-class tables (cross-check of the offsets).
            std::vector<u32> group_y(size_t(8) * 8, 0u);
            const bool explicit_cls = std::getenv("GB_MAP_EXPLICIT") != nullptr;
            if (use_groups && cls && !explicit_cls) {
                grp = gb::small_prime_group_masks_canonical(view.data(), mask_count_eff, M == 30 ? 15u : 3u);
                mask_coff = 0;
                for (u32 k = 0; k < K; ++k) {
                    const auto ys = gb::small_prime_group_offsets(view.data(), mask_count_eff, Qc[k], M);
                    for (size_t g = 0; g < ys.size(); ++g) group_y[size_t(k) * 8 + g] = ys[g];
                }
            } else if (use_groups && cls) {
                gb::SmallPrimeGroups all;
                for (u32 k = 0; k < K; ++k) {
                    const auto gk = gb::small_prime_group_masks_from(view.data(), mask_count_eff,
                        gb::small_prime_mask_table_step(view.data(), mask_count_eff, Qc[k], M));
                    if (k == 0) { all = gk; mask_coff = u32(gk.words.size()); }
                    else all.words.insert(all.words.end(), gk.words.begin(), gk.words.end());
                }
                grp = all;
            }
            u32* d_gy = nullptr;
            if (use_groups && cls && !explicit_cls) {
                d_gy = static_cast<u32*>(ctx.alloc_shared(group_y.size() * 4));
                std::memcpy(d_gy, group_y.data(), group_y.size() * 4);
            }
            const u32 n_grp = use_groups ? grp.count() : 0u;
            u64* d_grp = nullptr; u32* d_desc = nullptr;
            if (n_grp) {
                d_grp  = static_cast<u64*>(ctx.alloc_shared(grp.words.size() * 8));
                d_desc = static_cast<u32*>(ctx.alloc_shared(grp.desc.size() * 4));
                std::memcpy(d_grp, grp.words.data(), grp.words.size() * 8);
                std::memcpy(d_desc, grp.desc.data(), grp.desc.size() * 4);
            }
            k_sieve.set_arg(16, sizeof(d_grp), &d_grp);
            k_sieve.set_arg(17, sizeof(d_desc), &d_desc);
            k_sieve.set_arg(18, sizeof(n_grp), &n_grp);
            k_sieve.set_arg(19, 4, &cm);
            k_sieve.set_arg(20, 4, &tpc);
            k_sieve.set_arg(21, 4, &phis);
            k_sieve.set_arg(22, 8, &nc[0]);
            k_sieve.set_arg(23, 8, &nc[1]);
            k_sieve.set_arg(24, 4, &mask_coff);
            k_sieve.set_arg(26, sizeof(d_gy), &d_gy);
            k_sieve.set_arg(25, 4, &split);
            grp_bufs[0] = d_grp; grp_bufs[1] = d_desc;
            ZE_CHECK(zeEventHostReset(ev_sieve_dbg));
            k_sieve.launch(ctx.list(), u32(tiles), ev_sieve_dbg);

            u32* d_pk_bases = nullptr; u32* d_pk_gaps = nullptr;
            void* st_b = nullptr; void* st_g = nullptr;
            if (!tile_only) {
                // explicit dependency: the tile kernel's whole-word stores
                // must be VISIBLE before the large kernel's atomic ANDs.
                ZE_CHECK(zeCommandListAppendBarrier(ctx.list(), ev_sieve_dbg, 0, nullptr));
                u64 q_last_arg = q_last;
                k_large.set_arg(0, sizeof(map), &map);
                const u32 lc = count - split;   // large primes actually uploaded
                if (large_plain) {
                    k_large.set_arg(1, sizeof(d_primes), &d_primes);
                    k_large.set_arg(2, sizeof(split), &split);
                    k_large.set_arg(3, sizeof(count), &count);
                } else {
                    // Independent re-implementation of verifier.cpp's
                    // pack_large_primes (gap byte per prime, group bases).
                    const u64 items = (u64(lc) + gb::LARGE_PACK_PPI - 1) / gb::LARGE_PACK_PPI;
                    std::vector<u8> gaps(std::max<u64>(items, 1) * gb::LARGE_PACK_PPI, 0);
                    std::vector<u32> bases(std::max<u64>((u64(lc) + gb::LARGE_PACK_GROUP - 1) / gb::LARGE_PACK_GROUP, 1), 0);
                    for (u64 i = 0; i < lc; ++i) {
                        const u32 p = view.data()[split + i];
                        if (i % gb::LARGE_PACK_GROUP == 0) { bases[i / gb::LARGE_PACK_GROUP] = p; continue; }
                        const u32 gap = p - view.data()[split + i - 1];
                        if (gap / 2 > 0xFF) { std::fprintf(stderr, "gap too large\n"); return 1; }
                        gaps[i] = u8(gap / 2);
                    }
                    d_pk_bases = static_cast<u32*>(ctx.alloc_device(bases.size() * 4));
                    d_pk_gaps  = static_cast<u32*>(ctx.alloc_device(gaps.size()));
                    st_b = ctx.alloc_shared(bases.size() * 4);
                    st_g = ctx.alloc_shared(gaps.size());
                    std::memcpy(st_b, bases.data(), bases.size() * 4);
                    std::memcpy(st_g, gaps.data(), gaps.size());
                    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_pk_bases, st_b,
                                                           bases.size() * 4, nullptr, 0, nullptr));
                    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_pk_gaps, st_g,
                                                           gaps.size(), nullptr, 0, nullptr));
                    ZE_CHECK(zeCommandListAppendBarrier(ctx.list(), nullptr, 0, nullptr));
                    k_large.set_arg(1, sizeof(d_pk_bases), &d_pk_bases);
                    k_large.set_arg(2, sizeof(d_pk_gaps), &d_pk_gaps);
                    k_large.set_arg(3, sizeof(lc), &lc);
                }
                k_large.set_arg(4, sizeof(q_odds_arg), &q_odds_arg);
                k_large.set_arg(5, sizeof(c.q_first), &c.q_first);
                k_large.set_arg(6, sizeof(q_last_arg), &q_last_arg);
                k_large.set_arg(7, 4, &cm);
                k_large.set_arg(8, 4, &ga1);
                k_large.set_arg(9, 4, &ga2);
                k_large.set_arg(10, 4, &off5);
                const u64 large_count = lc;
                const u64 ppi = large_plain ? gb::LARGE_PRIMES_PER_ITEM : gb::LARGE_PACK_PPI;
                const u64 large_items = (large_count + ppi - 1) / ppi;
                k_large.launch(ctx.list(), u32((large_items + gb::LARGE_WG_SIZE - 1) / gb::LARGE_WG_SIZE));
            }
            ctx.host_sync();
            for (void* ptr : {(void*)d_pk_bases, (void*)d_pk_gaps, st_b, st_g})
                if (ptr) gb::Context::free_mem(ctx.context(), ptr);

            // ---- compare vs Miller-Rabin reference ----------------------
            for (void*& gbp : grp_bufs) if (gbp) { gb::Context::free_mem(ctx.context(), gbp); gbp = nullptr; }
            void* out = ctx.alloc_shared(map_words * 8);
            ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), out, map, map_words * 8,
                                                   nullptr, 0, nullptr));
            ctx.host_sync();
            const u64* got = static_cast<const u64*>(out);   // (class mode: remapped below)
            const std::vector<u64> ref = reference_map(c.q_first, c.q_odds);

            // Class mode: bring the class map back to dense coordinates (q = 3
            // (mod 6) bits are absent: compared as "not prime", which the
            // reference also says for every such q except q = 3).
            std::vector<u64> dense_got;
            u64 cls_tail = 0;
            if (cls) {
                dense_got.assign((c.q_odds + 63) / 64, 0);
                for (u32 k = 0; k < K; ++k) {
                    const u64* a = got + size_t(k) * c5w;
                    for (u64 t = 0; t < u64(tpc) * gb::BITS_PER_TILE; ++t) {
                        if (!((a[t >> 6] >> (t & 63)) & 1)) continue;
                        if (t >= nc[k]) { ++cls_tail; continue; }
                        const u64 i = DM * t + phi[k];
                        dense_got[i >> 6] |= 1ull << (i & 63);
                    }
                }
                got = dense_got.data();
            }
            u64 diffs = 0, over_mark = 0, missed_mark = 0;
            for (u64 i = 0; i < c.q_odds; ++i) {
                const bool g = (got[i >> 6] >> (i & 63)) & 1;
                bool r = (ref[i >> 6] >> (i & 63)) & 1;
                if (cls && (c.q_first + 2 * i) % 3 == 0) r = false;             // q = 3 not stored
                if (mode == 30 && (c.q_first + 2 * i) % 5 == 0) r = false;      // q = 5 not stored
                if (g != r) {
                    ++diffs;
                    if (g && !r) ++missed_mark;   // composite left "prime" (DANGEROUS)
                    else         ++over_mark;     // prime wrongly cleared (safe direction)
                    if (diffs <= 12)
                        std::printf("  q=%llu (bit %llu): gpu=%d ref=%d %s\n",
                                    (unsigned long long)(c.q_first + 2 * i),
                                    (unsigned long long)i, (int)g, (int)r,
                                    g ? "FALSE PRIME (missed mark)"
                                      : "false composite (over-marked)");
                }
            }
            // tail bits beyond q_odds must be 0 (verifier masks them)
            u64 tail_ones = cls_tail;
            for (u64 w = c.q_odds / 64; !cls && w < map_words; ++w) {
                const u64 valid_lo = (w == c.q_odds / 64) ? c.q_odds % 64 : 0;
                u64 v = got[w] & (valid_lo ? (~0ull << valid_lo) : ~0ull);
                tail_ones += __builtin_popcountll(v);
            }
            const bool ok = diffs == 0 && tail_ones == 0;
            std::printf("[%s%s] %s diffs=%llu (false_primes=%llu false_comps=%llu) "
                        "tail_ones=%llu\n",
                        c.name, mode == 30 ? ", class map mod 30" : cls ? ", class map" : "", ok ? "OK" : "FAILED",
                        (unsigned long long)diffs,
                        (unsigned long long)missed_mark,
                        (unsigned long long)over_mark,
                        (unsigned long long)tail_ones);
            // For false composites: which tile-range prime's mark sequence
            // covers the bit, per (a) the verifier's ceil formula and (b) an
            // independent brute-force first-multiple search? A mismatch
            // indicts the host formula; agreement indicts the kernel.
            u32 diag = 0;
            for (u64 i = 0; i < c.q_odds && diag < 4; ++i) {
                const bool g = (got[i >> 6] >> (i & 63)) & 1;
                const bool r = (ref[i >> 6] >> (i & 63)) & 1;
                if (g || r) continue;  // only false composites
                ++diag;
                const u64 q = c.q_first + 2 * i;
                for (u32 j = 0; j < split; ++j) {
                    const u64 p = view.data()[j];
                    // (a) verifier ceil formula
                    u64 first_a = p * p;
                    if (first_a < c.q_first) {
                        first_a = ((c.q_first + p - 1) / p) * p;
                        if ((first_a & 1) == 0) first_a += p;
                    }
                    // (b) brute force: smallest odd m with m*p >= q_first
                    u64 m = c.q_first / p;
                    while (m * p < c.q_first) ++m;
                    while (m > 1 && ((m - 2) * p) >= c.q_first) m -= 2;
                    if ((m & 1) == 0) ++m;
                    const u64 first_b = m * p;
                    if (first_b > q_last && first_a > q_last) continue;
                    const u64 fb_a = (first_a - c.q_first) >> 1;
                    const u64 fb_b = (first_b > q_last) ? ~0ull
                                                        : (first_b - c.q_first) >> 1;
                    const bool covers_a = fb_a <= i && (i - fb_a) % p == 0;
                    const bool covers_b = fb_b <= i && (i - fb_b) % p == 0;
                    if (covers_a || covers_b)
                        std::printf("    bit %llu (q=%llu): p=%u formula_fb=%llu "
                                    "brute_fb=%llu covers_a=%d covers_b=%d\n",
                                    (unsigned long long)i,
                                    (unsigned long long)q, view.data()[j],
                                    (unsigned long long)fb_a,
                                    (unsigned long long)fb_b,
                                    (int)covers_a, (int)covers_b);
                }
            }
            if (!ok) ++failures;

            gb::Context::free_mem(ctx.context(), map);
            gb::Context::free_mem(ctx.context(), d_primes);
            gb::Context::free_mem(ctx.context(), d_first);
            gb::Context::free_mem(ctx.context(), d_magic);
            gb::Context::free_mem(ctx.context(), d_shift);
            gb::Context::free_mem(ctx.context(), d_ms32);
            gb::Context::free_mem(ctx.context(), d_tbl);
            gb::Context::free_mem(ctx.context(), d_mask);
            if (d_gy) gb::Context::free_mem(ctx.context(), d_gy);
            gb::Context::free_mem(ctx.context(), out);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    if (failures) {
        std::printf("MAP FAILED: %d case(s)\n", failures);
        return 1;
    }
    std::printf("MAP OK\n");
    return 0;
}
