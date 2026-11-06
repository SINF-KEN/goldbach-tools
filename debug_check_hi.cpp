// debug_check_hi — bit-exact validation of the checker kernels in the
// PRODUCTION (geo_safe) geometry, which debug_check (a [4, 1.2M] segment)
// never reaches. Random maps are uploaded straight into the q-bitset:
//   dense  (density ~1/21, like primes near 4e18) -> early-exit path,
//   sparse (density ~1/400) -> thousands of genuine leftovers, full loop.
// The reference is a plain per-even CPU scan of the same map (even i is
// resolved iff some odd candidate p has map bit i + (seg_start-p-q_first)/2
// set) — no words, funnels or subgroups. Compared: the whole checked[]
// bitset AND the exact leftover set. The map buffer is allocated with NO
// slack (exactly ceil(q_odds/64) words), so a stray read past the map
// either faults or shows up as a diff.
//
// The twin checker (phase A over the twin map with the lower twins, phase B
// over the uncovered candidates) is compared against the same reference.
//
// usage: debug_check_hi [spv] ; GB_CHECK_KERNEL=check_numbers to test the
// per-item checker (default: both kernels).
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "l0_context.hpp"
#include "primes.hpp"
#include "config.hpp"
#include "split_check.hpp"
#include "check30.hpp"

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;

