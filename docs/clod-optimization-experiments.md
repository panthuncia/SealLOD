# CLOD optimization experiment log

This file records performance experiments that were rejected, constrained, or
inconclusive. Its purpose is to prevent the same ideas from being rediscovered
without new evidence.

Unless noted otherwise, measurements were taken on the development NVIDIA GPU
in the Zorah test scene using the Clang RelWithDebInfo build. Timing experiments
used 60–120 samples, a 95% confidence interval, and a target relative
confidence half-width of 0.5%. The scene contained no skinned geometry.

Results are content-, driver-, and GPU-dependent. Retest an entry only when one
of those conditions changes materially or the implementation avoids the stated
failure mode.

## Work-graph traversal and cluster culling

### Fuse leaf processing into the recursive traversal node

- Change: disabled the separate `LeafNodes` node and handled leaf records in
  `TraverseNodes`.
- Result: pass 1 changed from 0.80433 ms to 0.79919 ms, while pass 2 increased
  from 0.22915 ms to 0.22984 ms.
- Decision: rejected. Pass 1 did not converge and pass 2 regressed by about
  0.30%. The smaller specialized nodes outweighed duplicated leaf setup loads.

### Batch leaf-node output reservations with wave prefix sums

- Change: replaced per-thread cluster-bucket output reservations with one
  `GetGroupNodeOutputRecords` reservation per destination, using wave prefix
  sums to assign record offsets.
- Result: pass 1 increased from 0.80433 ms to 0.80827 ms; pass 2 was effectively
  unchanged.
- Decision: rejected. Wave-prefix work cost more than the reduced scheduler
  reservation overhead on this driver.

### Reduce the leaf coalescing width from 32 to 16

- Result: pass 1 increased to 0.94151 ms, approximately 17%.
- Decision: rejected. Extra coalescing launches and scheduler pressure dominate
  any improvement in partially filled groups.

### Convert traversal or leaf processing to thread-launch nodes

- Constraint: a thread-launch node permits at most eight output records and
  128 bytes of output-record storage. Traversal and leaf processing can emit
  eight records whose combined payload exceeds 128 bytes.
- Decision: not implemented. Coalescing is required unless the graph is
  redesigned around a smaller intermediate record.

### Expand replay records to cache traversal data

- Change: enlarged replay/frontier records to carry bounds and other data that
  would otherwise be reloaded.
- Result: pass 1 increased to approximately 0.920 ms and pass 2 to 0.240 ms.
- Decision: rejected. Larger records increased graph backing/scheduling costs
  much more than they reduced memory traffic.

### Hoist skinning instance information in traversal

- Result: pass 1 was neutral and pass 2 regressed.
- Decision: rejected for the tested rigid scene.

### Partial descriptor prefix followed by a skinned-only tail load

- Change: loaded a descriptor prefix first, then loaded the remaining fields
  only for skinned geometry.
- Result: neutral to slightly worse than the retained compact rigid header
  load.
- Decision: rejected. Keep the dedicated compact rigid meshlet-header load.

## Work-graph software raster dispatch

### Increase a software-raster broadcast batch from 8 to 16 clusters

- Result: pass 1 increased from 5.08221 ms to 5.42857 ms, approximately 6.82%;
  pass 2 increased by about 0.72%.
- Decision: rejected. Larger records and delayed/fewer consumer launches reduce
  producer-consumer overlap.

### Reduce a software-raster broadcast batch from 8 to 4 clusters

- Result: work-graph compilation failed.
- Constraint: `ClusterCull64` would require 512 output records, exceeding the
  work-graph limit of 256.
- Decision: eight clusters is the smallest legal batch for the current graph
  topology.

### Use 128 threads per software-raster cluster

- Result: pass 1 increased from 5.82397 ms to 6.88022 ms, approximately 18.1%.
- Decision: rejected. The loss of occupancy outweighs processing all possible
  vertices in one iteration.

### Cache all alpha-test material fields in group-shared memory

- Result: pass 1 improved by only about 0.18%, with overlapping confidence
  intervals, while pass 2 slightly regressed.
- Decision: rejected as inconclusive. Retain only the group-shared
  alpha-test-enabled flag and opaque UV/alpha bypass.

### Make the producer device-scope barrier conditional

- Change: skipped the visible-cluster producer barrier when software-raster,
  page-job, and Reyes pending counts were all zero.
- Result: neutral.
- Decision: rejected. Nearly every active cluster-cull group emitted dependent
  work, so the condition rarely removed a barrier.

## Software raster kernel

### Use 16 threads per software-raster cluster

- Context: tested after adding wave-cooperative packed-triangle loads; the
  retained 32-thread kernel measured 1.76841 ms
  (95% CI [1.76783, 1.76899]).
