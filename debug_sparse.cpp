// debug_sparse — validation of the sparse class map pipeline (--sparse).
//
//   map   : one full GPU segment (tile + large kernels) vs an independent CPU
//           sieve of the same class progressions (modular inverse by Fermat,
//           128-bit arithmetic), every bit; plus Miller-Rabin spot checks of
//           the CPU truth itself. Any false prime (GPU 1, truth 0) FAILS.
//   check : phase A + B on a given map vs a per-even CPU reference of the
//           plan (exact pending set, checked bits and leftover set), on the
//           real GPU map and on random maps (dense, sparse + truncated rest
//           lists to force leftovers).
//
// usage: debug_sparse map   START [DLT]        (default DLT: production 14M)
//        debug_sparse check START DLT [DENSITY_PERMILLE|real] [SEED]
// env: GB_SCM_* knobs as the verifier (e.g. GB_SCM_RESTCAP for leftovers).
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "primes.hpp"
#include "sparse.hpp"
#include "sparse_run.hpp"

using namespace gb;
using u64 = uint64_t;
using u32 = uint32_t;

static u32 env_m() { const char* e = std::getenv("GB_SCM_M"); return e ? u32(std::strtoul(e, nullptr, 0)) : SCM_M_DEFAULT; }
static u64 mulmod(u64 a, u64 b, u64 m) { return u64((unsigned __int128)a * b % m); }
static u64 powmod(u64 a, u64 e, u64 m) {
    u64 r = 1 % m; a %= m;
    while (e) { if (e & 1) r = mulmod(r, a, m); a = mulmod(a, a, m); e >>= 1; }
    return r;
}

template <typename Fn>
static void par(u32 n, Fn&& fn) {
    const u32 T = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> th;
    for (u32 t = 0; t < T; ++t) th.emplace_back([&, t] { fn(t, T); });
    for (auto& x : th) x.join();
    (void)n;
}

static inline bool gbit(const std::vector<u32>& map, u32 stride32, u32 k, u64 t) {
    return (map[size_t(k) * stride32 + (t >> 5)] >> (t & 31)) & 1u;
}

