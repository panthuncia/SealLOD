# VSM large-rerender performance experiments

## Protocol

- Primary stress case: VSM cache disabled, fixed streaming residency, normal and upgrade page budgets set to 500.
- Scaling points: 20, 50, 100, 250, and 500 admitted pages. The cache-enabled bounded orbit is the realistic control.
- Use one persistent Clang RelWithDebInfo process. Switch PSO generations in-place and measure A-B-A.
- Accept 80--160 frames at 95% confidence and at most 0.5% relative half-width.
- Record individual passes and the summed phase-1 and complete-VSM costs. Use `EvaluateMaterialGroupsPass` as the clock-drift reference.
- Detailed workload telemetry is opt-in and must be disabled for final timings.
- Retain only reproducible changes that improve the complete VSM rerender pipeline by at least 2% and preserve output and all raster modes.

Sampling is configured by `config/vsm_large_rerender_sampling.json`; counter captures use
`config/vsm_large_rerender_nvperf.json`. Runtime shader generations are controlled with
`scripts/Set-VsmPso.ps1`.

## Initial code attribution

Before measurement, inspection identified a high-probability duplication in the software preparation path:

- Histogram and emit independently decode every source-cluster vertex (including skinning), transform it, and derive screen bounds.
- Both passes independently enumerate candidate 4x4 virtual-page blocks.
- Both probe as many as 16 page-table entries per candidate block.
- Emit additionally keeps a large per-group block-metadata array in shared memory even though the expanded record consumes only the virtual block origin and active rectangle.

This is a hypothesis, not yet a retained optimization. Isolation generations and workload scaling must establish how much of the complete-VSM cost it explains.

## Baseline

Persistent bounded-orbit capture, 160 accepted frames (candidate A already compiled):

| Pass | Mean |
|---|---:|
| Hierarchical culling phase 1 | 15.444 ms |
| Hardware raster phase 1 | 22.957 ms |
| SW block histogram phase 1 | 8.713 ms |
| SW block emit phase 1 | 8.124 ms |
| Software raster phase 1 | 1.777 ms |
| Material-evaluation clock reference | 1.404 ms |

The long passes did not reach the 0.5% target under the moving orbit because their workload varies with the view. Final A-B-A decisions therefore require a deterministic held camera or paired orbit samples; this capture is workload attribution only.

The measured expensive subset totals 57.0 ms. Software raster is only 3.1% of that subset, confirming that preparation and the parallel hardware route, not triangle raster in the software kernel, dominate.

## Experiments

### Candidate A: remove unused emit shared metadata

Status: awaiting A-B-A timing.

The emit pass retained wrapped origins, active masks, and eight packed physical-page words for every tracked block. Its output path consumed only the virtual origin and active rectangle. The candidate removes the ten unused words per tracked block from group-shared storage while leaving block probing and emitted payloads unchanged.

The temporary block-isolation shader generations were removed after attribution.
The production shader now contains only the retained active-block metadata and
prepared-coverage implementation.

### Workload telemetry

At 500 admitted 128x128 pages, telemetry observed 94--422 million hardware pixel invocations and 66--271 million successful atomic depth writes. The admitted pages contain only 8.19 million unique texels. Hardware writes therefore show roughly 8x--33x overdraw, before counting 20--151 million page-rejected invocations. Culling performed about 0.60--0.70 million dirty-page queries to produce 31--49 thousand visible cluster records.

The former work-graph overflow fallback emitted an unscoped hardware record
when a meshlet exceeded `clodPageJobMaxPagesPerCluster`. It was removed during
cleanup. Hardware work now emits a bounded subset of block-local records;
remaining dirty blocks become eligible on later frames.

The counter remained zero in the measured Standard-mode orbit, so this fallback is not responsible for that workload. The dominant hardware cost is genuine depth overdraw from block-local records into a UAV atlas, where fixed-function early depth cannot eliminate hidden fragments.

### Page-job routing trial

The first runtime switch to Page-Job mode exposed a dormant graph-declaration bug: both page-job expansion and raster shaders load `Builtin::CLod::AssemblyTransforms`, but neither pass declared the resource. Parallel recording failed with `Resource Builtin::CLod::AssemblyTransforms not found`. Both passes now declare that dependency. Performance and output validation of forced page-job routing remains pending after rebuild.

After also declaring the CLod metadata/assembly remap inputs used by shared helpers, forced page-job routing ran successfully. A 160-frame attribution capture measured:

| Pass | Standard | Forced page-job |
|---|---:|---:|
| Hierarchical culling | 15.43 ms | 0.84 ms |
| Hardware raster | 23.38 ms | 0.07 ms |
| SW block histogram + emit | 15.45 ms | 7.28 ms |
| SW raster | 1.52 ms | 0.70 ms |
| Page-job expand + raster | 0 ms | 5.18 ms |