- Result: 2.58531 ms (95% CI [2.58455, 2.58608]), approximately 46.2% slower.
- Decision: rejected. A half-wave group loses too much parallel vertex and
  triangle work; retain one full 32-lane wave per cluster.

### Wave-cooperative packed-triangle loads

- Change: one 32-lane wave loads the consecutive packed triangle words once,
  then distributes the words with `WaveReadLaneAt` to decode 32 triangles.
- Confirmation result: 1.76841 ms (95% CI [1.76783, 1.76899]), compared with
  the original 1.81417 ms baseline, approximately 2.52% faster.
- Decision: retained for the compute software-raster path.

### Skip reciprocal-W group-shared stores and loads for opaque clusters

- Result: pass 1 increased from approximately 5.049 ms to 5.086 ms.
- Decision: rejected. The additional branch and live-register cost exceeded
  the saved shared-memory traffic.

### Select scanline versus bounding-box rasterization per triangle

- Change: replaced the wave-coherent `WaveActiveAnyTrue(rectWidth > 4)` choice
  with a per-thread `rectWidth > 4` choice.
- Result: statistically neutral.
- Decision: rejected. Retain the wave-coherent branch to avoid divergence.

### Hoist debug output state without removing metadata loads

- Change: loaded the debug output mode once per group and conditionally built
  debug payloads.
- Result: neutral to slightly negative.
- Decision: rejected in isolation. Hoisting the mode became useful only when it
  also allowed normal rigid rendering to skip CLOD metadata and group loads.

## Hardware mesh raster

### Initialize compacted-cluster mesh state once per thread group

- Change: lane 0 performed `InitializeMeshletFromCompactedCluster`, published a
  `MeshletSetup` through group-shared memory, and synchronized the group before
  the existing unconditional `SetMeshOutputCounts`.
- Result: DXC generated multiple `SetMeshOutputCounts` calls in DXIL even
  though the HLSL contained one call. Validation failed with
  `SetMeshOutputCounts cannot be called multiple times` and non-dominating mesh
  output writes.
- Workaround attempted: expressed the lane-0 region as a single-or-zero
  iteration loop instead of an `if` branch.
- Workaround result: the same invalid DXIL was generated.
- Decision: blocked by the current DXC compiler. Do not retry branch- or
  loop-based group setup before `SetMeshOutputCounts` without first checking
  that this compiler bug is fixed.

### Wave-cooperative hardware mesh triangle-index loads

- Change: after `SetMeshOutputCounts`, each mesh-shader wave loaded the packed
  3-byte triangle stream cooperatively and distributed words with
  `WaveReadLaneAt`.
- Compile result: all active opaque and alpha-tested mesh PSOs compiled and
  published successfully.
- Runtime result: rendering stopped advancing when profiling began and the
  sampling control pipe became unavailable. The process remained alive but the
  GPU submission path was stalled.
- Decision: rejected. The equivalent software-raster implementation is valid,
  but this mesh-shader form is unsafe on the tested compiler/driver.

### Fuse mesh triangle-index and primitive-ID output loops

- Change: emitted triangle indices and `VisibilityPerPrimitive::triangleIndex`
  in one loop after the unconditional `SetMeshOutputCounts`.
- Counter result: no material reduction in VTG registers, allocation, or launch
  stalls. A three-sample NVPerf duration capture was noisy
  (3.534 ms, 95% CI [3.163, 3.905]).
- Timing result: sequential measurements drifted from 3.414 ms for the fused
  version to 3.470 ms after restoring the original, while both the hardware and
  software passes moved in the same direction. This does not isolate a shader
  improvement.
- Decision: rejected as inconclusive; retain the simpler original output loops.

## Retesting guidance

Useful reasons to revisit an experiment include:

- a different GPU vendor or substantially different architecture;
- a driver or DXC update that changes node scheduling or register allocation;
- scenes dominated by skinned or alpha-tested geometry;
- a graph redesign that reduces record size or maximum output amplification;
- per-node timing or hardware counters showing that the original bottleneck has
  moved.

## Work-graph culling and integrated software raster

### Three-topology isolation

All three configurations were sampled in the same persistent process and scene:

| Configuration | Cull 1 | SW route 1 | HW route 1 | Phase 1 | Phase 2 | Core total |
|---|---:|---:|---:|---:|---:|---:|
| Pure-compute cull + compute SW | 1.092 ms | 1.935 ms | 3.539 ms | 6.566 ms | 0.681 ms | 7.246 ms |
| Work-graph cull + compute SW | 1.257 ms | 2.096 ms | 3.747 ms | 7.100 ms | 0.562 ms | 7.662 ms |
| Work-graph cull + graph SW | 4.985 ms | integrated | 3.836 ms | 8.821 ms | 0.894 ms | 9.715 ms |