// ---------------------------------------------------------------------------
static int cmd_map(u64 start, u32 dlt) {
    const u64 limit = start + u64(scm_geo(env_m()).M) * dlt * 2;   // >= 2 segments
    ScmOptions o;
    o.dlt = dlt;
    ScmEngine eng(start, limit, o);
    const ScmGeo& G = eng.geo();
    eng.enqueue(0);
    ScmSegResult r = eng.collect(0);
    std::vector<u32> map;
    eng.read_map(0, map);
    const u32 st = eng.stride32();
    const u32 T = r.T;
    const u64 Q = r.N - u64(G.M) * eng.tb();
    std::printf("map: N=%" PRIu64 " Q=%" PRIu64 " T=%u (%u classes x %u bits), tile %.3f large %.3f ms\n",
                r.N, Q, T, G.K, T, r.tile_s * 1e3, r.large_s * 1e3);
    // CPU truth: every base prime p >= 13 up to sqrt(q_max)
    const u64 qmax = Q + G.res[G.K - 1] + u64(G.M) * (T - 1);
    const u64 sq = u64(std::sqrt((long double)qmax)) + 1;
    const u32* pr = eng.base_primes();
    const u32 np = eng.base_count();
    if (u64(pr[np - 1]) < sq) { std::printf("FAIL: base primes do not reach sqrt(q_max)\n"); return 1; }
    if (Q <= u64(pr[np - 1])) { std::printf("FAIL: segment overlaps the base primes\n"); return 1; }
    std::vector<uint8_t> truth(size_t(G.K) * T, 1);
    std::atomic<u64> nprimes_used{0};
    par(0, [&](u32 tid, u32 nt) {
        u64 used = 0;
        for (u32 j = tid; j < np; j += nt) {
            const u64 p = pr[j];
            if (p < 13 || G.M % p == 0) continue;           // primes of M never divide a class member
            if (p > sq) break;
            ++used;
            const u64 minv = powmod(G.M % p, p - 2, p);     // Fermat (independent of scm_invmod)
            for (u32 k = 0; k < G.K; ++k) {
                const u64 x = (Q % p + G.res[k]) % p;
                const u64 t0 = mulmod((p - x) % p, minv, p);
                for (u64 t = t0; t < T; t += p)
                    __atomic_store_n(&truth[size_t(k) * T + t], (uint8_t)0, __ATOMIC_RELAXED);
            }
        }
        nprimes_used += used;
    });
    u64 fp = 0, fc = 0, ones = 0;
    for (u32 k = 0; k < G.K; ++k)
        for (u64 t = 0; t < T; ++t) {
            const bool g = gbit(map, st, k, t), tr = truth[size_t(k) * T + t];
            ones += g;
            if (g && !tr) { if (fp < 10) std::printf("  FALSE PRIME k=%u t=%" PRIu64 " q=%" PRIu64 "\n", k, t, Q + G.res[k] + u64(G.M) * t); ++fp; }
            if (!g && tr) { if (fc < 10) std::printf("  false composite k=%u t=%" PRIu64 "\n", k, t); ++fc; }
        }
    // bits beyond T within the array must be 0
    u64 tail = 0;
    for (u32 k = 0; k < G.K; ++k)
        for (u64 t = T; t < u64(st) * 32; ++t) tail += gbit(map, st, k, t);
    // Miller-Rabin spot check of the truth
    std::mt19937_64 rng(12345);
    u64 mr_bad = 0, mr_n = 200000;
    for (u64 s = 0; s < mr_n; ++s) {
        const u32 k = u32(rng() % G.K);
        const u64 t = rng() % T;
        const u64 q = Q + G.res[k] + u64(G.M) * t;
        if (is_prime_u64(q) != bool(truth[size_t(k) * T + t])) ++mr_bad;
    }
    std::printf("map: %" PRIu64 " primes sieved, %" PRIu64 " set bits (%.4f%%), false primes %" PRIu64
                ", false composites %" PRIu64 ", tail bits %" PRIu64 ", MR spot check %" PRIu64 "/%" PRIu64 " bad\n",
                nprimes_used.load(), ones, 100.0 * ones / (double(G.K) * T), fp, fc, tail, mr_bad, mr_n);
    const bool ok = fp == 0 && fc == 0 && tail == 0 && mr_bad == 0;
    std::printf("%s\n", ok ? "MAP OK" : "MAP FAILED");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
static int cmd_check(u64 start, u32 dlt, int permille, u64 seed) {
    const u64 D = u64(scm_geo(env_m()).M) * dlt;
    // start / limit inside the segments (partial first and last segments)
    const u64 limit = start + 2 * D - 7 * u64(scm_geo(env_m()).M) + 1234;
    ScmOptions o;
    o.dlt = dlt;
    o.write_checked = true;
    ScmEngine eng(start, limit, o);
    const ScmGeo& G = eng.geo();
    const PlanSCM& plan = eng.plan();
    const u32 st = eng.stride32();
    bool all_ok = true;
    for (u64 s = 0; s < 2; ++s) {
        const u32 slot = u32(s & 1);
        std::vector<u32> map;
        ScmSegResult r;
        if (permille < 0) {
            eng.enqueue(s);
            r = eng.collect(slot);
            eng.read_map(slot, map);
        } else {
            map.assign(size_t(G.K) * st, 0);
            std::mt19937_64 rng(seed * 1000 + s);
            const u64 N = eng.n0() + s * D;
            u32 dl = dlt;
            if ((limit - N) / G.M + 1 < dl) dl = u32((limit - N) / G.M + 1);
            const u32 T = dl + eng.tb();
            for (u32 k = 0; k < G.K; ++k)
                for (u32 t = 0; t < T; ++t)
                    if (int(rng() % 1000) < permille) map[size_t(k) * st + (t >> 5)] |= 1u << (t & 31);
            eng.enqueue_check_only(s, map);
            r = eng.collect(slot);
        }
        std::vector<u64> chk, pend;
        eng.read_checked(slot, chk);
        eng.read_pending(slot, pend);
        const u32 cs = eng.chk_stride();
        // CPU reference
        std::vector<std::vector<u32>> ref_pend(G.CR()), ref_left(G.CR());
        std::atomic<u64> bad_chk{0}, nvalid{0};
        par(0, [&](u32 tid, u32 nt) {
            for (u32 cr = tid; cr < G.CR(); cr += nt) {
                const u64 n0 = r.N + 2ull * cr;
                u64 ilo = 0, ihi = 0;
                if (limit >= n0) ihi = std::min<u64>(r.dl, (limit - n0) / G.M + 1);
                if (start > n0) ilo = (start - n0 + G.M - 1) / G.M;
                if (ilo > ihi) ilo = ihi;
                const u32* meta = &plan.meta[size_t(cr) * SCM_META];
                for (u64 i = ilo; i < ihi; ++i) {
                    ++nvalid;
                    bool a = false;
                    for (u32 sg = 0; sg < meta[0] && !a; ++sg) {
                        const u32 jb = meta[1 + 3 * sg], je = meta[2 + 3 * sg], km = meta[3 + 3 * sg];
                        const u32 k = km & 0xFF, mask = (km >> 8) & 0xFF;
                        for (u32 j = jb; j < je && !a; ++j)
                            for (u32 d = 0; d <= SCM_MAXD; ++d)
                                if ((mask >> d) & 1u) {
                                    const u64 U = plan.cands[j];
                                    if (U < d) { a = false; }
                                    else if (gbit(map, st, k, U - d + i)) { a = true; break; }
                                }
                    }
                    bool ok = a;
                    if (!a) {
                        ref_pend[cr].push_back(u32(i));
                        const u32 ro = plan.rtab[cr], rc = plan.rtab[G.CR() + cr];
                        for (u32 j = 0; j < rc && !ok; ++j) {
                            const u32 v = plan.rest[ro + j];
                            ok = gbit(map, st, v >> 28, (v & 0x0FFFFFFFu) + i);
                        }
                        if (!ok) ref_left[cr].push_back(u32(i));
                    }
                    const bool g = (chk[size_t(cr) * cs + (i >> 6)] >> (i & 63)) & 1u;
                    if (g != ok) ++bad_chk;
                }
            }
        });
        // compare pending and leftover sets
        // pending entries: cr << 51 | word << 32 | miss mask -> expand to cr << 32 | i
        std::set<u64> gp, gl(r.left.begin(), r.left.end());
        u64 bad_entries = 0;
        const u32 psh = eng.pshift();
        for (u64 e : pend) {
            const u64 cr = e >> psh, w = (e >> 32) & ((1ull << (psh - 32)) - 1);
            u32 m = u32(e);
            if (!m) ++bad_entries;
            while (m) { const u32 b = __builtin_ctz(m); m &= m - 1; if (!gp.insert((cr << 32) | (w * 32 + b)).second) ++bad_entries; }
        }
        u64 npend = 0, nleft = 0, bad_p = 0, bad_l = 0;
        for (u32 cr = 0; cr < G.CR(); ++cr) {
            for (u32 i : ref_pend[cr]) { ++npend; if (!gp.count((u64(cr) << 32) | i)) ++bad_p; }
            for (u32 i : ref_left[cr]) { ++nleft; if (!gl.count((u64(cr) << 32) | i)) ++bad_l; }
        }
        const bool ok = bad_chk == 0 && bad_p == 0 && bad_l == 0 && npend == gp.size() &&
                        bad_entries == 0 && nleft == gl.size() && r.left.size() == gl.size();
        std::printf("check seg %" PRIu64 " (%s): valid evens %" PRIu64 ", pending %" PRIu64 " (gpu %zu), leftovers %" PRIu64
                    " (gpu %zu), checked mismatches %" PRIu64 ", missing pending %" PRIu64 ", missing leftovers %" PRIu64 " -> %s\n",
                    s, permille < 0 ? "real map" : "random map", nvalid.load(), npend, gp.size(), nleft,
                    r.left.size(), bad_chk.load(), bad_p, bad_l, ok ? "ok" : "FAIL");
        all_ok = all_ok && ok;
    }
    std::printf("%s\n", all_ok ? "CHECK OK" : "CHECK FAILED");
    return all_ok ? 0 : 1;
}

int main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string(argv[1]) == "map")
            return cmd_map(std::strtoull(argv[2], nullptr, 10),
                           argc > 3 ? u32(std::strtoul(argv[3], nullptr, 10)) : 14000000u);
        if (argc >= 4 && std::string(argv[1]) == "check") {
            int pm = -1;
            if (argc > 4 && std::string(argv[4]) != "real") pm = std::atoi(argv[4]);
            return cmd_check(std::strtoull(argv[2], nullptr, 10), u32(std::strtoul(argv[3], nullptr, 10)),
                             pm, argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 1);
        }
        std::fprintf(stderr, "usage: debug_sparse map START [DLT] | check START DLT [PERMILLE|real] [SEED]\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
