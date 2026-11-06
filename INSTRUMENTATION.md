# Instrumentation survey — what to benchmark with (and what NOT to add)

Goal: stay **fast and just** — the verifier's throughput is the product;
benchmarks only exist to keep it honest. Anything that tunes FOR the
benchmark rather than the real 4e18→5e18 campaign is banned by default.

## What exists here (use these first)

| Tool | Role |
|---|---|
| `sweep.py` | parameter sweeps + **`--ab` interleaved A/B with paired deltas** — the only valid way to compare two binaries on this bimodal hardware |
| `gpu_watch.py` | runs a command while sampling `gputop` (GT frequency, render busy) — diagnoses fast/slow whole-run modes without touching the GPU |
| `freq_probe.py` | frequency-state probe around short runs |
| `benchmark.cpp` / `tuner.cpp` | multi-run harness / staged auto-tuner (fresh process per sample) |
| `debug_*.cpp` | bit-exact validators — a benchmark without `result=PASS` is worth nothing |

Known limitation: on the Xe kernel driver (B580), `intel_gpu_top` does not
work at all ("use gputop instead"), and `gputop`'s per-PID rcs% does not
track Level Zero compute clients — only its **GT frequency header** is
reliable. `gpu_watch.py` therefore reports frequency as primary signal.

## Available but not installed / deeper options (in order of value)

1. **Level Zero metrics API (`zeMetric*`)** — hardware counters (EU
   occupancy, memory bandwidth, L3 hit rate, pipe stalls) via the driver's
   metric groups. This is the real next step for kernel-level questions:
   it would settle WHY the unrolled atomic loop loses in production
   (bandwidth vs issue-rate) instead of guessing. Cost: a small C tool next
   to `benchmark.cpp`; sampling adds launch overhead, so gate it behind a
   flag and never mix sampled and unsampled numbers in one comparison.
2. **`gputop` scripting / PMT sysfs** (`/sys/class/intel_pmt/*`) —
   coarse power/telemetry; useful to rule out power caps for the slow mode.
   Read-only, zero distortion.
3. **VTune (oneAPI `vtune` CLI, not currently installed)** — GUI-grade
   GPU hotspot analysis. Heavyweight; only worth it for a dedicated
   kernel-optimization session, not routine regression checks.
4. **Tracing (`ze Loader traces`, `L0_DEBUG`)** — launch/sync overhead
   breakdowns. Use only when the pipeline (not the kernels) is the suspect.

## Deliberately NOT added (anti-overfitting rules)

- No auto-tuning loops that watch the benchmark journal and mutate configs
  without a human KEEP decision (the tuner's soak/backoff gates exist for
  exactly this).
- No "benchmark-special" configs: production-shaped runs only (the
  gpu-benchmark skill's rule; odd debug splits have wedged the GPU).
- No caching of benchmark results across code changes — every comparison
  is fresh-process, interleaved, PASS-gated.
- Do not chase variance below ~2%: same-day drift on this box is ~15%
  (bimodality); paired A/B deltas are the currency, not absolute numbers.
