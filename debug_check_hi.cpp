// debug_check_hi — bit-exact validation of the mod-30 checker kernels
// (check30_a / check30_b) in the PRODUCTION geometry. Random maps are built
// as a dense q-bitset and converted to the eight mod-30 class arrays:
//   dense  (density ~1/21, like primes near 4e18) -> early-exit path,
//   sparse (density ~1/400) -> thousands of genuine leftovers, full loop.
// The reference is a plain per-even CPU scan of the same map (even i is
// resolved iff some odd candidate p has map bit i + (seg_start-p-q_first)/2
// set) — no words, funnels or subgroups. Compared: the whole checked[]
// bitset, the exact leftover set, and the coverage counters the verifier
// requires (acct[0] + pending == evens, acct[1] + leftovers == pending). The
// class map buffer has NO slack, so a stray read past it faults or shows up
// as a diff. (v2.6 removed the single-phase, twin and class-split checkers
// and their cases.)
//
// usage: debug_check_hi [spv]
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
    u32* d_acct = static_cast<u32*>(ctx.alloc_shared(8));
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
    ZE_CHECK(zeCommandListAppendMemoryFill(ctx.list(), d_acct, &zero, 4, 8, nullptr, 0, nullptr));
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
    ka.set_arg(15, sizeof(d_acct), &d_acct);
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
    kb.set_arg(23, sizeof(d_acct), &d_acct);
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
    // coverage counters: exactly what collect_slot requires of a segment
    const bool acct_ok = u64(d_acct[0]) + pend == c.seg_evens && u64(d_acct[1]) + n_left == pend;
    const bool ok = word_diffs == 0 && n_left == ref_left.size() && got_left == ref_left && layout_diffs == 0 &&
                    acct_ok;
    std::printf("  [%s, c30] %s evens=%llu pending=%u leftovers gpu=%u ref=%zu word_diffs=%llu layout_diffs=%llu "
                "acct=%u+%u%s\n",
                c.name, ok ? "OK" : "FAILED", (unsigned long long)c.seg_evens, pend, n_left, ref_left.size(),
                (unsigned long long)word_diffs, (unsigned long long)layout_diffs, d_acct[0], d_acct[1],
                acct_ok ? "" : " COVERAGE MISMATCH");
    for (void* ptr : {(void*)d_map, (void*)d_chk, (void*)d_c, (void*)d_meta, (void*)d_rest, (void*)d_rtab,
                      (void*)d_pend, (void*)d_pn, (void*)d_left, (void*)d_ln, (void*)d_acct})
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
    int failures = 0;
    try {
        gb::Context ctx;
        gb::Module mod(ctx, argc > 1 ? argv[1] : "kernels/goldbach_bmg.spv");
        // Mod-30 checker (production since v1.13.0): plans x cases.
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
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf(failures ? "CHECK_HI FAILED: %d case(s)\n" : "CHECK_HI OK\n", failures);
    return failures ? 1 : 0;
}
