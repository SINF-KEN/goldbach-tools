<!-- Filled 2026-09-05 for the [Question] template (intel/compute-runtime).
     Copy each field into the matching form box. Doc/issue checks done 2026-09-05. -->

## Title

```
Level Zero (Xe2/Battlemage): is zeCommandListAppendBarrier the required sync for store->atomic visibility between kernels, or do event waits suffice?
```

## Pre-submission Checklist

- ✅ **I have searched existing issues and discussions for similar questions** —
  done 2026-09-05: no issue or discussion covers memory visibility across
  event waits / the barrier-vs-wait flush asymmetry. Closest found:
  discussion #520 ("Best Practices and Guidelines" — general sync advice,
  not this question) and issue #765 (host-memory visibility with a memset
  kernel — different scope). Neither answers device-side store→atomic
  visibility between kernels.
- ✅ **I have checked the documentation and FAQ / programmers-guide** —
  done 2026-09-05: `documentation/FAQ.md` and
  `programmers-guide/PROGRAMMERS_GUIDE.md` contain nothing about memory
  coherency or cache-flush requirements between commands;
  `programmers-guide/IMMEDIATE_COMMANDLIST.md` details synchronization
  models (events, `zeEventHostSynchronize`) but says nothing about memory
  visibility between kernels. The **Level Zero spec's Core Programming
  Guide** does state: *"Commands executed on a command list are not
  guaranteed to maintain memory coherency with other commands; i.e. there
  is no implicit memory or cache coherency"* — which is why this is filed
  as a clarification question, not a bug report.

## Your Question

### Question Category *

Select the closest available option:

```
Level Zero / API behavior  (if the dropdown has a Level Zero or API-usage category)
```

If only generic categories exist, pick **Other** and specify:

```
Level Zero API semantics — memory visibility / synchronization between kernels
```

### Question *

On an Intel Arc B580 (Xe2/Battlemage, compute-runtime 26.31.39395.13): what must an application do to guarantee that kernel A's regular global-memory stores are visible to kernel B's atomics on the same allocation?

**Source observation (tag `26.31.39395.13`):** only `zeCommandListAppendBarrier` flushes the data-port caches (`setBarrierWaFlags`, `gfx_core_helper_xe2_hpg_core.cpp:157-162`); the `zeCommandListAppendWaitOnEvents` path emits no `DataportFlush` at all (`cmdlist_hw.inl:3576-3756`).

**Spec:** the Core Programming Guide says commands get "no implicit memory or cache coherency"; events "can invoke an execution and/or memory barrier", with the `ze_event_desc_t` scope flags controlling coherency.

**Questions:**

1. Is `zeCommandListAppendBarrier` the intended and only guaranteed way to make kernel A's global stores visible to kernel B's atomics on the same in-order/immediate command list?
2. Does `ZE_EVENT_SCOPE_FLAG_DEVICE` on an event wait emit the data-port flush? We find no `DataportFlush` anywhere in the wait path.
3. If the barrier is required, could this be documented (e.g. `programmers-guide/IMMEDIATE_COMMANDLIST.md`)? The failure mode when missed is nondeterministic data corruption, not an error.

**Context:** our fill kernel (whole-word global stores) + bit-clear kernel (32-bit `atomic_and` RMWs) corrupted nondeterministically when synchronized only by event waits / in-order succession on one immediate `ZE_COMMAND_QUEUE_FLAG_IN_ORDER` command list; inserting `zeCommandListAppendBarrier` between them fixed it permanently. A minimal 6-variant reproducer runs clean on 26.31 — hence a question, not a bug report. Testcase available on request.

### Additional Notes

- Honest reproduction status: a minimal standalone reproducer of this shape
  (fill kernel + atomic kernel, alternating patterns, 6 synchronization
  variants including the SLM-staged production shape, buffers 8–32 MB,
  30–100 iterations) runs **clean** on compute-runtime 26.31.39395.13 —
  the gap could not be triggered in isolation on this driver version. The
  historical corruption predates this driver. This is filed as a
  clarification/documentation question for that reason.
- Environment: Intel Arc B580 (Battlemage G21, `8086:e20b`, PCI
  `0000:03:00.0`), kernel `xe`, kernel 7.2.3-1-cachyos, CachyOS
  (Arch-based, rolling), packages `intel-compute-runtime 26.31.39395.13-1.1`,
  `level-zero-loader 1.32.0-1`, `intel-graphics-compiler 1:2.40.13-1.1`,
  `intel-gmmlib 22.10.0-1.1`, installed from the distribution repository.
- The standalone testcase (6 sync variants + SLM-staged production shape)
  is available on request.
- Line numbers verified against the `26.31.39395.13` release tag.