The hybrid topology is only 5.7% slower than pure compute, while integrating
software raster into traversal is 34.1% slower. The dominant gap is therefore
graph-integrated software raster, not traversal alone. Retain the hybrid as the
production fallback.

### Put complete visible-cluster data in SW-raster node records

- Change: expanded each SW-raster record to carry packed visible-cluster data,
  assembly transform index, and final visible-cluster index.
- Result: work-graph state-object creation failed with `0x80070057`.
- Cause: `ClusterCull64` can emit up to 256 records. At 208 bytes per expanded
  record its declared node output exceeds the 32 KiB work-graph node-output
  limit.
- Decision: rejected. A direct payload requires fewer/coarser output records or
  a separate batching node; do not retry it with the existing amplification.

### Rolling eight-cluster SW accumulator

- Change: removed the universal 2,048-index group-shared accumulator and emitted
  compact eight-cluster batches from a group-uniform loop.
- Result: cull 1 increased from 4.985 ms to 5.577 ms and the core total from
  9.715 ms to 10.156 ms (about 4.5% slower overall).
- Decision: rejected and reverted. The extra output-record emission and
  scheduling cost outweighs the shared-memory reduction on this GPU.

### Rigid-only work-graph specialization

- Change: an opt-in shader variant compiles out animated node/meshlet bounds,
  bone-slot validation, skinned meshlet handling, and their fallback paths.
  The general variant remains the default.
- A–B–A result: the two general samples averaged 9.778 ms core total and
  5.013 ms cull 1; the two rigid samples averaged 9.574 ms and 4.735 ms.
- Result: approximately 2.1% faster end-to-end and 5.5% faster in graph culling
  for the current rigid-only scene.
- Decision: retained. Enable only when the workload is known to contain no
  skinned geometry.

### Use 64 threads for the graph-integrated SW consumer

- Change: doubled `WG_SWRaster` from 32 to 64 threads while leaving compute SW
  raster at 32 threads.
- Result: phase-1 integrated cull/SW increased from 4.617 ms to 5.547 ms and
  the full measured pipeline increased from 9.820 ms to 10.746 ms. Hardware
  raster was unchanged.
- Interpretation: additional intra-cluster waves reduce concurrent cluster
  residency; this consumer is not limited by insufficient ALU parallelism.
- Decision: rejected. Retain one 32-lane wave per cluster.

### Four-cluster SW node records

- Change: reduced SW records from eight to four cluster indices, halving each
  record payload while doubling record count.
- Result: DXC compiled the library, but `CreateStateObject` rejected the graph
  with `0x80070057`. The previous graph remained active.
- Decision: rejected as unsupported by the tested driver graph validator.

### Use 64-record traversal and leaf coalescing groups

- Change: increased `TraverseNodes` and `LeafNodes` from 32 to 64 threads and
  input records, leaving cluster-cull and raster nodes unchanged.
- Result: DXC compiled the shader library, but `CreateStateObject` rejected the
  resulting graph with `0x80070057`; the live system retained the old graph.
- Likely constraint: the traversal node declares worst-case child output for
  every input record. At eight children per node, doubling the input width also
  doubles its maximum output amplification and associated graph backing
  requirements.
- Retesting avenue: a 64-wide pattern may become viable if the BVH format caps
  each node at fewer children. That is not a local shader experiment: it also
  changes BVH construction, depth/fanout tradeoffs, traversal record counts,
  streaming behavior, and likely cluster locality. Re-evaluate the topology as
  a coordinated asset/runtime change rather than simply reducing the shader's
  declared maximum.

### Remove `GROUP_SYNC` from the producer device barrier

- Change: retained the device-scope visible-cluster barrier and relied on the
  immediately following group-memory barrier for group convergence.
- Result: 9.828 ms versus 9.836 ms; statistically neutral.
- Decision: rejected in isolation.

### Compile out the unused PageJob child edge and accumulator

- Change: when all three dedicated PageJob side-channel buffers are present,
  compile `CLodWorkGraphUseDedicatedComputePageJobBuffer` to true and remove
  the fallback PageJob node edge, epilogue, and 8 KiB group-shared accumulator
  from every cluster-cull node. Passes without the dedicated buffers retain the
  general fallback graph.
- A–B–A result: specialized 9.402 ms, general 9.847 ms, specialized 9.402 ms.
  Phase-1 integrated work improved from about 4.636 ms to 4.473 ms; phase 2
  improved from 0.826 ms to 0.532 ms.
- Decision: retained. This is a 4.5% full-pipeline improvement and materially
  reduces graph state and cluster-cull shared-memory pressure.

### Wave-cooperative visible-cluster record load

- Change: lane 0 loaded the four packed visible-cluster words and transform
  index, then broadcast them with `WaveReadLaneFirst`.
