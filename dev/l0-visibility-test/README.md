# L0 store→atomic visibility testcase — 2026-09-05

Independently tests the goldbach-l0 claim: "an event wait (or an in-order
queue succession) does NOT guarantee kernel A's stores are visible to
kernel B's atomics on the same words; only zeCommandListAppendBarrier
guarantees it."

Per iteration a fill kernel writes an alternating pattern (stale data of
the previous iteration is distinguishable), then an atomic_and RMW runs on
the same words; any word != pattern&mask is classified (stale-zero /
stale-prev / other).

Modes:
  inorder      immediate IN_ORDER list, no barrier between the kernels
  barrier      same + zeCommandListAppendBarrier (production rule)
  event        two regular command lists, second waits on a plain event
  event_ts     same with a KERNEL_TIMESTAMP-flagged event
  slm_inorder  production shape: fill stages in SLM, whole-u64-word global
               stores; atomics are 32-bit half-word atomic_and
  slm_barrier  same + barrier

Build:
  ocloc compile -file vis.cl -device bmg -output vis
  g++ -O2 -std=c++17 -I /usr/include/level_zero vis_test.cpp -o vis_test -lze_loader
Run (from this dir):
  ./vis_test <mode> [iters] [nwords]

## Result (B580, compute-runtime 26.31.39395, 2026-09-05)

ALL modes clean: 0 corrupt iterations out of 30 per mode (2M words), plus
100 extra slm_inorder iterations at 4M u64 words (32 MB). The historical
corruption does NOT reproduce in minimal form on this driver.

Verdict: not an upstream-reportable bug today (no reproducer). The
production AppendBarrier rule stays as defense-in-depth; the driver-source
analysis (WaitOnEvents emits no DataportFlush) still stands — the current
driver simply did not expose the gap in this shape. If map corruption ever
reappears, bisect with this tool first.

## Footgun found while building this

zeEventPoolCreate with a zero-initialized ze_event_pool_desc_t (count
field left 0) makes the driver ABORT inside gmm_helper/resource_info.cpp
instead of returning ZE_RESULT_ERROR_INVALID_SIZE — no clean error without
the validation layer. Invalid input on our side, but a diagnostics quality
issue. Minimal repro: /tmp/pool_min.cpp (regenerate from history if lost).