namespace {

struct Case {
    u64 seg_start;
    u64 seg_evens;
    u32 density_inv;   // map bit set with probability 1/density_inv
    u32 gb_limit;      // candidate primes <= gb_limit (MAX_GB_PRIME = production)
    const char* name;
};

int run_case(gb::Context& ctx, gb::Kernel& kc, gb::Kernel* kb, u32 cap,
             const Case& c, u32 seed, bool twin = false) {
    const u64 seg_end = c.seg_start + (c.seg_evens - 1) * 2;
    const u64 q_first = (c.seg_start >= gb::MAX_GB_PRIME + 2) ? c.seg_start - gb::MAX_GB_PRIME : 3;
    const u64 q_last = seg_end - 3;
    const u64 q_odds = (q_last - q_first) / 2 + 1;
    const u64 map_words = (q_odds + 63) / 64;
    const u64 chk_words = (c.seg_evens + 63) / 64;

    std::mt19937_64 rng(seed);
    std::vector<u64> map(map_words, 0);
    for (u64 i = 0; i < q_odds; ++i)
        if (rng() % c.density_inv == 0) map[i >> 6] |= 1ull << (i & 63);

    std::vector<u16> gbp;
    for (u32 p : gb::odd_primes_up_to(c.gb_limit)) gbp.push_back(u16(p));
    const u32 gb_count = u32(gbp.size());
    // Twin checker (v1.10.0, production): phase A gets the lower twins and
    // reads the map as the twin map; phase B probes the candidates the first
    // `cap` twin pairs did not cover. Same final result as the reference.
    std::vector<u16> twl, rest;
    for (size_t i = 0; i + 1 < gbp.size(); ++i)
        if (gbp[i + 1] == gbp[i] + 2) twl.push_back(gbp[i]);
    {
        std::vector<bool> cov(65536, false);
        for (size_t i = 0; i < std::min<size_t>(cap, twl.size()); ++i) cov[twl[i]] = cov[twl[i] + 2] = true;
        for (u16 p : gbp) if (!cov[p]) rest.push_back(p);
    }

    // CPU reference: per even, first candidate whose q bit is set.
    std::vector<u64> ref(chk_words, 0);
    std::vector<u32> ref_left;
    for (u64 i = 0; i < c.seg_evens; ++i) {
        bool ok = false;
        for (u16 p : gbp) {
            if (p == 2) continue;
            const long long qb = (long long)i + ((long long)c.seg_start - p - (long long)q_first) / 2;
            if (qb < 0 || u64(qb) >= q_odds) continue;
            if ((map[u64(qb) >> 6] >> (u64(qb) & 63)) & 1) { ok = true; break; }
        }
        if (ok) ref[i >> 6] |= 1ull << (i & 63);
        else ref_left.push_back(u32(i));
    }

    u64* d_map = static_cast<u64*>(ctx.alloc_device(map_words * 8));
    u64* d_chk = static_cast<u64*>(ctx.alloc_device(chk_words * 8));
    u16* d_gb  = static_cast<u16*>(ctx.alloc_device(gbp.size() * 2));
    u32* d_left = static_cast<u32*>(ctx.alloc_device(size_t(gb::MAX_LEFTOVERS) * 4));
    u32* d_ln   = static_cast<u32*>(ctx.alloc_shared(4));
    void* st1 = ctx.alloc_shared(map_words * 8);
    void* st2 = ctx.alloc_shared(gbp.size() * 2);
    std::memcpy(st1, map.data(), map_words * 8);
    std::memcpy(st2, gbp.data(), gbp.size() * 2);
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_map, st1, map_words * 8, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_gb, st2, gbp.size() * 2, nullptr, 0, nullptr));
    const u64 ones = ~0ull;  // poison checked[]: every word must be written
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_chk, &ones, 8, chk_words * 8, nullptr, 0, nullptr));
    const u32 zero = 0;
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_ln, &zero, 4, 4, nullptr, 0, nullptr));
    ctx.host_sync();

    u64 qf = q_first, qo = q_odds, ss = c.seg_start, se = c.seg_evens;
    const u64 epg = u64(gb::WG_SIZE) * gb::CHECK_WORDS_PER_ITEM * 64;   // >= groups any checker needs
    const u32 ridge_off = 0xFFFFFFFFu;   // ridge recording disabled (dummy buffer)
    u32* d_pend = nullptr; u32* d_pn = nullptr;
    const u32 pcap = 1u << 22;
    if (!kb) {
        kc.set_arg(0, sizeof(d_map), &d_map);
        kc.set_arg(1, sizeof(d_chk), &d_chk);
        kc.set_arg(2, sizeof(d_gb), &d_gb);
        kc.set_arg(3, sizeof(gb_count), &gb_count);
        kc.set_arg(4, sizeof(d_left), &d_left);
        kc.set_arg(5, sizeof(d_ln), &d_ln);
        kc.set_arg(6, sizeof(qf), &qf);
        kc.set_arg(7, sizeof(qo), &qo);
        kc.set_arg(8, sizeof(ss), &ss);
        kc.set_arg(9, sizeof(se), &se);
        kc.launch(ctx.list(), u32((c.seg_evens + epg - 1) / epg));
    } else {
        // two-phase: check_phase_a (capped) -> barrier -> check_phase_b
        d_pend = static_cast<u32*>(ctx.alloc_device(size_t(pcap) * 4));
        d_pn   = static_cast<u32*>(ctx.alloc_shared(4));
        ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_pn, &zero, 4, 4, nullptr, 0, nullptr));
        ZE_CHECK(zeCommandListAppendBarrier(ctx.list(), nullptr, 0, nullptr));
        u16* d_tw = nullptr; u16* d_rest = nullptr;
        const u32 tw_count = u32(twl.size()), rest_count = u32(rest.size()), tw_flag = twin ? 1u : 0u;
        if (twin) {
            d_tw   = static_cast<u16*>(ctx.alloc_device(std::max<size_t>(twl.size(), 1) * 2));
            d_rest = static_cast<u16*>(ctx.alloc_device(std::max<size_t>(rest.size(), 1) * 2));
            if (!twl.empty())
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_tw, twl.data(), twl.size() * 2, nullptr, 0, nullptr));
            if (!rest.empty())
                ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_rest, rest.data(), rest.size() * 2, nullptr, 0, nullptr));
            ctx.host_sync();
        }
        const u32 from = twin ? 0u : cap;
        kc.set_arg(0, sizeof(d_map), &d_map);
        kc.set_arg(1, sizeof(d_chk), &d_chk);
        kc.set_arg(2, sizeof(d_gb), twin ? &d_tw : &d_gb);
        kc.set_arg(3, sizeof(gb_count), twin ? &tw_count : &gb_count);
        kc.set_arg(4, sizeof(d_pend), &d_pend);
        kc.set_arg(5, sizeof(d_pn), &d_pn);
        kc.set_arg(6, sizeof(qf), &qf);
        kc.set_arg(7, sizeof(qo), &qo);
        kc.set_arg(8, sizeof(ss), &ss);
        kc.set_arg(9, sizeof(se), &se);
        kc.set_arg(10, sizeof(cap), &cap);
        kc.set_arg(11, sizeof(pcap), &pcap);
        kc.set_arg(12, sizeof(tw_flag), &tw_flag);
        const u64 epg_a = u64(gb::WG_SIZE) * gb::CHECK_PHASE_A_W64 * 64;   // phase A item width
        kc.launch(ctx.list(), u32((c.seg_evens + epg_a - 1) / epg_a));
        ZE_CHECK(zeCommandListAppendBarrier(ctx.list(), nullptr, 0, nullptr));
        kb->set_arg(0, sizeof(d_map), &d_map);
        kb->set_arg(1, sizeof(d_chk), &d_chk);
        kb->set_arg(2, sizeof(d_gb), twin ? &d_rest : &d_gb);
        kb->set_arg(3, sizeof(gb_count), twin ? &rest_count : &gb_count);
        kb->set_arg(4, sizeof(d_pend), &d_pend);
        kb->set_arg(5, sizeof(d_pn), &d_pn);
        kb->set_arg(6, sizeof(d_left), &d_left);
        kb->set_arg(7, sizeof(d_ln), &d_ln);
        kb->set_arg(8, sizeof(qf), &qf);
        kb->set_arg(9, sizeof(qo), &qo);
        kb->set_arg(10, sizeof(ss), &ss);
        kb->set_arg(11, sizeof(from), &from);
        kb->set_arg(12, sizeof(pcap), &pcap);
        kb->set_arg(13, sizeof(d_left), &d_left);   // ridge buffer (unused)
        kb->set_arg(14, 4, &ridge_off);
        kb->launch(ctx.list(), 64);   // grid-stride
        ctx.host_sync();
        if (d_tw) gb::Context::free_mem(ctx.context(), d_tw);
        if (d_rest) gb::Context::free_mem(ctx.context(), d_rest);
    }
    ctx.host_sync();
    if (d_pn && *d_pn > pcap) { std::printf("  pending overflow %u\n", *d_pn); return 1; }

    void* out = ctx.alloc_shared(chk_words * 8);
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), out, d_chk, chk_words * 8, nullptr, 0, nullptr));
    ctx.host_sync();
    const u64* got = static_cast<const u64*>(out);
    const u32 n_left = *d_ln;
    std::vector<u32> got_left(std::min<u32>(n_left, gb::MAX_LEFTOVERS));
    if (!got_left.empty()) {
        ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), got_left.data(), d_left,
                                               got_left.size() * 4, nullptr, 0, nullptr));
        ctx.host_sync();
    }
    std::sort(got_left.begin(), got_left.end());

    u64 word_diffs = 0;
    for (u64 w = 0; w < chk_words; ++w) {
        const u64 vb = (w == chk_words - 1) ? c.seg_evens - w * 64 : 64;
        const u64 m = vb >= 64 ? ~0ull : ((1ull << vb) - 1);
        if ((got[w] & m) != (ref[w] & m) || (got[w] & ~m) != 0) ++word_diffs;
    }
    const bool left_ok = n_left == ref_left.size() && got_left == ref_left;
    const bool ok = word_diffs == 0 && left_ok;
    std::printf("  [%s] %s evens=%llu leftovers gpu=%u ref=%zu word_diffs=%llu\n",
                c.name, ok ? "OK" : "FAILED", (unsigned long long)c.seg_evens,
                n_left, ref_left.size(), (unsigned long long)word_diffs);

    for (void* ptr : {(void*)d_map, (void*)d_chk, (void*)d_gb, (void*)d_left, (void*)d_ln, st1, st2, out})
        gb::Context::free_mem(ctx.context(), ptr);
    if (d_pend) gb::Context::free_mem(ctx.context(), d_pend);
    if (d_pn) gb::Context::free_mem(ctx.context(), d_pn);
    return ok ? 0 : 1;
}