- Result: 9.394 ms versus 9.402 ms; statistically neutral.
- Interpretation: the cache/uniform-load path already collapses these
  identical per-wave reads effectively.
- Decision: rejected and reverted.

### Extend rigid specialization through graph SW raster

- Change: the rigid graph now compiles out SW-raster skinning-slot resolution,
  assembly bone-remap setup, and the per-vertex dynamic skinning branch.
  Assembly debug handling remains available, and the general and compute
  variants retain all skinned behavior.
- Result: phase-1 integrated work improved by 0.38–0.44 ms in the two brackets.
  The first paired total moved from 9.402 ms to 8.953 ms. The latest retained
  configuration measured 8.527 ms during upward GPU-frequency drift.
- Decision: retained. This clears the 4.5 ms phase-1 milestone, with phase-1
  integrated cull/SW measuring 3.96–4.03 ms.
- Telemetry validation: the rebuilt retained configuration reported zero
  skinned lanes, zero explicit bone evaluations, zero invalid payloads, and
  zero voxel/SW queue overflows. Visible-cluster output and phase-2 replay
  continued advancing normally. Switching back to the general graph produced
  the same workload/capacity-counter behavior.

### Remove the redundant SW record cluster count

- Change: removed `numClusters` and its consumer bounds branch because
  `SV_DispatchGrid.x` already equals the exact cluster count.
- Initial result: 8.484 ms versus 8.527 ms.
- A–B–A result: the return and confirmation samples moved with substantial GPU
  frequency drift; normalized against hardware raster, the apparent gain did
  not reproduce.
- Decision: rejected and reverted.

### Compress the SW accumulator into contiguous wave runs

- Change: represented each meshlet iteration as `{firstIndex, count}` rather
  than storing every visible index, reducing the worst-case accumulator from
  8 KiB to 512 bytes. The normal single uniform node-output allocation was
  retained, and lane 0 expanded the runs into eight-cluster records.
- Result: phase 2 improved, but phase 1 was flat to slightly worse after
  normalization. Full-pipeline time improved only 0.4%, below the retention
  threshold.
- Interpretation: serial run expansion consumed the shared-memory residency
  benefit. This representation may become useful with a parallel prefix/run
  expansion, but should not be retried with lane-0 serialization.
- Decision: rejected and reverted.

### Fill SW node records across the cluster-cull wave

- Change: after the group-uniform `GetGroupNodeOutputRecords`, distributed
  independent eight-cluster record fills across all 32 lanes instead of making
  lane 0 copy as many as 2,048 indices serially.
- Stable fan-profile result: 9.020 ms serial versus 8.958 ms parallel.
  Integrated phase 1 improved from 4.237 ms to 4.171 ms (1.54%), and the
  cull/hardware-raster ratio improved by 1.67%.
- Decision: retained. Full-pipeline improvement is 0.68%.

### Condition the producer device barrier on consumer output

- Change: executed the device-scope visible-cluster barrier only when the
  group-uniform SW or Reyes pending count was nonzero.
- Stable fan-profile result: 8.965 ms unconditional versus 8.969 ms
  conditional, with matching normalized ratios.
- Decision: rejected and reverted. The uniform branch offsets any skipped
  barriers for this workload.

### Replace the second synchronized barrier with a memory-only barrier

- Change: retained the producer device barrier with group synchronization, but
  changed the subsequent SW epilogue barrier to `GroupMemoryBarrier()` because
  the 32-thread cluster-cull group is one native wave.
- Result: 8.958 ms with the synchronized barrier versus 8.964 ms with the
  memory-only form.
- Decision: rejected and reverted.

### Explicit 32-lane wave-size attributes

- Change: added `WaveSize(32)` to all cluster-cull nodes and `WG_SWRaster`.
- Result: 8.958 ms without the attributes versus 8.959 ms with them; the
  normalized cull ratio was slightly worse.
- Decision: rejected and reverted. The compiler/driver already selects the
  appropriate native wave size.

### Compact rigid SW setup metadata loads

- Change: replaced the generic 64-byte page-header and 64-byte meshlet
  descriptor loads with scalar/vector loads for only the fields used by rigid
  SW raster.
- Result: the first candidate was neutral after normalization and the
  confirmation was about 2.8% worse.
- Interpretation: fewer requested bytes did not compensate for losing aligned
  `Load4` transactions.
- Decision: rejected and reverted.

### Non-globallycoherent SW consumer load

- Change: retained globally coherent producer stores and the device barrier,
  but read the visible-cluster payload through a normal RW buffer view in the
  dependent SW node.
- Result: 8.953 ms globally coherent versus 8.931 ms cached, but the
  cull/hardware ratio was unchanged and the 0.25% total movement was below the
  retention threshold.
- Decision: rejected and reverted to the conservative memory model.
