# Material evaluation optimization experiments

## Object POM distance and debug-height gating (2026-09-04)

- Baseline `EvaluateMaterialGroupsPass`: 5.172462 ms over 70 samples.
- Added an object-POM distance test matching the existing terrain fade range.
  Pixels beyond `heightFadeEndDistance` now skip camera-vector normalization,
  TBN construction, and the POM texture walk. Pixels in the fade interval use
  a faded height scale and proportionally reduced step count.
- POM candidate: 4.851859 ms over 38 samples, 6.20% below baseline.
- Restricted `SampleMaterialGeometricHeightDebug` and the terrain RVT
  geometric-height lookup to `OUTPUT_TERRAIN_GEOMETRIC_HEIGHT`; these values
  have no normal shading consumer.
- Combined candidate: 4.767046 ms over 88 samples, 7.84% below baseline.
- All 50 live material-evaluation PSO specializations were compiled and
  published in the same MO2-launched process. Generation 3 is the retained
  combined candidate for every specialization.
- The first single NVPerf candidate sample was noisy/order-sensitive. An
  immediate baseline/candidate repeat measured 4.804960 ms versus 4.421888 ms,
  a 7.97% reduction consistent with the statistical result. The multi-sample
  render-graph timing remains the reliable estimator.