// Class-split checker (v1.11.0): split_map -> check_split_a -> check_split_b
// against the same per-even CPU reference. mod3 = 1 zeroes every map bit of
// a q divisible by 3 (as in a real map; the phase-A "useful residue" logic
// only matters then), 0 keeps the fully random map (phase B's futile tail
// must then still find those witnesses). checked[] comes back in the class
// layout and is converted to the dense even index for the compare.
// cls = 1: the map is uploaded as the production class map (M1 / M5, whole
// tiles each, q = 3 (mod 6) not stored) — no split_map; phase A reads the
// class arrays directly and phase B probes them with the class conversion.
// Only meaningful with mod3 maps (a class map cannot hold q = 3 (mod 6) bits).
gb::Kernel* km = nullptr;   // merge30 (set in main)
int run_split_case(gb::Context& ctx, gb::Kernel& ks, gb::Kernel& ka, gb::Kernel& kb,
                   u32 cap, const char* stages, const Case& c, u32 seed, bool mod3,
                   bool cls = false, bool c30 = false) {
    // c30 (implies cls): the v1.12.0 production map — 8 class arrays mod 30;
    // merge30 rebuilds S1 / S5 (compared with the dense map), phase A reads
    // them, phase B probes the class arrays (q = 3, 5 not stored: prime).
    if (c30) cls = true;
    const u64 seg_end = c.seg_start + (c.seg_evens - 1) * 2;
    const u64 q_first = (c.seg_start >= gb::MAX_GB_PRIME + 2) ? c.seg_start - gb::MAX_GB_PRIME : 3;
    const u64 q_last = seg_end - 3;
    const u64 q_odds = (q_last - q_first) / 2 + 1;
    const u64 map_words = (q_odds + 63) / 64;
    const u64 chk_words = (c.seg_evens + 63) / 64;

    std::mt19937_64 rng(seed);
    std::vector<u64> map(map_words, 0);
    for (u64 i = 0; i < q_odds; ++i)
        if (rng() % c.density_inv == 0 && !(mod3 && (q_first + 2 * i) % 3 == 0 && q_first + 2 * i != 3) &&
            !(c30 && (q_first + 2 * i) % 5 == 0 && q_first + 2 * i != 5))
            map[i >> 6] |= 1ull << (i & 63);
    if (c30 && q_first <= 5) map[0] |= 1ull << ((5 - q_first) / 2);
    // A mod-3 map is a real map's shape: q = 3 is prime (the class map does
    // not store it; phase B treats q = 3 as prime).
    if (mod3 && q_first <= 3) map[0] |= 1ull << ((3 - q_first) / 2);

    std::vector<u32> gball = gb::odd_primes_up_to(c.gb_limit);
    const gb::SplitPlan plan = gb::make_split_plan(gball, cap, gb::parse_split_stages(stages));
    const gb::SplitSeg sg = gb::split_segment(c.seg_start, c.seg_evens, q_first, plan);
    if (!sg.ok) {
        std::printf("  [%s] skipped: split geometry rejected (twin path)\n", c.name);
        return 0;
    }

    std::vector<u64> ref(chk_words, 0);
    std::vector<u32> ref_left;
    for (u64 i = 0; i < c.seg_evens; ++i) {
        bool ok = false;
        for (u32 p : gball) {
            const long long qb = (long long)i + ((long long)c.seg_start - p - (long long)q_first) / 2;
            if (qb < 0 || u64(qb) >= q_odds) continue;
            if ((map[u64(qb) >> 6] >> (u64(qb) & 63)) & 1) { ok = true; break; }
        }
        if (ok) ref[i >> 6] |= 1ull << (i & 63);
        else ref_left.push_back(u32(i));
    }

    const u32 map32_words = u32(2 * map_words);
    u32 s_words = (map32_words + 2) / 3 + 1;
    // class map (cls): M1 at word 0, M5 at word c5w (u64), tpc whole tiles each
    u64 nc[2] = {0, 0};
    for (u32 k = 0; k < 2; ++k) {
        const u32 ph = k ? sg.phi5 : sg.phi1;
        nc[k] = q_odds > ph ? (q_odds - ph + 2) / 3 : 0;
    }
    const u32 tpc = u32(std::max<u64>((std::max(nc[0], nc[1]) + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE, 1));
    u32 c5w = tpc * gb::TILE_WORDS, off5 = c5w * 64u, cm = cls ? 1u : 0u;
    u32 ga1 = sg.phi1, ga2 = sg.phi5, kmap[2] = {0, 0};
    std::vector<u64> cmap;
    if (c30) {
        static const u32 R[8] = {1, 7, 11, 13, 17, 19, 23, 29};
        u32 phi[8]; u64 n30[8], nmax = 0; u64 ph2k = ~0ull;
        for (u32 k = 0; k < 8; ++k) {
            phi[k] = u32(((R[k] + 30 - q_first % 30) % 30) / 2);
            n30[k] = q_odds > phi[k] ? (q_odds - phi[k] + 14) / 15 : 0;
            nmax = std::max(nmax, n30[k]);
            ph2k = (ph2k & ~(0xFull << (4 * phi[k]))) | (u64(k) << (4 * phi[k]));
        }
        const u32 tpc30 = u32(std::max<u64>((nmax + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE, 1));
        c5w = tpc30 * gb::TILE_WORDS; off5 = c5w * 64u; cm = 2u;
        ga1 = u32(ph2k); ga2 = u32(ph2k >> 32);
        cmap.assign(size_t(8) * c5w, 0);
        for (u32 k = 0; k < 8; ++k)
            for (u64 t = 0; t < n30[k]; ++t) {
                const u64 i = 15 * t + phi[k];
                if ((map[i >> 6] >> (i & 63)) & 1) cmap[size_t(k) * c5w + (t >> 6)] |= 1ull << (t & 63);
            }
        for (int cc = 0; cc < 2; ++cc) {           // merge30 j -> class of (Q6 + 6j) mod 30
            const u64 Q6 = q_first + 2 * u64(cc ? sg.phi5 : sg.phi1);
            for (u32 j = 0; j < 5; ++j) {
                u32 k = 15;
                for (u32 kk = 0; kk < 8; ++kk) if (R[kk] == (Q6 + 6 * j) % 30) k = kk;
                kmap[cc] |= k << (4 * j);
            }
        }
    } else if (cls) {
        cmap.assign(size_t(2) * c5w, 0);
        for (u32 k = 0; k < 2; ++k)
            for (u64 t = 0; t < nc[k]; ++t) {
                const u64 i = 3 * t + (k ? sg.phi5 : sg.phi1);
                if ((map[i >> 6] >> (i & 63)) & 1) cmap[size_t(k) * c5w + (t >> 6)] |= 1ull << (t & 63);
            }
        s_words = 2 * c5w;
    }
    const u32 chk_stride = u32((c.seg_evens / 3 + 1 + 63) / 64 + 1);
    const u32 pcap = 1u << 22;
    const size_t up_words = cls ? cmap.size() : map_words;
    u64* d_map = static_cast<u64*>(ctx.alloc_device(up_words * 8));   // no slack
    u32* d_s1 = static_cast<u32*>(ctx.alloc_device(size_t(s_words) * 4));
    u32* d_s5 = static_cast<u32*>(ctx.alloc_device(size_t(s_words) * 4));
    u64* d_chk = static_cast<u64*>(ctx.alloc_device(size_t(3) * chk_stride * 8));
    u16* d_c = static_cast<u16*>(ctx.alloc_device(std::max<size_t>(plan.cands.size(), 1) * 2));
    u32* d_meta = static_cast<u32*>(ctx.alloc_device(plan.meta.size() * 4));
    u16* d_rest = static_cast<u16*>(ctx.alloc_device(std::max<size_t>(plan.rest.size(), 1) * 2));
    u32* d_pend = static_cast<u32*>(ctx.alloc_device(size_t(pcap) * 4));
    u32* d_pn = static_cast<u32*>(ctx.alloc_shared(4));
    u32* d_left = static_cast<u32*>(ctx.alloc_device(size_t(gb::MAX_LEFTOVERS) * 4));
    u32* d_ln = static_cast<u32*>(ctx.alloc_shared(4));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_map, cls ? cmap.data() : map.data(),
                                           up_words * 8, nullptr, 0, nullptr));
    if (!plan.cands.empty())
        ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_c, plan.cands.data(), plan.cands.size() * 2, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_meta, plan.meta.data(), plan.meta.size() * 4, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_rest, plan.rest.data(), plan.rest.size() * 2, nullptr, 0, nullptr));
    const u64 ones = ~0ull;   // poison: every checked word in range must be written
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_chk, &ones, 8, size_t(3) * chk_stride * 8, nullptr, 0, nullptr));
    const u32 poison = 0xA5A5A5A5u;   // split maps must be fully rewritten
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_s1, &poison, 4, size_t(s_words) * 4, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_s5, &poison, 4, size_t(s_words) * 4, nullptr, 0, nullptr));
    const u32 zero = 0;
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_pn, &zero, 4, 4, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_ln, &zero, 4, 4, nullptr, 0, nullptr));
    const u32 ridge_off = 0xFFFFFFFFu;   // ridge recording disabled (dummy buffer)
    ctx.host_sync();

    ks.set_arg(0, sizeof(d_map), &d_map);
    ks.set_arg(1, 4, &map32_words);
    ks.set_arg(2, sizeof(d_s1), &d_s1);
    ks.set_arg(3, sizeof(d_s5), &d_s5);
    ks.set_arg(4, 4, &s_words);
    ks.set_arg(5, 4, &sg.phi1);
    ks.set_arg(6, 4, &sg.phi5);
    if (!cls) {
        ks.launch(ctx.list(), (s_words + gb::WG_SIZE - 1) / gb::WG_SIZE);
        ZE_CHECK(zeCommandListAppendBarrier(ctx.list(), nullptr, 0, nullptr));
    }
    if (c30) {
        const u32 cw32 = 2 * c5w;
        km->set_arg(0, sizeof(d_map), &d_map);
        km->set_arg(1, 4, &cw32);
        km->set_arg(2, sizeof(d_s1), &d_s1);
        km->set_arg(3, sizeof(d_s5), &d_s5);
        km->set_arg(4, 4, &s_words);
        km->set_arg(5, 4, &kmap[0]);
        km->set_arg(6, 4, &kmap[1]);
        const u64 Q65 = q_first + 2 * u64(sg.phi5);
        const u32 q5_t = (q_first <= 5 && Q65 <= 5) ? u32((5 - Q65) / 6) : ~0u;
        km->set_arg(7, 4, &q5_t);
        km->launch(ctx.list(), (2 * ((s_words + 4) / 5) + gb::WG_SIZE - 1) / gb::WG_SIZE);
        ZE_CHECK(zeCommandListAppendBarrier(ctx.list(), nullptr, 0, nullptr));
    }
    u32* a_s1 = d_s1; u32* a_s5 = d_s5;
    if (cls && !c30) { a_s1 = reinterpret_cast<u32*>(d_map); a_s5 = a_s1 + 2 * size_t(c5w); }
    const u32 per_item = gb::CHECK_PHASE_A_W64 * 64;
    const u32 ec_max = std::max({sg.ec[0], sg.ec[1], sg.ec[2]});
    const u32 items_pad = ((ec_max + per_item - 1) / per_item + gb::WG_SIZE - 1) / gb::WG_SIZE * gb::WG_SIZE;
    const u32 wc = 1;
    ka.set_arg(0, sizeof(a_s1), &a_s1);
    ka.set_arg(1, sizeof(a_s5), &a_s5);
    ka.set_arg(2, 4, &s_words);
    ka.set_arg(3, sizeof(d_chk), &d_chk);
    ka.set_arg(4, 4, &chk_stride);
    ka.set_arg(5, sizeof(d_c), &d_c);
    ka.set_arg(6, sizeof(d_meta), &d_meta);
    ka.set_arg(7, sizeof(d_pend), &d_pend);
    ka.set_arg(8, sizeof(d_pn), &d_pn);
    ka.set_arg(9, 4, &pcap);
    ka.set_arg(10, 4, &items_pad);
    for (u32 k = 0; k < 3; ++k) {
        ka.set_arg(11 + k, 4, &sg.ec[k]);
        ka.set_arg(14 + k, 4, &sg.e0[k]);
        ka.set_arg(17 + k, 4, &sg.v[k]);
    }
    ka.set_arg(20, 4, &wc);
    ka.launch(ctx.list(), 3 * items_pad / gb::WG_SIZE);
    ZE_CHECK(zeCommandListAppendBarrier(ctx.list(), nullptr, 0, nullptr));
    u64 qf = q_first, qo = q_odds, ss = c.seg_start;
    kb.set_arg(0, sizeof(d_map), &d_map);
    kb.set_arg(1, sizeof(d_chk), &d_chk);
    kb.set_arg(2, 4, &chk_stride);
    kb.set_arg(3, sizeof(d_rest), &d_rest);
    for (u32 k = 0; k < 3; ++k) {
        kb.set_arg(4 + k, 4, &plan.roff[k]);
        kb.set_arg(7 + k, 4, &plan.rcnt[k]);
    }
    kb.set_arg(10, sizeof(d_pend), &d_pend);
    kb.set_arg(11, sizeof(d_pn), &d_pn);
    kb.set_arg(12, sizeof(d_left), &d_left);
    kb.set_arg(13, sizeof(d_ln), &d_ln);
    kb.set_arg(14, 8, &qf);
    kb.set_arg(15, 8, &qo);
    kb.set_arg(16, 8, &ss);
    kb.set_arg(17, 4, &pcap);
    for (u32 k = 0; k < 3; ++k) kb.set_arg(18 + k, 4, &sg.e0[k]);
    kb.set_arg(21, 4, &wc);
    kb.set_arg(22, 4, &cm);
    kb.set_arg(23, 4, &ga1);
    kb.set_arg(24, 4, &ga2);
    kb.set_arg(25, 4, &off5);
    kb.set_arg(26, sizeof(d_left), &d_left);   // ridge buffer (unused)
    kb.set_arg(27, 4, &ridge_off);
    kb.launch(ctx.list(), 64);
    ctx.host_sync();
    const u32 pend = *d_pn;
    if (pend > pcap) { std::printf("  pending overflow %u\n", pend); return 1; }

    // split maps vs the host: S1/S5 bit t = dense bit 3t + phi (0 past the map)
    std::vector<u32> s1(s_words), s5(s_words);
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), s1.data(), d_s1, size_t(s_words) * 4, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), s5.data(), d_s5, size_t(s_words) * 4, nullptr, 0, nullptr));
    std::vector<u64> chk(size_t(3) * chk_stride);
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), chk.data(), d_chk, chk.size() * 8, nullptr, 0, nullptr));
    ctx.host_sync();
    u64 split_diffs = 0;
    for (u32 t = 0; (!cls || c30) && t < s_words * 32u; ++t)
        for (int w = 0; w < 2; ++w) {
            const u64 b = 3ull * t + (w ? sg.phi5 : sg.phi1);
            u32 want = b < map_words * 64 ? u32((map[b >> 6] >> (b & 63)) & 1) : 0u;
            // (q = 5: not stored mod 30, but merge30 restores it in S5)
            const u32 got = ((w ? s5 : s1)[t >> 5] >> (t & 31)) & 1u;
            if (want != got) ++split_diffs;
        }

    std::vector<u64> dense(chk_words, 0);
    u64 layout_diffs = 0;
    for (u32 k3 = 0; k3 < 3; ++k3) {
        const u32 words = (sg.ec[k3] + 63) / 64;
        for (u32 w = 0; w < words; ++w) {
            const u64 v = chk[size_t(k3) * chk_stride + w];
            const u32 nb = std::min<u32>(64, sg.ec[k3] - w * 64);
            if (nb < 64 && (v >> nb)) ++layout_diffs;          // bits past the class end
            for (u32 j = 0; j < nb; ++j)
                if ((v >> j) & 1) {
                    const u64 e = sg.e0[k3] + 3ull * (w * 64ull + j);
                    dense[e >> 6] |= 1ull << (e & 63);
                }
        }
    }
    const u32 n_left = *d_ln;
    std::vector<u32> got_left(std::min<u32>(n_left, gb::MAX_LEFTOVERS));
    if (!got_left.empty()) {
        ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), got_left.data(), d_left,
                                               got_left.size() * 4, nullptr, 0, nullptr));
        ctx.host_sync();
    }
    std::sort(got_left.begin(), got_left.end());
    u64 word_diffs = 0;
    for (u64 w = 0; w < chk_words; ++w) if (dense[w] != ref[w]) ++word_diffs;
    const bool left_ok = n_left == ref_left.size() && got_left == ref_left;
    const bool ok = word_diffs == 0 && left_ok && split_diffs == 0 && layout_diffs == 0;
    std::printf("  [%s%s%s] %s evens=%llu pending=%u leftovers gpu=%u ref=%zu word_diffs=%llu split_diffs=%llu layout_diffs=%llu\n",
                c.name, mod3 ? ", mod-3 map" : "", c30 ? ", class map mod 30" : cls ? ", class map" : "", ok ? "OK" : "FAILED", (unsigned long long)c.seg_evens,
                pend, n_left, ref_left.size(), (unsigned long long)word_diffs,
                (unsigned long long)split_diffs, (unsigned long long)layout_diffs);
    for (void* ptr : {(void*)d_map, (void*)d_s1, (void*)d_s5, (void*)d_chk, (void*)d_c, (void*)d_meta,
                      (void*)d_rest, (void*)d_pend, (void*)d_pn, (void*)d_left, (void*)d_ln})
        gb::Context::free_mem(ctx.context(), ptr);
    return ok ? 0 : 1;
}