The measured subset fell from about 55.8 ms to 14.1 ms (about 75%). Hardware pixel activity fell to roughly 13 thousand invocations in a sampled frame. This is not yet retained as the shipping routing policy: telemetry reported only 170 of 500 admitted pages rerendered in one sampled frame. Page-job build/raster lifecycle counters are now included in the compact VSM log to distinguish empty pages from capacity/truncation before deciding whether a hybrid policy is correct.

The apparent completion loss was a completion-signal defect: the system equated a successful page render with at least one fragment depth write. Exact page-job rendering exposed genuinely empty admitted pages, while Standard mode's broad overdraw happened to write most pages and masked the issue. In Page-Job mode, admitted pages that reach finalization without a depth write are now completed as valid empty pages. Repeated telemetry reports `admitted=500`, `rendered=500`, and `admittedNotRendered=0`.

The temporary hardware-raster isolation generations were removed after the
wave page-stamp result was retained. Hardware VSM output now always uses the
wave-coalesced implementation.

The page-validation/no-output generation reduced hardware raster from roughly 23 ms to 5.05 ms with culling unchanged. About 18 ms is therefore atomic output serialization. The complete pixel path previously issued a page-table `InterlockedOr` after every successful depth atomic, causing all fragments in a page to contend on one word. The next candidate stamps each unique page once per wave using `WaveMatch`, while retaining per-pixel depth atomics and exact dependency timing.

### Retained: wave-coalesced hardware page stamping

A-B-A in one persistent Standard-mode process:

| Generation | Hardware raster |
|---|---:|
| Per-pixel stamp A | 21.15 ms |
| Wave page stamp B | 6.62 ms |
| Per-pixel stamp A | 21.15 ms |
| Wave page stamp B confirmation | 6.79 ms |

Culling stayed at 15.27--15.41 ms. The complete measured phase subset improved by roughly 32%. The wave-coalesced implementation is retained.

### Retained default: hybrid Page-Job mode

Page-Job mode with the normal diameter policy (`force all` disabled) measured:

| Pass | Mean |
|---|---:|
| Hierarchical culling | 0.924 ms |
| Hardware raster | 1.804 ms |
| SW block histogram + emit | 6.532 ms |
| SW raster | 0.677 ms |
| Page-job expand + raster | 2.292 ms |

The subset totals about 12.23 ms versus roughly 55.8 ms in the original Standard stress capture, a reduction of about 78%. It retains hardware routing for unsupported/alpha work, software raster for small clusters, and page-local jobs for large clusters. Telemetry repeatedly reported 500 rendered pages for 500 admitted pages. Page-Job is now the default VSM raster mode; Hardware Only and Standard remain available for compatibility and diagnosis.

### Retained: shared active-block metadata and prepared cluster coverage

Finer shader isolation showed that SW block preparation had two independent
sources of duplicated work:

- histogram and emit each probed all 16 page-table entries in every candidate
  4x4 block;
- histogram and emit each decoded and transformed every meshlet vertex solely
  to recover the same screen-space block rectangle.

A post-admission pass now scans each virtual block once and writes a compact
active-page rectangle. It costs approximately 0.013 ms at the configured
maximum table size. Histogram and emit replace their repeated 16-entry texture
scans with one structured-buffer lookup.

Histogram now also writes one packed 32-bit block-coverage record per source
cluster. Emit consumes that record and no longer decodes geometry or recomputes
bounds. Matched moving-orbit samples (histogram 3.345 ms versus 3.332 ms,
providing a workload reference) reduced emit from 3.108 ms to 0.143 ms.
A repeat at a heavier orbit position kept emit at 0.319 ms while histogram was
8.055 ms, confirming that emit no longer scales with vertex decode.

For rigid meshlets, histogram now projects the existing conservative meshlet
bounding sphere through the directional-shadow matrix instead of decoding all
vertices. Skinned meshlets retain exact deformed-vertex bounds. A subsequent
500-page capture measured:

| Pass | Previous matched workload | Prepared coverage + rigid sphere |
|---|---:|---:|
| Active-block table build | 0.013 ms | 0.013 ms |
| SW block histogram | 3.332 ms | 0.237 ms |
| SW block emit | 0.143 ms | 0.180 ms |
| SW preparation total | 3.488 ms | 0.430 ms |

The original retained Page-Job capture spent 6.532 ms in histogram plus emit.
The new bookkeeping path is about 0.43 ms in the comparable 500-page orbit
sample, a roughly 93% reduction for SW block preparation. Coverage remains
conservative at block granularity; raster-time page ownership validation is
unchanged.

### Final cleanup validation

Experiment-only shader branches, the old per-cluster page-table probe path,
duplicate emit-time geometry bounds, per-pixel page stamping, the alternative
dirty-sphere query, primary-camera VSM LOD, and the unscoped hardware overflow
fallback were removed.

A post-cleanup pure-compute 500-page orbit measured:

| Pass | Mean |
|---|---:|
| Active-block table build | 0.013 ms |
| SW block histogram | 0.168 ms |
| SW block emit | 0.128 ms |
| SW raster | 0.447 ms |

Hardware, Standard, Page-Job, and Reyes remain broad runtime techniques.
Budget, ownership, exact-upgrade recovery, routing, and aggregate workload
telemetry remain available as explicit opt-in diagnostics.

### Correctness follow-up: complete block emission and empty-page identity

The first cleaned implementation incorrectly assumed that blocks beyond the
32-record per-cluster tracking limit would remain dirty. Page admission and
finalization operate at page granularity, so an admitted page omitted by that
truncation could instead be finalized as valid-empty. This appeared as white
cleared-depth splotches in the Page State view.

The limit has been removed for both hardware and compute SW block records.
The optimized active-block table and prepared cluster coverage remain; emit
now loops over all committed records using the same 64-thread group. A static
cache-disabled capture measured 0.554 ms for histogram and 0.376 ms for emit,
still well below the original approximately 6.5 ms combined preparation cost.

Empty pages exposed a second independent issue. Clear/finalization could mark
a genuinely empty page content-valid without initializing its cached absolute
page tag, leaving the previous physical owner's tag and producing orange Page
State diagnostics. `VirtualShadowClearPagesPass` now initializes the page's
depth origin and absolute tag from the current owner before clearing its atlas
contents. Static cache-disabled telemetry reported 500 admitted, cleared, and
rendered pages with zero ownership/content/tag mismatches and no budget
invariant failures.

### Correctness follow-up: page-job transform and virtual-screen inputs

The optimized page-job path initially produced mostly cleared depth despite
valid page ownership. Lifecycle telemetry localized the loss after record
consumption: about 26.8K jobs and 2.93M triangles produced zero covered pixels
and zero page writes.

Two page-job inputs were wrong:

- the compute expand and raster passes read the compacted visible-cluster
  transform-index descriptor without declaring or binding its buffer;
- page-job screen mapping used `ClodViewRasterInfo` scissor dimensions rather
  than the VSM clipmap's virtual resolution.

The missing transform descriptor supplied an arbitrary assembly-transform
index to rigid geometry and collapsed projected triangles. Both compute
page-job passes now bind the compacted transform-index buffer, and compute and
work-graph page-job mapping use `clipmapInfo.virtualResolution` with a zero
virtual origin.

After the fix, the same static cache-disabled run reported approximately
48.4K consumed page jobs, 5.29M processed triangles, 47.2M covered pixels, and
42.5K page writes. Expanded-record and page-job drops remained zero, and all
page-budget invariants remained valid. The detailed raster counters are gated
by opt-in VSM telemetry so they add no per-pixel bookkeeping in normal runs.

### Re-evaluation: corrected Page-Job raster is not a performance win

The earlier Page-Job timing was invalid because the broken transform/screen
inputs caused its raster shader to do almost no pixel work. After the
correctness fix above, Standard and Page-Job were compared again with cache
disabled and both page budgets set to 500. Detailed raster counters were
disabled for timing.

The first static A-B-A comparison used the 500-page cap but only had the
default view's roughly 209-page demand. It already showed Page-Job increasing
the complete measured VSM pipeline from approximately 15.38 ms to 29.42 ms.
This was retained only as a lower-demand control, not treated as the requested
500-page result.

The saturated comparison used the bounded VSM orbit in one persistent process.
Each leg collected 160 accepted frames:

| Pass / total | Standard A1 | Page-Job B | Standard A2 |
|---|---:|---:|---:|
| Hierarchical culling | 7.894 ms | 0.893 ms | 7.638 ms |
| Hardware raster | 5.538 ms | 1.738 ms | 5.204 ms |
| SW histogram + emit | 0.519 ms | 0.410 ms | 0.404 ms |
| Conventional SW raster | 0.850 ms | 0.708 ms | 0.680 ms |
| Page-job expand + raster | 0 ms | 31.319 ms | 0 ms |
| Complete sampled VSM pipeline | 15.456 ms | 35.723 ms | 14.585 ms |
| Clear-pages workload reference | 0.0676 ms | 0.0670 ms | 0.0690 ms |

The orbit varies the view, so the long passes reached the 160-frame maximum
rather than the 0.5% confidence target. Nevertheless, both Standard legs
bracket Page-Job closely, and the clear-pages time remains stable, indicating
matched admitted-page work. Relative to the mean of the two Standard legs,
corrected Page-Job is approximately 2.38x slower for the complete sampled VSM
pipeline. Normalizing by clear-pages time produces the same conclusion
(approximately 2.42x).

Page-Job still removes about 6.9 ms of traversal and hardware raster work, but
its now-functional page-local raster costs about 29.2 ms by itself. Therefore
the previous performance justification for making Page-Job the preferred
technique is rejected. The switch remains useful for diagnosis and future
optimization, but its corrected performance figures must replace the earlier
mostly-no-output measurements.
