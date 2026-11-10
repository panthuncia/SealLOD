# Lens tooling

## LensBake

`LensBake` (built from `LensCore/`, it lands beside the other offline tools in the runtime root) is the lens toolkit: CPU
analysis, the exact pupil the renderer samples, and the polynomial lens's fit. It needs no Python and no GPU. A standalone
build of just LensCore, without a renderer build tree:

```powershell
cmake -S LensCore -B <build> -DCMAKE_PREFIX_PATH=<vcpkg_installed>/x64-windows-static-md   # needs nlohmann_json
cmake --build <build> --config RelWithDebInfo; ctest --test-dir <build> -C RelWithDebInfo   # LensCoreTests
```

For one lens (`$L = lenses\double_gauss_50mm.lens.json`):

```powershell
LensBake info $L                                   # paraxial data, planes, distortion, pupil boxes, ghost preview
LensBake rank $L --top 8                           # ghost paths by energy (CPU analysis; flares are not rendered)
LensBake pupil $L --rays 1000000                   # the exact pupil's discs, light outside them, what the sampler rejects
LensBake pupil $L --mono 1 --stop 0.5 --focus-m 1  #   ... at the d line, stopped down, focused at 1 m
LensBake fitpoly $L --out lenses\double_gauss_50mm.brlens --degrees 5,7 --rows 4000000 --manifest lenses\MANIFEST.json
LensBake eval lenses\double_gauss_50mm.brlens $L --count 200000   # a .brlens against exact transport, fresh rays
```

- **`pupil`** builds what `LensManager` builds (`ComputePupilDiscs`: 49 nodes to 1.25x the image circle, 256 boundary
  directions, 5 wavelengths or the d line with `--mono 1`), then traces `--rays` random rays per node interval over the
  region's bounding box widened by `--margin` (10%) and reports, per interval, the exact sampler's acceptance (rays in the
  region that clear the lens), the smallest disc's (sampling one disc and rejecting outside the others), and passing rays
  outside the region (must be 0). `--rims 0` turns the element rims' clipping off (the stop only).
- **`fitpoly`** traces `--rows` exact rays uniform over the backward transmission domain (`DefaultSamplingDomain`: sensor
  disk 1.1x the image circle, sensor plane from infinity focus to `--min-focus-mm` 300, rear square 1.05x the last
  semi-aperture, wavelengths importance-sampled over 380-780 nm), fits each degree by least squares (`FitPolynomial`:
  normal equations in a product-Legendre basis, double, equilibrated Cholesky with one refinement step, converted to
  monomials; mirror symmetry built in), evaluates it in float32 on `--eval-count` fresh rays (seed `--eval-seed`
  9000001), and writes the `.brlens`. With `--manifest` it records the lens's entry (file size, CRC-32, source hash, fit
  settings and accuracy) in `MANIFEST.json`. About 5 s per lens for 4M rows.
- **`.brlens`** (version 3) is documented in `LensCore/include/LensCore/BrLens.h`: the domain and one polynomial per
  degree. Readers reject a wrong magic, other versions (1 and 2 held the retired neural networks) with a message to refit,
  a CRC mismatch, and (given the prescription) a different source hash.
- **Canonical inputs** `(r, dx, dy >= 0, lambda_nm)` at the rear plane, rounded to float32 before tracing, so each row is
  the exact transport of its stored input; outputs `(px, py, dx, dy, dz, throughput)` at the front plane in the same frame
  (`LensDataset.h`). The sensor position never enters, so refocusing needs no refit.

## Test scenes (`make_lens_test_scene.py`) and image comparison (`compare_lens_images.py`)

Both need only numpy.

```powershell
python scripts/lens/make_lens_test_scene.py --variant chart --focus-distance 3 --out models/lens_tests/chart.glb --validate
python scripts/lens/make_lens_test_scene.py --variant all --write-camera logs/demo_camera_state.json
BasicRenderer.exe --model models/lens_tests/chart.glb
python scripts/lens/compare_lens_images.py polynomial.dds explicit.dds --threshold-mape 0.05 --diff diff.png --json report.json
python scripts/lens/compare_lens_images.py --self-test
```

- Variants: `chart` (2 m slanted-edge squares, checkerboards of 8/16/32/64 cells, Siemens star, plus slanted 2x2
  checkers near the four frame corners for lateral CA), `bokeh` (1.5 cm emissive spheres on a 7x5 grid at 1.5, 3
  and 12 m, out to the frame edges), `flare` (a 5000-radiance disc at 50 m near the top-right edge over a dim
  ground), `furnace` (an inward-facing, double-sided emissive sphere of radius 10 m around the camera), `all`
  (chart + bokeh). Output defaults to `models/lens_tests/<variant>.glb`; `models/` is git-ignored.
- Placement is relative to `--camera-origin` (default `-3,5,1`, the demo's start pose) looking down -Z, metres,
  Y up. Frame-edge content uses `--fov-deg 80 --aspect 1.777`; change them to match the lens's field of view.
  `--write-camera` writes a matching `demo_camera_state.json` (identity rotation looks down -Z).
- Charts are self-lit (emissive white, black base colour) in front of a black backdrop (black is `--black-albedo 0.02`: pure-black non-emissive materials currently do not render in the demo), so the demo's sky and sun
  do not change them. Geometry and factor-only materials, no textures. `--no-backdrop`, `--chart-lighting diffuse`.
- Emissive above 1 goes straight into `emissiveFactor` (`--emissive-encoding raw`): the importer reads it
  unclamped and ignores `KHR_materials_emissive_strength`. That is outside the glTF spec; `khr` writes the spec
  form, which today's importer renders at the normalized factor only.
- `--validate` is a structural check (GLB chunks, accessor ranges, index bounds, min/max); no glTF validator is
  vendored.
- The comparison treats `b` as the reference. It reports MAPE `mean |a-b| / (|b| + eps)` (`--eps 0.01`) overall and
  per channel, per-channel mean relative bias, RMSE, and a tonemapped MAE (sRGB of Reinhard `x/(1+x)` after
  `--exposure`) as a crude perceptual proxy; it is not FLIP. Non-finite pixels are excluded from the metrics and
  fail the comparison. `--diff` writes per-pixel relative error as a Turbo PNG, full scale at `--diff-scale 0.2`,
  magenta where non-finite. Reads DX10 and legacy-FourCC float DDS (DirectXTex writes RGBA32F with FourCC 116
  under `DDS_FLAGS_NONE`). Exit 0 pass, 1 fail, 2 unreadable or mismatched sizes.
