# CLod software raster: optimization experiments

`CLodOpaque::SoftwareRasterizeClustersPass1` (`shaders/ClusterLOD/softwareRaster.hlsl`, `SWRasterIndirectCSMain`) on the Zorah
scene: raster with stochastic direct lighting, capped quality (`SARP_TEXTURE_FEEDBACK_FORCE_MIP=4`, `clodLodErrorPixels` 3),
frozen lights, moving camera (`ZORAH_CAMERA_MOTION=profile`), 2560×1600, RTX 5070 Laptop (GB206). Times are the in-situ render
graph timer; NVPerf for counters only. The virtual shadow raster's own tuning is in commit 80775aa8.

## Where the work goes (2026-11-21)

SW raster telemetry (`SARP_CLOD_VISIBILITY_TELEMETRY=1` and `CLOD_SW_RASTER_TELEMETRY=1` by `pso.recompile`), one frame:

| | Count |
|---|---:|
| Clusters | 602K |
| Input triangles | 37.0M |
| Back-face culled | 14.6M (39%) |
| No pixel centre in bounds | 19.6M (53%) |
| No sample covered | 1.9M (5%) |
| Covered pixels / visibility writes | 1.0M |

97.6% of triangles reach no pixel; the cost is per cluster and per triangle setup. NVPerf: 96-99 registers, 38% occupancy,
issue active 36-40%, long scoreboard the largest stall (7.5 per issue); launch stalls 35% registers, 12% shared memory and barrier.

## Clusters per group (kept)

`SW_RASTER_CLUSTERS_PER_GROUP` (each lane loads one cluster's setup, then the group rasters them in turn) was 4 for every output
kind, tuned on the virtual shadow raster. Swept live on every SW raster PSO and `CLod_RasterBucketsCompactAndArgsPSO` together,
A/B/A/B in one session:

| Clusters per group | 1 | 2 | 4 | 6 | 8 | 16 | 32 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Primary SW raster (ms) | 3.17-3.23 | 2.85-2.90 | 2.53-2.66 | 2.51 | 2.34-2.43 | 2.49-2.57 | 3.56-3.64 |

`CLodSoftwareRasterClustersPerGroup(outputKind)` (`CLodOptions.h`) is now 8 for the primary view and 4 for the virtual shadow
raster. `PSOManager::CreateClusterLODSoftwareRasterPSO` defines it, and `RasterBucketCompactAndArgsPass` passes it to the
arguments shader in bits 8-15 of `CLOD_COMPACTION_READ_MODE_FLAGS`, so one arguments PSO serves both. The dynamic wind skin cache
passes are virtual-shadow only and keep the header's default of 4. Validation: SW raster telemetry after the change, 609K
clusters against 602K before (LOD and streaming variation; a dispatch mismatch would have halved it). Whole-frame overview across
sessions: 2.69 → 2.39 ms.

The virtual shadow raster was left at 4: its time swung 5.3-10.6 ms between profiles (page budget churn) in both builds, too much
to compare. Re-sweep it with a steadier VSM workload before changing it.

## Tried, no gain

- **Loading the cluster state after the barrier.** `SWRasterCluster` copies the cluster, work and flag structs from groupshared
  memory before the vertex phase's barrier, so the triangle phase's fields were thought to be held across it. Reloading them in
  `SWRasterTriangles` left registers at 96.6 and the time unchanged.
- **Register attribution by stubs.** The vertex phase alone (raster stubbed) and the raster phase alone (no transform) both
  compile to ~97 registers: 96 is the compiler's budget step for the whole shader, not one live range.