// Mod-30 checker (v1.13.0): check30_a -> check30_b on the production mod-30
// class map (8 arrays, q = 3, 5 not stored: prime), evens grouped by n mod 30,
// against the per-even CPU reference. The random map has the real map's
// shape (no q divisible by 3 or 5 except q = 3, 5). checked[] comes back in
// the class layout (class cr at cr * stride words, bit i = even e0 + 15 i).
int run_c30_case(gb::Context& ctx, gb::Kernel& ka, gb::Kernel& kb, const std::string& spec,
                 const Case& c, u32 seed) {
    const u64 seg_end = c.seg_start + (c.seg_evens - 1) * 2;
    const u64 q_first = (c.seg_start >= gb::MAX_GB_PRIME + 2) ? c.seg_start - gb::MAX_GB_PRIME : 3;
    const u64 q_last = seg_end - 3;
    const u64 q_odds = (q_last - q_first) / 2 + 1;
    const u64 map_words = (q_odds + 63) / 64;
    const u64 chk_words = (c.seg_evens + 63) / 64;
    std::mt19937_64 rng(seed);
    std::vector<u64> map(map_words, 0);
    for (u64 i = 0; i < q_odds; ++i) {
        const u64 q = q_first + 2 * i;
        const bool unit = q % 3 && q % 5;
        if ((unit && rng() % c.density_inv == 0) || q == 3 || q == 5) map[i >> 6] |= 1ull << (i & 63);
    }
    std::vector<u32> gball = gb::odd_primes_up_to(c.gb_limit);
    const gb::Plan30 plan = gb::make_plan30(gball, spec);
    const gb::Seg30 sg = gb::seg30(c.seg_start, c.seg_evens, q_first);
    if (!sg.ok) { std::printf("  [%s] c30 geometry rejected\n", c.name); return 1; }

    std::vector<u64> ref(chk_words, 0);
    std::vector<u32> ref_left;
    for (u64 i = 0; i < c.seg_evens; ++i) {
        bool ok = false;
        for (u32 p : gball) {
            const long long qb = (long long)i + ((long long)c.seg_start - p - (long long)q_first) / 2;
            if (qb < 0 || u64(qb) >= q_odds) continue;
            if ((map[u64(qb) >> 6] >> (u64(qb) & 63)) & 1) { ok = true; break; }
        }
        if (ok) ref[i >> 6] |= 1ull << (i & 63);
        else ref_left.push_back(u32(i));
    }
    // class map, as the verifier's class_geometry(K = 8)
    static const u32 R[8] = {1, 7, 11, 13, 17, 19, 23, 29};
    u32 phi[8]; u64 n30[8], nmax = 0, ph2k = ~0ull;
    for (u32 k = 0; k < 8; ++k) {
        phi[k] = u32(((R[k] + 30 - q_first % 30) % 30) / 2);
        n30[k] = q_odds > phi[k] ? (q_odds - phi[k] + 14) / 15 : 0;
        nmax = std::max(nmax, n30[k]);
        ph2k = (ph2k & ~(0xFull << (4 * phi[k]))) | (u64(k) << (4 * phi[k]));
        if (((sg.phik >> (4 * k)) & 15u) != phi[k]) { std::printf("  seg30 phik mismatch\n"); return 1; }
    }
    const u32 tpc30 = u32(std::max<u64>((nmax + gb::BITS_PER_TILE - 1) / gb::BITS_PER_TILE, 1));
    const u32 c5w = tpc30 * gb::TILE_WORDS, off5 = c5w * 64u, cw32 = 2 * c5w;
    const u32 ga1 = u32(ph2k), ga2 = u32(ph2k >> 32);
    std::vector<u64> cmap(size_t(8) * c5w, 0);
    for (u32 k = 0; k < 8; ++k)
        for (u64 t = 0; t < n30[k]; ++t) {
            const u64 i = 15 * t + phi[k];
            if ((map[i >> 6] >> (i & 63)) & 1) cmap[size_t(k) * c5w + (t >> 6)] |= 1ull << (t & 63);
        }
    const u32 stride = u32((c.seg_evens / 15 + 1 + 63) / 64 + 1);
    const u32 pcap = 1u << 22;
    u64* d_map = static_cast<u64*>(ctx.alloc_device(cmap.size() * 8));   // no slack
    u64* d_chk = static_cast<u64*>(ctx.alloc_device(size_t(15) * stride * 8));
    u16* d_c = static_cast<u16*>(ctx.alloc_device(std::max<size_t>(plan.cands.size(), 1) * 2));
    u32* d_meta = static_cast<u32*>(ctx.alloc_device(plan.meta.size() * 4));
    u16* d_rest = static_cast<u16*>(ctx.alloc_device(std::max<size_t>(plan.rest.size(), 1) * 2));
    u32* d_rtab = static_cast<u32*>(ctx.alloc_device(plan.rtab.size() * 4));
    u32* d_pend = static_cast<u32*>(ctx.alloc_device(size_t(pcap) * 4));
    u32* d_pn = static_cast<u32*>(ctx.alloc_shared(4));
    u32* d_left = static_cast<u32*>(ctx.alloc_device(size_t(gb::MAX_LEFTOVERS) * 4));
    u32* d_ln = static_cast<u32*>(ctx.alloc_shared(4));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_map, cmap.data(), cmap.size() * 8, nullptr, 0, nullptr));
    if (!plan.cands.empty())
        ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_c, plan.cands.data(), plan.cands.size() * 2, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_meta, plan.meta.data(), plan.meta.size() * 4, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_rest, plan.rest.data(), plan.rest.size() * 2, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), d_rtab, plan.rtab.data(), plan.rtab.size() * 4, nullptr, 0, nullptr));
    const u64 ones = ~0ull;   // poison: every checked word in range must be written
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_chk, &ones, 8, size_t(15) * stride * 8, nullptr, 0, nullptr));
    const u32 zero = 0;
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_pn, &zero, 4, 4, nullptr, 0, nullptr));
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_ln, &zero, 4, 4, nullptr, 0, nullptr));
    ctx.host_sync();
    const u32 per_item = gb::CHECK_PHASE_A_W64 * 64;
    const u32 items_pad = ((sg.ec_max + per_item - 1) / per_item + gb::WG_SIZE - 1) / gb::WG_SIZE * gb::WG_SIZE;
    const u32 wc = 1, se32 = u32(c.seg_evens);
    const u32 ridge_off = 0xFFFFFFFFu;   // ridge recording disabled (dummy buffer)
    ka.set_arg(0, sizeof(d_map), &d_map);
    ka.set_arg(1, 4, &cw32);
    ka.set_arg(2, sizeof(d_chk), &d_chk);
    ka.set_arg(3, 4, &stride);
    ka.set_arg(4, sizeof(d_c), &d_c);
    ka.set_arg(5, sizeof(d_meta), &d_meta);
    ka.set_arg(6, sizeof(d_pend), &d_pend);
    ka.set_arg(7, sizeof(d_pn), &d_pn);
    ka.set_arg(8, 4, &pcap);
    ka.set_arg(9, 4, &items_pad);
    ka.set_arg(10, 4, &se32);
    ka.set_arg(11, 4, &sg.sm30);
    ka.set_arg(12, 4, &sg.tb);
    ka.set_arg(13, 4, &sg.phik);
    ka.set_arg(14, 4, &wc);
    ka.launch(ctx.list(), 15 * items_pad / gb::WG_SIZE);
    ZE_CHECK(zeCommandListAppendBarrier(ctx.list(), nullptr, 0, nullptr));
    u64 qf = q_first, qo = q_odds, ss = c.seg_start;
    kb.set_arg(0, sizeof(d_map), &d_map);
    kb.set_arg(1, sizeof(d_chk), &d_chk);
    kb.set_arg(2, 4, &stride);
    kb.set_arg(3, sizeof(d_rest), &d_rest);
    kb.set_arg(4, sizeof(d_rtab), &d_rtab);
    kb.set_arg(5, sizeof(d_pend), &d_pend);
    kb.set_arg(6, sizeof(d_pn), &d_pn);
    kb.set_arg(7, sizeof(d_left), &d_left);
    kb.set_arg(8, sizeof(d_ln), &d_ln);
    kb.set_arg(9, 8, &qf);
    kb.set_arg(10, 8, &qo);
    kb.set_arg(11, 8, &ss);
    kb.set_arg(12, 4, &pcap);
    kb.set_arg(13, 4, &wc);
    kb.set_arg(14, 4, &ga1);
    kb.set_arg(15, 4, &ga2);
    kb.set_arg(16, 4, &off5);
    kb.set_arg(17, sizeof(d_left), &d_left);   // ridge buffer (unused)
    kb.set_arg(18, 4, &ridge_off);
    for (u32 k = 19; k < 22; ++k) kb.set_arg(k, sizeof(d_left), &d_left);   // tail statistics (off)
    kb.set_arg(22, 4, &ridge_off);             // tail bound ~0u: no statistics writes
    kb.launch(ctx.list(), 64);
    ctx.host_sync();
    const u32 pend = *d_pn;
    if (pend > pcap) { std::printf("  pending overflow %u\n", pend); return 1; }
    std::vector<u64> chk(size_t(15) * stride);
    ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), chk.data(), d_chk, chk.size() * 8, nullptr, 0, nullptr));
    ctx.host_sync();
    std::vector<u64> dense(chk_words, 0);
    u64 layout_diffs = 0;
    const u32 sm30 = u32(c.seg_start % 30);
    for (u32 cr = 0; cr < 15; ++cr) {
        const u32 e0 = ((2 * cr + 30 - sm30) % 30) / 2;
        const u32 ec = c.seg_evens > e0 ? u32((c.seg_evens - e0 + 14) / 15) : 0u;
        for (u32 w = 0; w < (ec + 63) / 64; ++w) {
            const u64 v = chk[size_t(cr) * stride + w];
            const u32 nb = std::min<u32>(64, ec - w * 64);
            if (nb < 64 && (v >> nb)) ++layout_diffs;
            for (u32 j = 0; j < nb; ++j)
                if ((v >> j) & 1) {
                    const u64 e = e0 + 15ull * (w * 64ull + j);
                    dense[e >> 6] |= 1ull << (e & 63);
                }
        }
    }
    const u32 n_left = *d_ln;
    std::vector<u32> got_left(std::min<u32>(n_left, gb::MAX_LEFTOVERS));
    if (!got_left.empty()) {
        ZE_CHECK(zeCommandListAppendMemoryCopy(ctx.list(), got_left.data(), d_left, got_left.size() * 4, nullptr, 0, nullptr));
        ctx.host_sync();
    }
    std::sort(got_left.begin(), got_left.end());
    u64 word_diffs = 0;
    for (u64 w = 0; w < chk_words; ++w) if (dense[w] != ref[w]) ++word_diffs;
    const bool ok = word_diffs == 0 && n_left == ref_left.size() && got_left == ref_left && layout_diffs == 0;
    std::printf("  [%s, c30] %s evens=%llu pending=%u leftovers gpu=%u ref=%zu word_diffs=%llu layout_diffs=%llu\n",
                c.name, ok ? "OK" : "FAILED", (unsigned long long)c.seg_evens, pend, n_left, ref_left.size(),
                (unsigned long long)word_diffs, (unsigned long long)layout_diffs);
    for (void* ptr : {(void*)d_map, (void*)d_chk, (void*)d_c, (void*)d_meta, (void*)d_rest, (void*)d_rtab,
                      (void*)d_pend, (void*)d_pn, (void*)d_left, (void*)d_ln})
        gb::Context::free_mem(ctx.context(), ptr);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const Case cases[] = {
        {4000000000000000000ull, 1000000, 21,  gb::MAX_GB_PRIME, "4e18 dense, 1M evens"},
        {4000000000000000000ull,  300017, 400, gb::MAX_GB_PRIME, "4e18 sparse, odd tail"},
        {4999999999999000000ull,  262144, 21,  gb::MAX_GB_PRIME, "5e18 dense, exact groups"},
        {4999999999999000000ull,  65536 * 3 + 64 * 5 + 1, 150, gb::MAX_GB_PRIME, "5e18 medium, 1-even tail"},
        {4000000000000000000ull,  100000, 21,  500,  "4e18 short candidate list"},
        {4000000000000000000ull,  100000, 21, 2500, "4e18 floor-list 2500"},
        {4000000000169000000ull / 10ull, 100000, 21, 2500, "4e18 offset window"},
        {4000000000169000000ull / 10ull, 1500000, 300, 2500, "4e18 offset big sparse"},
        {4000000000000000000ull,  777,    30,  gb::MAX_GB_PRIME, "4e18 tiny segment"},
        {4,                        400000, 21,  gb::MAX_GB_PRIME, "low range (guarded path)"},
        {4,                        200003, 300, gb::MAX_GB_PRIME, "low range sparse"},
    };
    std::vector<std::string> kernels;
    if (const char* k = std::getenv("GB_CHECK_KERNEL")) kernels.push_back(k);
    else kernels = {"check_numbers_w32", "check_numbers"};
    int failures = 0;
    try {
        gb::Context ctx;
        gb::Module mod(ctx, argc > 1 ? argv[1] : "kernels/goldbach_bmg.spv");
        if (std::getenv("GB_SPLIT_ONLY") || std::getenv("GB_C30_ONLY")) kernels.clear();
        for (const std::string& kn : kernels) {
            gb::Kernel kc(mod, kn.c_str());
            kc.set_group_size(gb::WG_SIZE);
            std::printf("%s:\n", kn.c_str());
            u32 seed = 12345;
            for (const Case& c : cases) failures += run_case(ctx, kc, nullptr, 0, c, seed++);
        }
        // Two-phase production checker at several caps: 0 (everything in
        // phase B), small, production-like, and beyond the list (all phase A).
        if (!std::getenv("GB_CHECK_KERNEL") && !std::getenv("GB_SPLIT_ONLY") && !std::getenv("GB_C30_ONLY")) {
            gb::Kernel ka(mod, "check_phase_a");
            gb::Kernel kb(mod, "check_phase_b");
            ka.set_group_size(gb::WG_SIZE);
            kb.set_group_size(gb::WG_SIZE);
            for (u32 cap : {0u, 16u, 96u, 128u, 4000u}) {
                std::printf("two-phase cap=%u:\n", cap);
                u32 seed = 12345;
                for (const Case& c : cases) failures += run_case(ctx, ka, &kb, cap, c, seed++);
            }
            // Twin checker (v1.10.0; still the path where split geometry is rejected).
            for (u32 cap : {0u, 16u, 100u, 4000u}) {
                std::printf("twin two-phase cap=%u:\n", cap);
                u32 seed = 12345;
                for (const Case& c : cases) failures += run_case(ctx, ka, &kb, cap, c, seed++, true);
            }
        }
        // Mod-30 checker (production since v1.13.0): plans x cases.
        // GB_C30_ONLY=1 runs just these.
        {
            gb::Kernel k3a(mod, "check30_a"), k3b(mod, "check30_b");
            k3a.set_group_size(gb::WG_SIZE);
            k3b.set_group_size(gb::WG_SIZE);
            std::string empty, singles, deep;
            for (u32 cr = 0; cr < 15; ++cr) {
                const u32 r = 2 * cr;
                std::vector<u32> ok;
                for (u32 s : gb::C30_UNITS) if (gb::c30_unit(r + 30 - s)) ok.push_back(s);
                if (cr) { empty += ";"; singles += ";"; deep += ";"; }
                singles += std::to_string(ok[0]) + "/1x100," + std::to_string(ok.back()) + "/3x7";
                for (size_t i = 0; i < ok.size(); ++i)   // every useful candidate (<= 15 segments)
                    deep += (i ? "," : "") + (ok.size() <= 3 ? std::to_string(ok[i]) + "/31x400," +
                            std::to_string(ok[i]) + "/15x400," + std::to_string(ok[i]) + "/3x400,"
                            : std::string()) + std::to_string(ok[i]) + "/1x400";
            }
            // the verifier's ridge-pass floor plan: single-member funnels
            // over every useful candidate (verifier.cpp ridge tracking)
            std::string rspec;
            for (u32 cr = 0; cr < 15; ++cr) {
                const u32 r = 2 * cr;
                if (cr) rspec += ';';
                bool first = true;
                for (u32 s : gb::C30_UNITS)
                    if (gb::c30_unit(r + 30 - s)) {
                        rspec += (first ? "" : ",") + std::to_string(s) + "/1x999";
                        first = false;
                    }
            }
            const std::pair<const char*, std::string> plans[] = {
                {"default", gb::CHECK30_PLAN}, {"empty (all phase B)", empty},
                {"singles + pairs", singles}, {"deep (every candidate)", deep},
                {"ridge floor plan", rspec}};
            for (const auto& pl : plans) {
                std::printf("c30 plan %s:\n", pl.first);
                u32 seed = 3030;
                for (const Case& c : cases) failures += run_c30_case(ctx, k3a, k3b, pl.second, c, seed++);
            }
        }
        if (std::getenv("GB_C30_ONLY")) {
            std::printf(failures ? "CHECK_HI FAILED: %d case(s)\n" : "CHECK_HI OK\n", failures);
            return failures ? 1 : 0;
        }
        // Class-split checker (production since v1.11.0): stage sets x caps x
        // random / mod-3 maps. GB_SPLIT_ONLY=1 runs just these.
        {
            gb::Kernel ks(mod, "split_map"), ksa(mod, "check_split_a"), ksb(mod, "check_split_b");
            gb::Kernel kmerge(mod, "merge30");
            kmerge.set_group_size(gb::WG_SIZE);
            km = &kmerge;
            ks.set_group_size(gb::WG_SIZE);
            ksa.set_group_size(gb::WG_SIZE);
            ksb.set_group_size(gb::WG_SIZE);
            const std::pair<const char*, u32> runs[] = {
                {"3", 0}, {"3", 7}, {"3", gb::CHECK_SPLIT_CANDS}, {"3", 4000}, {"1", 50},
                {"7/3", 35}, {"7", 4000}, {"5/3/1", 4000}, {gb::CHECK_SPLIT_STAGES, gb::CHECK_SPLIT_CANDS}};
            for (const auto& r : runs) {
                std::printf("split stages=%s cap=%u:\n", r.first, r.second);
                u32 seed = 777;
                for (const Case& c : cases) {
                    for (bool m3 : {false, true})
                        failures += run_split_case(ctx, ks, ksa, ksb, r.second, r.first, c, seed++, m3);
                    failures += run_split_case(ctx, ks, ksa, ksb, r.second, r.first, c, seed++, true, true);
                    failures += run_split_case(ctx, ks, ksa, ksb, r.second, r.first, c, seed++, true, true, true);
                }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf(failures ? "CHECK_HI FAILED: %d case(s)\n" : "CHECK_HI OK\n", failures);
    return failures ? 1 : 0;
}
