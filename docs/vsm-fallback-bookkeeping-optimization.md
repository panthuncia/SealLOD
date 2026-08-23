# VSM fallback bookkeeping optimization

## What the passes do

`CLodShadow::VirtualShadowFinalizeFallbackPagesPass` is the render-graph name
of `VirtualShadowMapExpandPredictedPagesPass`. It contains three dispatches:

1. Stamp the current content generation on physical pages rendered this frame,
   and invalidate pages whose clear epoch does not match.
2. Project every fallback-LOD candidate sphere into its shadow clipmap, visit
   the covered virtual-page rectangle, and append a raw dependency for every
   allocated, valid page rendered this frame.
3. Publish candidate/raw counts and overflow telemetry, then reset the
   candidate counter.

`CLodShadow::VirtualShadowDeduplicateFallbackPagesPass` clears a 131,072-entry
64-bit hash table, scans the raw dependency list, and retains one record for
each `(sourceGroupGlobalIndex, physicalPageIndex)` pair. The output is read
back by `CLodStreamingSystem`. When the missing fallback group (or a parent)
becomes resident, the CPU uses the saved physical-page allocation/content
generation token to request an exact VSM-page refresh. Hash/output overflow
re-dirties the page so recovery is retried instead of silently lost.

The current capacities are 65,536 candidates, 1,048,576 raw records, 131,072
hash entries, and 65,536 output records.

## Persistent-profiler methodology

All A/B measurements below ran in one renderer process (PID 18324). Shader
candidates were published as live PSO generations; the process was never
restarted. Measurements use GPU render-graph timestamps, 95% confidence
intervals, and 80--160 accepted frames.

Two regimes were measured:

- Settled/normal caching: about 35 candidates and no raw dependencies.
- Repeatable loaded stress (`cache_disabled=true`): about 1,600--1,900
  candidates, 7,000--8,000 raw dependencies, and 3,100--3,600 unique
  dependencies per sampled telemetry frame.

All retained runs reported zero candidate, raw, hash/output, and retry
overflow in the measured telemetry.

## Retained changes

### Cooperative candidate expansion

The old expansion assigned one thread to each candidate. A single lane then
serially walked the complete projected page rectangle, including empty page
table entries. The retained shader assigns one 64-thread group to a candidate
at a time. Lane zero projects the sphere and publishes the rectangle through
group-shared state; all lanes cooperatively scan the rectangle. Wrapped
coordinates are advanced with a conditional subtract rather than per-page
modulo.

| Regime | Baseline Finalize | Retained Finalize | Change |
|---|---:|---:|---:|
| Loaded | 1.068756 ms | 0.063639 ms | -94.0% |
| Settled | 0.036803 ms | 0.014869 ms | -59.6% |

Raw and unique dependency counts remained in the same ranges before and after
the work-remapping change.

### Native 32-bit hash-slot mixing

The hash table still stores and atomically compares the complete 64-bit
dependency key. Only slot selection changed: two emulated 64-bit multiplies
and shifts were replaced by a 32-bit avalanche mix over the two key fields.
This reduced the cost sensitivity caused by the more concurrent raw-record
ordering from cooperative expansion.

With cooperative expansion, the original hash produced a noisy 0.10398 ms
Deduplicate mean. The retained 32-bit hash measured 0.04411--0.05497 ms in
loaded confirmations. Periodic outliers prevented the loaded Deduplicate
interval from converging, so this is treated as mitigation rather than a
precise standalone speedup. In the settled confirmation it measured
0.022247 ms.

The loaded combined cost changed from 1.09132 ms to 0.11861 ms in the final
confirmation, an 89.1% reduction. The settled combined cost changed from
0.08807 ms to 0.03712 ms, a 57.9% reduction.

## Rejected shader experiments

- Analytic orthographic sphere bounds replaced eight corner transforms with
  one center transform and absolute matrix-column extents. It measured
  1.07096 ms versus 1.06876 ms after rollback, so it was reverted.
- Wave-level duplicate candidate suppression reduced raw records by roughly
  25--30% but measured 1.07575 ms Finalize, so the wave-match overhead was not
  recovered.
- Wave-level duplicate dependency suppression did not improve the noisy
  Deduplicate distribution and was reverted.

## Higher-leverage follow-ups

1. Fuse expansion and deduplication. Insert the final
   `(source group, physical page)` key while a cooperative group visits a page
   and write the CPU record directly. This removes the 32 MiB raw buffer, the
   raw counter, a full-capacity scan, and one inter-pass UAV barrier.
2. Replace the per-frame 1 MiB hash clear with epoch-tagged entries, or keep a
   compact list of touched slots and clear only those slots.
3. Generate indirect dispatch arguments from the candidate/raw counters.
   Cooperative expansion already supports at most one group per live
   candidate for the first 1,024 candidates; an indirect dispatch would remove
   the remaining idle groups in settled frames. Deduplication should likewise
   dispatch only `ceil(rawCount / 64)` groups.
4. If candidate overlap remains high in other scenes, perform global candidate
   deduplication on `(group, instance, clipmap)` before page enumeration.
   Wave-only suppression was insufficient on this workload.
