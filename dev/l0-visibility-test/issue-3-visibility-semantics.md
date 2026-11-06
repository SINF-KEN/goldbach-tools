# NEO/Level Zero: clarify memory visibility across event waits / in-order succession — wait paths emit no DataportFlush, only AppendBarrier does

## Summary

Request for clarification (and documentation) of Level Zero memory-visibility
semantics on Xe2/Battlemage: **does a kernel's regular global stores become
visible to a later kernel's atomics on the same allocation when the only
synchronization is an event wait or in-order queue succession?**

Driver source suggests not unconditionally:

- The `zeCommandListAppendBarrier` path programs
  `PIPE_CONTROL::DataportFlush + UnTypedDataPortCacheFlush`
  (`shared/source/xe2_hpg_core/gfx_core_helper_xe2_hpg_core.cpp:157-162`,
  `setBarrierWaFlags`, verified at the `26.31.39395.13` release tag).
- The `zeCommandListAppendWaitOnEvents` path emits only a conditional
  batch-buffer-start, a CS-stall-only `PIPE_CONTROL`, or an
  `MI_SEMAPHORE_WAIT` — **no data-port flush**
  (`level_zero/core/source/cmdlist/cmdlist_hw.inl:3576-3756`: conditional
  batch-buffer-start at :3592, `MI_SEMAPHORE_WAIT` at :3653, then
  `appendWaitOnEvents` with in-order dependency-counter handling at :3685;
  the only flush call in the region writes the dependency counter, not the
  data-port caches).

If visibility is not implied, that is defensible as a design point — but it
is not documented anywhere a Level Zero developer would look, and the failure
mode is nondeterministic data corruption, not an error.

## Environment

- intel-compute-runtime 26.31.39395.13, Intel Arc B580 (Battlemage / Xe2)
- Level Zero via the distro loader (`libze_loader`), headers from
  `/usr/include/level_zero`
- OS: CachyOS x86_64, kernel 7.2.3-1-cachyos
- Date: 2026-09-05

## Background: the corruption we hit

A segmented sieve pipeline (three kernels per segment on one immediate
IN_ORDER command list, double-buffered two-slot structure):

1. kernel A builds a bitmap with **whole-word global stores** (data staged
   in SLM, written out as `u64` words),
2. kernel B clears individual bits in that bitmap with **32-bit
   `atomic_and`** read-modify-writes on the containing half-words.

With only event-wait/in-order succession between A and B, the map was
corrupted nondeterministically — the atomics occasionally read pre-fill
state and wrote it back, losing whole-word stores. A dedicated debug tool
comparing the bitmap caught it. Inserting `zeCommandListAppendBarrier`
between A and B eliminated it permanently.

## Reproduction status (honest)

A minimal standalone reproducer (fill kernel + atomic kernel, alternating
patterns so stale data is distinguishable, 6 synchronization variants:
in-order succession, barrier, plain-event wait, timestamp-event wait, plus
SLM-staged u64-store production shape; buffers 8–32 MB, 30–100 iterations)
runs **clean** on the current driver — the gap could not be triggered in
isolation on compute-runtime 26.31. The historical corruption predates this
driver version. The source-level flush asymmetry described above is still
present, which is why we are asking rather than staying silent.

## Ask

1. Is event-wait / in-order succession between kernels *intended* to
   guarantee visibility of regular stores to subsequent atomics on Xe2,
   matching the Level Zero programming model's expectations for command
   dependencies?
2. If yes: is the missing data-port flush on the wait paths a defect?
3. If no: please document the requirement explicitly (AppendBarrier — or
   timestamp-flagged event completion, whose post-sync PIPE_CONTROLs do
   flush) so applications don't have to discover it through corruption.

## References

- Flush behavior on barrier vs. wait paths (line numbers verified against
  the `26.31.39395.13` release tag of intel/compute-runtime):
  `shared/source/xe2_hpg_core/gfx_core_helper_xe2_hpg_core.cpp:157-162`
  (barrier: `setBarrierWaFlags` forces `DataportFlush` +
  `UnTypedDataPortCacheFlush`),
  `level_zero/core/source/cmdlist/cmdlist_hw.inl:3576-3756` (wait path: no
  data-port flush).
- Post-sync PIPE_CONTROLs for KERNEL_TIMESTAMP-flagged events do flush the
  data-port caches — i.e., the visibility outcome silently depends on
  event-pool flags, which is especially invisible to users.
- Our standalone testcase (6 sync variants + SLM-staged production shape)
  is available on request; it currently reproduces nothing on 26.31, which
  is disclosed above.
