"""Analysis for scripts/stochastic_scaling.sh: profile summaries, unbiasedness and noise of stochastic lighting captures.

Captures are the RGBA float DDS files `capture.hdr` writes for the targets `stochastic_diffuse` and `stochastic_specular`,
named <prefix>_<tag>_<d|s>_<i>.dds. A frame's value is the luminance of diffuse + specular.

    stochastic_scaling_analysis.py summary MD                     pass<TAB>mean ms lines from a profile's last_run_summary
    stochastic_scaling_analysis.py table TSV                      the profile mode's rows as a table (Sample: the split passes' sum)
    stochastic_scaling_analysis.py sample MD                      the estimator's time in a summary: one pass or the split passes' sum
    stochastic_scaling_analysis.py bias DIR PREFIX                accumulation (A) against the reference (R1, R2)
    stochastic_scaling_analysis.py noise DIR PREFIX TAG...        live frames TAG_1..n against the accumulation (A)
    stochastic_scaling_analysis.py png DIR PREFIX OUT TAG...      frames TAG_1..n as PNGs in OUT: rgb x / (1 + x), sRGB-encoded
    stochastic_scaling_analysis.py pair DIR PREFIX TAG...         DENOISE_DEBUG_PAIR frames (r denoised, g noisy luminance): bias and lag
    stochastic_scaling_analysis.py dropped LOG [SKIP_LINES]       the most (froxel, light) pairs the light grid dropped, from its log lines
    stochastic_scaling_analysis.py dlss DIR PREFIX OUT TAG...     consecutive frames before and after DLSS against the reference's (R1, R2)
    stochastic_scaling_analysis.py dlssnoise DIR PREFIX TAG...    the same frames without a reference: each variant's slow and fast noise
    stochastic_scaling_analysis.py trail DIR PREFIX OUT TAG...    consecutive frames of moving lights: stuck history, per stage
    stochastic_scaling_analysis.py still DIR PREFIX               frozen lights: denoised (DENOISE_DEBUG_VIEW 1) against the reference
    stochastic_scaling_analysis.py guide DIR PREFIX               the guide (overhaul phase 2): its stability over frames G (jitter on)
                                                                  and J (jitter off), and J against the unshadowed reference U
    stochastic_scaling_analysis.py paired DIR PREFIX [OUT]        the paired ratio's denominator (overhaul phase 3): DENOISE_DEBUG_VIEW 7
                                                                  frames P with the guide: E[x] and the history H_X against 1

Errors are relative to the mean of the comparison image: pixel |d| is the mean absolute per-pixel difference, block |d|
the same over 16x16 block means (what is left once the noise is averaged locally: bias shows up there).
"""
import glob
import os
import re
import sys

import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)

# DXGI formats and legacy D3DFMT codes for the float layouts capture.hdr can write.
DXGI_FLOAT = {2: (np.float32, 4), 6: (np.float32, 3), 10: (np.float16, 4), 41: (np.float32, 1)}
D3DFMT_FLOAT = {116: (np.float32, 4), 113: (np.float16, 4)}


def read_dds(path, channels=3):
    """Returns an HxWx3 (or HxWxchannels) float32 array from an uncompressed float DDS; HxW for a one-channel file."""
    b = open(path, 'rb').read()
    if b[:4] != b'DDS ':
        raise ValueError(f'{path}: not a DDS file')
    h = int.from_bytes(b[12:16], 'little')
    w = int.from_bytes(b[16:20], 'little')
    fourcc = b[84:88]
    if fourcc == b'DX10':
        fmt = int.from_bytes(b[128:132], 'little')
        layout, off = DXGI_FLOAT.get(fmt), 148
    else:
        fmt = int.from_bytes(fourcc, 'little')
        layout, off = D3DFMT_FLOAT.get(fmt), 128
    if layout is None:
        raise ValueError(f'{path}: unsupported format {fmt}')
    dtype, stored = layout
    count = w * h * stored
    a = np.frombuffer(b, dtype=dtype, count=count, offset=off).reshape(h, w, stored)
    if stored == 1:
        return a[..., 0].astype(np.float32)
    return a[..., :channels].astype(np.float32)


def luminance(directory, prefix, tag, i=1):
    base = os.path.join(directory, f'{prefix}_{tag}')
    return (read_dds(f'{base}_d_{i}.dds') + read_dds(f'{base}_s_{i}.dds')) @ LUMA


def frame_count(directory, prefix, tag):
    return len(glob.glob(os.path.join(directory, f'{prefix}_{tag}_d_*.dds')))


def blocks(x, k=16):
    h, w = x.shape[0] // k, x.shape[1] // k
    return x[:h * k, :w * k].reshape(h, k, w, k).mean(axis=(1, 3))


def compare(a, b):
    """a against b: mean ratio, pixel |d|, block |d| mean / p95 / max, all relative to b's mean."""
    m = float(b.mean())
    db = np.abs(blocks(a) - blocks(b))
    return {
        'ratio': float(a.mean()) / m,
        'pixel': float(np.abs(a - b).mean()) / m,
        'block': float(db.mean()) / m,
        'block_p95': float(np.percentile(db, 95)) / m,
        'block_max': float(db.max()) / m,
    }


def fmt_compare(name, c):
    return (f"{name:<38} mean ratio {c['ratio']:.4f}  pixel |d| {c['pixel']:.4f}  "
            f"16x16 block |d| mean {c['block']:.4f} p95 {c['block_p95']:.4f} max {c['block_max']:.3f}")


def cmd_bias(directory, prefix):
    r1, a, r2 = (luminance(directory, prefix, t) for t in ('R1', 'A', 'R2'))
    ref = (r1 + r2) * 0.5
    est = compare(a, ref)
    drift = compare(r2, r1)
    print(fmt_compare('accumulated estimator vs reference', est))
    print(fmt_compare('reference drift (R2 vs R1)', drift))
    ok = abs(est['ratio'] - 1.0) <= 0.005
    print(f"verdict: mean ratio {'within' if ok else 'OUTSIDE'} 0.5%; block |d| mean {est['block']:.4f} vs drift "
          f"{drift['block']:.4f} (p95 {est['block_p95']:.4f} vs {drift['block_p95']:.4f})")


def cmd_noise(directory, prefix, tags):
    acc = luminance(directory, prefix, 'A')
    base = None
    for tag in tags:
        n = frame_count(directory, prefix, tag)
        if n == 0:
            print(f'{tag}: no frames')
            continue
        cs = [compare(luminance(directory, prefix, tag, i), acc) for i in range(1, n + 1)]
        pixel = float(np.mean([c['pixel'] for c in cs]))
        block = float(np.mean([c['block'] for c in cs]))
        ratio = float(np.mean([c['ratio'] for c in cs]))
        rel = '' if base is None else f'  ({(pixel / base - 1) * 100:+.1f}% pixel error vs {tags[0]})'
        base = pixel if base is None else base
        print(f'{tag:<16} frames {n}  single-frame pixel |d| {pixel:.4f}  16x16 block |d| {block:.4f}  frame mean ratio {ratio:.4f}{rel}')


def cmd_png(directory, prefix, out, tags):
    from PIL import Image
    for tag in tags:
        for i in range(1, frame_count(directory, prefix, tag) + 1):
            base = os.path.join(directory, f'{prefix}_{tag}')
            x = np.maximum(read_dds(f'{base}_d_{i}.dds') + read_dds(f'{base}_s_{i}.dds'), 0.0)
            x = x / (1.0 + x)
            x = np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)
            path = os.path.join(out, f'{prefix}_{tag}_{i}.png')
            Image.fromarray(np.clip(x * 255.0 + 0.5, 0, 255).astype(np.uint8)).save(path)
            print(path)


def cmd_pair(directory, prefix, tags):
    """Each frame's denoised luminance (r) against its own noisy input (g), which is an unbiased estimate of that frame: the
    mean ratio is the denoiser's bias, and the 32x32 block |d| (relative to the mean) its lag and blur, plus the noisy block
    means' own noise. That noise is estimated per block from the spread of its distinct samples (with reduced resolution the
    noisy value repeats over each 2x2, so a block has a quarter as many) and given as the floor, E|N(0, se)| = se sqrt(2/pi)."""
    k = 32
    for tag in tags:
        n = frame_count(directory, prefix, tag)
        if n == 0:
            print(f'{tag}: no frames')
            continue
        ratios, errors, floors, ds = [], [], [], []
        for i in range(1, n + 1):
            x = read_dds(os.path.join(directory, f'{prefix}_{tag}_d_{i}.dds'))
            r, g = x[..., 0].astype(np.float64), x[..., 1].astype(np.float64)
            h, w = (r.shape[0] // k) * k, (r.shape[1] // k) * k
            r, g = r[:h, :w], g[:h, :w]
            # Reduced resolution repeats each sample over its 2x2 (background pixels, written 0, break some blocks).
            d = 2 if np.mean(g[0::2, 0::2] == g[1::2, 1::2]) > 0.9 and np.mean(g[0::2, 0::2] == g[0::2, 1::2]) > 0.9 else 1
            m = g.mean()
            rb, gb = blocks(r, k), blocks(g, k)
            distinct = g[0::d, 0::d]
            kd = k // d
            var = distinct.reshape(h // k, kd, w // k, kd).var(axis=(1, 3), ddof=1)
            se = np.sqrt(var / (kd * kd))
            ratios.append(r.mean() / m)
            errors.append(np.abs(rb - gb).mean() / m)
            floors.append((se * np.sqrt(2.0 / np.pi)).mean() / m)
            ds.append(d)
        print(f'{tag:<16} frames {n}  downsample {max(ds)}  denoised/noisy mean ratio {np.mean(ratios):.4f} '
              f'(min {np.min(ratios):.4f}, max {np.max(ratios):.4f})  32x32 block |d| {np.mean(errors):.4f}  '
              f'noise floor {np.mean(floors):.4f}  excess {np.sqrt(max(np.mean(errors) ** 2 - np.mean(floors) ** 2, 0.0)):.4f}')


TRAIL_K, TRAIL_HALF = 8, 4
TRAIL_DARK, TRAIL_LIT, TRAIL_STUCK = 0.02, 0.10, 0.25


def trail_metrics(rb, gb):
    """The trail numbers (cmd_trail) of an output's 8x8 block means rb against the noisy input's gb, both [frames, h, w]."""
    half, DARK, LIT, STUCK = TRAIL_HALF, TRAIL_DARK, TRAIL_LIT, TRAIL_STUCK
    n = len(rb)
    m = gb.mean()
    # Windowed truth: the input's block mean over +-half frames (edges: what is there).
    c = np.cumsum(np.concatenate([np.zeros((1,) + gb.shape[1:]), gb]), axis=0)
    lo = np.clip(np.arange(n) - half, 0, n)
    hi = np.clip(np.arange(n) + half + 1, 0, n)
    truth = (c[hi] - c[lo]) / (hi - lo)[:, None, None]
    # As displayed: x / (1 + x) of the block means (debug output 77's view). Linear sums are decided by the few pixels
    # beside a light at thousands of times the mean, which all display white.
    t_out, t_truth = rb / (1.0 + rb), truth / (1.0 + truth)
    excess = np.maximum(t_out - t_truth, 0.0).sum() / max(t_truth.sum(), 1e-12)
    deficit = np.maximum(t_truth - t_out, 0.0).sum() / max(t_truth.sum(), 1e-12)
    shown = t_out.sum() / max(t_truth.sum(), 1e-12)
    # The same with the output windowed like the input: like with like for an output that follows the light without lag
    # (a lag-free output is sharper in time than the windowed truth, which reads as a displayed deficit beside fast lights).
    co = np.cumsum(np.concatenate([np.zeros((1,) + rb.shape[1:]), rb]), axis=0)
    out_w = (co[hi] - co[lo]) / (hi - lo)[:, None, None]
    shown_windowed = (out_w / (1.0 + out_w)).sum() / max(t_truth.sum(), 1e-12)
    dark = truth < DARK * m
    stuck_energy = rb[dark].sum() / max(rb.sum(), 1e-12)
    # Frames since the block's input was last lit (n+1 if never within the capture).
    age = np.full(gb.shape, n + 1.0)
    since = np.full(gb.shape[1:], n + 1.0)
    for i in range(n):
        since = np.where(truth[i] > LIT * m, 0.0, since + 1.0)
        age[i] = since
    sel = dark & (rb > STUCK * m) & (age <= n)
    weights = rb[sel]
    mean_age = float((age[sel] * weights).sum() / weights.sum()) if weights.size else 0.0
    p95_age = float(np.percentile(age[sel], 95)) if weights.size else 0.0
    max_age = float(age[sel].max()) if weights.size else 0.0
    stuck_blocks = (dark & (rb > STUCK * m)).mean(axis=(1, 2)).mean()
    # Absolute counts (block-frames), all and long-lived (10 frames or more since lit): the share above rounds to 0.01%.
    stuck_count = int(sel.sum())
    stuck_long = int((sel & (age >= 10)).sum())
    # Persistence: per block, the longest run of consecutive frames it is stuck (dark input, lit output). The age above is the
    # time since the block was last lit, which a one-frame overshoot in a block that has been dark for long reads as old.
    stuck_any = dark & (rb > STUCK * m)
    run = np.zeros(gb.shape[1:]); longest = np.zeros(gb.shape[1:])
    for i in range(n):
        run = np.where(stuck_any[i], run + 1.0, 0.0)
        longest = np.maximum(longest, run)
    runs = longest[longest > 0]
    run_max = float(runs.max()) if runs.size else 0.0
    run_mean = float(runs.mean()) if runs.size else 0.0
    run_blocks = int(runs.size)
    # Lag: per block whose input changes a lot over the capture, the delay (0..MAX_LAG frames) that best lines its
    # denoised series up with its input series (smoothed over +-1 frame), by least squares in log2(1 + x) so the
    # brightest moments don't decide alone. Weighted by the input's range; noise doesn't shift the best delay.
    MAX_LAG = 12
    lo1, hi1 = np.clip(np.arange(n) - 1, 0, n), np.clip(np.arange(n) + 2, 0, n)
    smooth = np.log2(1.0 + (c[hi1] - c[lo1]) / (hi1 - lo1)[:, None, None] / m)
    out_l = np.log2(1.0 + rb / m)
    span = smooth.max(axis=0) - smooth.min(axis=0)
    moving = span > 1.0
    lags, lag_w = np.zeros(0), np.zeros(0)
    if moving.any() and n > 2 * MAX_LAG:
        costs = np.stack([((out_l[MAX_LAG:] - smooth[MAX_LAG - d:n - d]) ** 2).mean(axis=0) for d in range(MAX_LAG + 1)])
        best = costs.argmin(axis=0)
        lags, lag_w = best[moving].astype(np.float64), span[moving]
    lag_mean = float((lags * lag_w).sum() / lag_w.sum()) if lags.size else 0.0
    lag_p90 = float(np.percentile(lags, 90)) if lags.size else 0.0
    # Paired, per frame (what pair mode measures; the one reading that stays valid with a moving camera, where blocks don't
    # line up across frames): the output's mean over the input's, and the 32x32 block |d| relative to the input's mean.
    f, h4, w4 = rb.shape[0], rb.shape[1] // 4, rb.shape[2] // 4
    r32 = rb[:, :h4 * 4, :w4 * 4].reshape(f, h4, 4, w4, 4).mean(axis=(2, 4))
    g32 = gb[:, :h4 * 4, :w4 * 4].reshape(f, h4, 4, w4, 4).mean(axis=(2, 4))
    frame_ratio = rb.mean(axis=(1, 2)) / np.maximum(gb.mean(axis=(1, 2)), 1e-12)
    block32 = float(np.abs(r32 - g32).mean() / m)
    return {'n': n, 'm': m, 'ratio': rb.mean() / m, 'shown': shown, 'shown_windowed': shown_windowed, 'excess': excess, 'deficit': deficit,
            'stuck_energy': stuck_energy, 'stuck_blocks': stuck_blocks, 'stuck_count': stuck_count, 'stuck_long': stuck_long,
            'run_max': run_max, 'run_mean': run_mean, 'run_blocks': run_blocks,
            'mean_age': mean_age, 'p95_age': p95_age,
            'max_age': max_age, 'lag_mean': lag_mean, 'lag_p90': lag_p90, 'lag_blocks': lags.size, 'dark': dark,
            'frame_ratio': frame_ratio, 'block32': block32}


def fmt_trail(label, t):
    return (f'{label:<24} frames {t["n"]}  input mean {t["m"]:.4f}  denoised/input {t["ratio"]:.4f} (displayed {t["shown"]:.4f}, windowed {t["shown_windowed"]:.4f})  '
            f'excess {t["excess"]:.4f}  deficit {t["deficit"]:.4f}  stuck energy {t["stuck_energy"]:.4f}  '
            f'stuck blocks {100 * t["stuck_blocks"]:.2f}% ({t.get("stuck_count", 0)} block-frames, {t.get("stuck_long", 0)} aged 10+; {t.get("run_blocks", 0)} blocks, run mean {t.get("run_mean", 0):.2f} max {t.get("run_max", 0):.0f})  age (frames since lit) mean {t["mean_age"]:.1f} p95 {t["p95_age"]:.0f} '
            f'max {t["max_age"]:.0f}  lag mean {t["lag_mean"]:.2f} p90 {t["lag_p90"]:.0f} frames ({t["lag_blocks"]} blocks)  '
            f'paired: frame ratio {np.mean(t["frame_ratio"]):.4f} ({np.min(t["frame_ratio"]):.3f}-{np.max(t["frame_ratio"]):.3f}) '
            f'32x32 |d| {t["block32"]:.4f}')


def frame_index(path):
    return int(path.rsplit('_', 1)[1].split('.')[0])


def cmd_trail(directory, prefix, out, tags):
    """Moving-light trails: consecutive frames of a held view with moving lights (files <tag>_<target>_<i>). The noisy input is
    unbiased each frame, so a block whose input stays dark while the denoised output stays lit is history the denoiser kept
    after the light left. Per 8x8 block and frame, the input's truth is its mean over a window of +-W frames (the light moves
    a few blocks a frame, so the window blurs it a little; the stuck history this is after lasts far longer than the window).
    Reported, relative to the mean of the input over all frames:
      excess   the denoised output above the windowed input, as displayed (x / (1 + x) of the block means), summed over
               blocks and frames, over the input's total (departed light, plus noise); deficit the same below it (arrival
               lag, plus noise); displayed: the output's displayed total over the input's;
      stuck    the denoised energy in blocks whose windowed input is dark (< DARK of the mean), over the denoised total;
      age      for those dark blocks, frames since their input was last lit (> LIT of the mean), weighted by their output;
               long ages mean history that does not decay. p95 and the longest age still above STUCK of the mean.
      lag      per block whose input changes by more than 2x (in log2(1 + x / mean)), the delay in frames that best lines
               the denoised series up with the input's: the denoiser's lag, which noise doesn't move.
      paired   per frame, the output's mean over the input's and the 32x32 block |d| (valid with a moving camera too).
    Two capture layouts:
      DENOISE_DEBUG_PAIR (trail --stages 1): stochastic_diffuse r denoised, g noisy luminance. One stage, after the spatial pass.
      DENOISE_DEBUG_VIEW 5 with the HDR image (--stages 3): stochastic_diffuse rgb the denoised total and a the noisy luminance,
      stochastic_specular a the temporal pass's result, hdr DLSS's output. Three stages from the same frames:
        temporal    the history before the spatial filter, remodulated;
        spatial     the denoiser's output;
        after DLSS  the HDR image's luminance less the rest of the frame (the sun's and everything else stochastic lighting
                    doesn't make, which is static under the held camera): per block, the median over the frames of the HDR
                    image less the denoised output. What DLSS adds to the trail over the frames is kept (its lag and ghosting
                    of the moving part); what it does to the static part, its darkening or brightening of the mean, cancels
                    by construction, so read the after-DLSS mean ratios as the denoiser's, not DLSS's.
    Writes <prefix>_<tag>_trail.png: the last frame's denoised luminance in grey, stuck blocks in red by brightness."""
    from PIL import Image
    k, STUCK = TRAIL_K, TRAIL_STUCK
    for tag in tags:
        paths = sorted(glob.glob(os.path.join(directory, f'{prefix}_{tag}_stochastic_diffuse_*.dds')), key=frame_index)
        if not paths:
            print(f'{tag}: no frames')
            continue
        three = os.path.exists(os.path.join(directory, f'{prefix}_{tag}_hdr_1.dds'))
        rb, gb, tb, pb, last = [], [], [], [], None
        for p in paths:
            if three:
                i = frame_index(p)
                d = read_dds(p, 4)
                den = d[..., :3] @ LUMA
                noisy = d[..., 3]
                temporal = read_dds(os.path.join(directory, f'{prefix}_{tag}_stochastic_specular_{i}.dds'), 4)[..., 3]
                post = read_dds(os.path.join(directory, f'{prefix}_{tag}_hdr_{i}.dds')) @ LUMA
            else:
                x = read_dds(p)
                den, noisy, temporal, post = x[..., 0], x[..., 1], None, None
            h, w = (den.shape[0] // k) * k, (den.shape[1] // k) * k
            rb.append(blocks(den[:h, :w].astype(np.float64), k))
            gb.append(blocks(noisy[:h, :w].astype(np.float64), k))
            if three:
                tb.append(blocks(temporal[:h, :w].astype(np.float64), k))
                pb.append(blocks(post[:h, :w].astype(np.float64), k))
            last = den[:h, :w].astype(np.float64)
        rb, gb = np.stack(rb), np.stack(gb)
        stages = []
        if three:
            tb, pb = np.stack(tb), np.stack(pb)
            rest = np.median(pb - rb, axis=0)
            stages.append(('temporal', tb))
        stages.append(('spatial', rb))
        if three:
            stages.append(('after DLSS', np.maximum(pb - rest[None], 0.0)))
        spatial = None
        for stage, values in stages:
            t = trail_metrics(values, gb)
            print(fmt_trail(f'{tag} {stage}' if three else tag, t))
            if stage == 'spatial':
                spatial = t
                # The paired per-frame ratio's series (for an event: how many frames until the output follows the input).
                print(f'{"":<24} frame ratios: {" ".join(f"{x:.3f}" for x in t["frame_ratio"])}')
        if three:
            print(f'{"":<24} after DLSS: rest of the frame (median HDR less denoised) {float(rest.mean()):.4f} against the input mean '
                  f'{float(gb.mean()):.4f}')
        # Overlay: last frame, grey x/(1+x), stuck blocks of the last frame tinted red by their output.
        dark, m = spatial['dark'], spatial['m']
        y = np.maximum(last, 0.0)
        y = y / (1.0 + y)
        rgb = np.repeat(y[..., None], 3, axis=2)
        mask = np.kron((dark[-1] & (rb[-1] > STUCK * m)).astype(np.float64), np.ones((k, k)))
        strength = np.kron(np.clip(rb[-1] / (4.0 * m), 0.0, 1.0), np.ones((k, k))) * mask
        rgb[..., 0] = np.maximum(rgb[..., 0], strength)
        rgb[..., 1] *= 1.0 - 0.7 * mask
        rgb[..., 2] *= 1.0 - 0.7 * mask
        rgb = np.where(rgb <= 0.0031308, 12.92 * rgb, 1.055 * np.power(rgb, 1.0 / 2.4) - 0.055)
        path = os.path.join(out, f'{prefix}_{tag}_trail.png')
        Image.fromarray(np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8)).save(path)
        print(f'{"":<24} {path}')


def cmd_still(directory, prefix, variants=()):
    """Frozen lights (stochastic_scaling.sh still): D and E frames of DENOISE_DEBUG_VIEW 1 (r denoised, g noisy, b temporal-only
    luminance), before and after R, frames of the converged reference (luminance of each target). Means over the reference's,
    linear and as displayed (x / (1 + x) of 8x8 block means, the frames averaged first), and the denoised over the noisy input
    the way the trail detector measures it (the doc's "still lights, displayed"). D against E is the scene's drift. With
    variants, each has its own D.<name> and E.<name> sets against the one reference."""
    if variants:
        for v in variants:
            print(f'--- {v}')
            still_sets(directory, prefix, (f'D.{v}', f'E.{v}'), v)
        return
    still_sets(directory, prefix, ('D', 'E'), '')


def still_sets(directory, prefix, names, label):
    k = TRAIL_K
    rpaths = sorted(glob.glob(os.path.join(directory, f'{prefix}_R_stochastic_diffuse_*.dds')), key=frame_index)
    sets = {}
    for name in names:
        paths = sorted(glob.glob(os.path.join(directory, f'{prefix}_{name}_stochastic_diffuse_*.dds')), key=frame_index)
        if not paths:
            continue
        stacks = [[], [], []]
        for p in paths:
            x = read_dds(p).astype(np.float64)
            for c in range(3):
                stacks[c].append(blocks(x[..., c], k))
        sets[name] = [np.stack(s) for s in stacks]
    if not sets or not rpaths:
        print(f'still: frames missing (denoised sets {list(sets)}, reference {len(rpaths)})')
        return
    ref = None
    for p in rpaths:
        i = frame_index(p)
        y = read_dds(p).astype(np.float64) + read_dds(os.path.join(directory, f'{prefix}_R_stochastic_specular_{i}.dds')).astype(np.float64)
        b = blocks(y, k)
        ref = b if ref is None else ref + b
    ref /= len(rpaths)
    shown = lambda a: (a / (1.0 + a)).sum()
    print(f'frames: denoised {", ".join(f"{n} {s[0].shape[0]}" for n, s in sets.items())}, reference {len(rpaths)}; '
          f'reference mean {ref.mean():.4f}')
    both = {}
    for name, (rb, gb, tb) in sets.items():
        h, w = min(ref.shape[0], rb.shape[1]), min(ref.shape[1], rb.shape[2])
        r = ref[:h, :w]
        for kind, a in (('denoised', rb), ('temporal only', tb), ('noisy input', gb)):
            a = a[:, :h, :w]
            both.setdefault(kind, []).append((a.mean() / r.mean(), shown(a.mean(axis=0)) / shown(r)))
            print(f'{name} {kind:<14} mean / reference {a.mean() / r.mean():.4f}  displayed {shown(a.mean(axis=0)) / shown(r):.4f}  '
                  f'(frames {np.min(a.mean(axis=(1, 2))) / r.mean():.4f}-{np.max(a.mean(axis=(1, 2))) / r.mean():.4f})')
        t = trail_metrics(rb, gb)
        print(f'{name} denoised / noisy input as the trail detector measures it: {t["ratio"]:.4f} (displayed {t["shown"]:.4f})')
        # Noise against the reference, relative to its mean: block |d| of each frame (all the error) and of the set's mean
        # (what lasts over its frames: slow noise and bias), at 8x8 and 32x32. The reference's own noise is common to all sets.
        a = rb[:, :h, :w]
        h4, w4 = (h // 4) * 4, (w // 4) * 4
        to32 = lambda x: x[..., :h4, :w4].reshape(x.shape[:-2] + (h4 // 4, 4, w4 // 4, 4)).mean(axis=(-3, -1))
        noise = (np.abs(a - r).mean() / r.mean(), np.abs(a.mean(axis=0) - r).mean() / r.mean(),
                 np.abs(to32(a) - to32(r)).mean() / r.mean(), np.abs(to32(a.mean(axis=0)) - to32(r)).mean() / r.mean())
        both.setdefault('noise', []).append(noise)
        print(f'{name} denoised against the reference, block |d| / mean: 8x8 frames {noise[0]:.4f} set mean {noise[1]:.4f}; '
              f'32x32 frames {noise[2]:.4f} set mean {noise[3]:.4f}')
    noise = both.pop('noise', None)
    if noise:
        n = np.mean(noise, axis=0)
        print(f'{label + " " if label else ""}mean of the sets: noise 8x8 frames {n[0]:.4f} set mean {n[1]:.4f}; 32x32 frames {n[2]:.4f} '
              f'set mean {n[3]:.4f}')
    for kind, values in both.items():
        print(f'{label + " " if label else ""}mean of the sets: {kind:<14} {np.mean([v[0] for v in values]):.4f}  '
              f'displayed {np.mean([v[1] for v in values]):.4f}')


def cmd_cross(directory, prefix, out, tags):
    """Cross-surface history (stochastic_scaling.sh trail --debug-view 6): stochastic_diffuse frames of DENOISE_DEBUG_VIEW 6, r the
    denoised and a the noisy luminance, b the temporal pass's result, g the share of each pixel's reprojected history weight
    that came from off its plane. The trail numbers after the temporal and spatial passes (cmd_trail), then how much of the
    stuck-block population (dark input, lit output) was reached by cross-surface history: per 8x8 block and frame the share
    of pixels whose cross share exceeds CROSS_PIXEL, carried as a maximum since the block's input was last lit, and the
    stuck block-frames (and their output) whose carried share exceeds 1/64 (a pixel) and CROSS_BLOCK. The same over every
    block-frame is the base rate."""
    k = TRAIL_K
    CROSS_PIXEL, CROSS_BLOCK = 0.25, 0.10
    for tag in tags:
        paths = sorted(glob.glob(os.path.join(directory, f'{prefix}_{tag}_stochastic_diffuse_*.dds')), key=frame_index)
        if not paths:
            print(f'{tag}: no frames')
            continue
        rb, gb, tb, cb, cmean = [], [], [], [], []
        for p in paths:
            d = read_dds(p, 4).astype(np.float64)
            h, w = (d.shape[0] // k) * k, (d.shape[1] // k) * k
            d = d[:h, :w]
            rb.append(blocks(d[..., 0], k))
            gb.append(blocks(d[..., 3], k))
            tb.append(blocks(d[..., 2], k))
            cb.append(blocks((d[..., 1] > CROSS_PIXEL).astype(np.float64), k))
            valid = d[..., 3] >= 0.0
            cmean.append(float(d[..., 1][valid].mean()))
        rb, gb, tb, cb = np.stack(rb), np.stack(gb), np.stack(tb), np.stack(cb)
        for stage, values in (('temporal', tb), ('spatial', rb)):
            t = trail_metrics(values, gb)
            print(fmt_trail(f'{tag} {stage}', t))
        # Carried cross share: the largest since the block's input was last lit (trail_metrics' windowed truth and LIT).
        n, m = len(rb), gb.mean()
        c = np.cumsum(np.concatenate([np.zeros((1,) + gb.shape[1:]), gb]), axis=0)
        lo, hi = np.clip(np.arange(n) - TRAIL_HALF, 0, n), np.clip(np.arange(n) + TRAIL_HALF + 1, 0, n)
        truth = (c[hi] - c[lo]) / (hi - lo)[:, None, None]
        carried = np.zeros_like(cb)
        held = np.zeros(cb.shape[1:])
        for i in range(n):
            held = np.where(truth[i] > TRAIL_LIT * m, cb[i], np.maximum(held, cb[i]))
            carried[i] = held
        dark = truth < TRAIL_DARK * m
        for stage, values in (('temporal', tb), ('spatial', rb)):
            stuck = dark & (values > TRAIL_STUCK * m)
            count = int(stuck.sum())
            if count == 0:
                print(f'{tag} {stage}: no stuck blocks')
                continue
            energy = values[stuck]
            parts = []
            for limit, name in ((1.0 / 64.0, 'any pixel'), (CROSS_BLOCK, f'>{CROSS_BLOCK:.0%} of pixels')):
                hit = carried[stuck] > limit * 0.999
                parts.append(f'{name}: {hit.mean():.1%} of stuck block-frames, {energy[hit].sum() / energy.sum():.1%} of their output '
                             f'(base rate {(carried > limit * 0.999).mean():.1%} of all block-frames)')
            now = cb[stuck].mean()
            print(f'{tag} {stage}: {count} stuck block-frames; cross-surface since last lit, {"; ".join(parts)}; '
                  f'pixels over {CROSS_PIXEL} in the stuck blocks this frame {now:.2%} (all blocks {cb.mean():.2%})')
        print(f'{tag}: mean per-pixel cross share {np.mean(cmean):.4f}, pixels over {CROSS_PIXEL} {cb.mean():.2%}')


def cmd_confidence(directory, prefix, out, tags):
    """Light-mixture confidence (stochastic_scaling.sh trail --debug-view 8; docs/stochastic_lighting.md "Guide-normalized accumulation"):
    stochastic_diffuse frames of DENOISE_DEBUG_VIEW 8, r the denoised and a the noisy luminance, b the temporal pass's result,
    g the factor c the history's length was scaled by this frame (1 where nothing was compared or the setting is off). The
    trail numbers after the temporal and spatial passes (cmd_trail), then c over the surface's pixels (background, all four 0,
    left out): its mean, the fraction of pixels below 0.9 and 0.5 (plain and weighted by the denoised luminance), and per
    frame the mean and the fraction below 0.9 (for a scene with an event, when it fires)."""
    k = TRAIL_K
    for tag in tags:
        paths = sorted(glob.glob(os.path.join(directory, f'{prefix}_{tag}_stochastic_diffuse_*.dds')), key=frame_index)
        if not paths:
            print(f'{tag}: no frames')
            continue
        rb, gb, tb, means, below, below_w, below5 = [], [], [], [], [], [], []
        for p in paths:
            d = read_dds(p, 4).astype(np.float64)
            h, w = (d.shape[0] // k) * k, (d.shape[1] // k) * k
            d = d[:h, :w]
            rb.append(blocks(d[..., 0], k))
            gb.append(blocks(d[..., 3], k))
            tb.append(blocks(d[..., 2], k))
            valid = (d[..., 0] > 0.0) | (d[..., 1] > 0.0) | (d[..., 2] > 0.0) | (d[..., 3] > 0.0)
            c, y = d[..., 1][valid], np.maximum(d[..., 0][valid], 0.0)
            means.append(float(c.mean()) if c.size else 1.0)
            below.append(float((c < 0.9).mean()) if c.size else 0.0)
            below5.append(float((c < 0.5).mean()) if c.size else 0.0)
            below_w.append(float(y[c < 0.9].sum() / max(y.sum(), 1e-12)) if c.size else 0.0)
        rb, gb, tb = np.stack(rb), np.stack(gb), np.stack(tb)
        for stage, values in (('temporal', tb), ('spatial', rb)):
            t = trail_metrics(values, gb)
            print(fmt_trail(f'{tag} {stage}', t))
            if stage == 'spatial':
                print(f'{"":<24} frame ratios: {" ".join(f"{x:.3f}" for x in t["frame_ratio"])}')
        print(f'{tag}: confidence mean {np.mean(means):.4f}; pixels below 0.9 {100 * np.mean(below):.3f}% '
              f'(by denoised luminance {100 * np.mean(below_w):.3f}%), below 0.5 {100 * np.mean(below5):.3f}%')
        print(f'{"":<24} per frame mean: {" ".join(f"{x:.3f}" for x in means)}')
        print(f'{"":<24} per frame below 0.9 (%): {" ".join(f"{100 * x:.2f}" for x in below)}')


DLSS_SCALES = (1, 8, 32)


def dlss_frames(directory, prefix, tag):
    """A capture_set's frames: (pre, post) stacks, pre the luminance of stochastic diffuse + specular (before DLSS), post the
    upscaled HDR image's (after DLSS), both float32 [frames, h, w]."""
    n = len(glob.glob(os.path.join(directory, f'{prefix}_{tag}_hdr_*.dds')))
    pre, post = [], []
    for i in range(1, n + 1):
        base = os.path.join(directory, f'{prefix}_{tag}')
        pre.append(read_dds(f'{base}_stochastic_diffuse_{i}.dds') + read_dds(f'{base}_stochastic_specular_{i}.dds'))
        post.append(read_dds(f'{base}_hdr_{i}.dds'))
    if n == 0:
        return np.zeros((0, 1, 1), np.float32), np.zeros((0, 1, 1), np.float32)
    return np.stack(pre), np.stack(post)


def dlss_sets(directory, prefix, tag):
    """capture_sets' sets <tag>.1, <tag>.2, ... (or the single set <tag>): lists of (pre, post) stacks."""
    names = sorted({os.path.basename(p)[len(prefix) + 1:].rsplit('_hdr_', 1)[0]
                    for p in glob.glob(os.path.join(directory, f'{prefix}_{tag}.*_hdr_1.dds'))},
                   key=lambda name: int(name.rsplit('.', 1)[1]))
    if not names and os.path.exists(os.path.join(directory, f'{prefix}_{tag}_hdr_1.dds')):
        names = [tag]
    return [dlss_frames(directory, prefix, name) for name in names]


def stack_blocks(x, k):
    if k == 1:
        return x.astype(np.float64)
    f, h, w = x.shape[0], x.shape[1] // k, x.shape[2] // k
    return x[:, :h * k, :w * k].reshape(f, h, k, w, k).mean(axis=(2, 4), dtype=np.float64)


def dlss_errors(sets, truth, m):
    """Per scale (pixels, 8x8 and 32x32 block means), RMS relative to m, from S sets of N consecutive frames each:
    total: a frame against the truth.
    fast: the flicker within a set (the per-pixel standard deviation over its N frames), noise that is new every few frames.
    slow: the standard deviation of the sets' means about each other, less what the fast part contributes to it (fast^2 / N):
      noise that holds over a set's frames but not between sets (crawling or blotchy noise lasting tens of frames).
    bias: the mean of all frames against the truth, less what fast and slow noise leave in that mean: what every set shares
      (darkening, blur, edges).
    total^2 is about bias^2 + slow^2 + fast^2."""
    out = {}
    for k in DLSS_SCALES:
        t = stack_blocks(truth[None], k)[0]
        means, fast2, total2, n = [], [], [], 0
        for frames in sets:
            f = stack_blocks(frames, k)
            means.append(f.mean(axis=0))
            fast2.append(f.var(axis=0).mean())
            total2.append(((f - t) ** 2).mean())
            n = f.shape[0]
        means = np.stack(means)
        s = means.shape[0]
        fast2 = float(np.mean(fast2))
        slow2 = max(float(means.var(axis=0, ddof=1).mean()) - fast2 / max(n - 1, 1), 0.0) if s > 1 else float('nan')
        mean2 = float(((means.mean(axis=0) - t) ** 2).mean())
        bias2 = max(mean2 - ((slow2 if s > 1 else 0.0) + fast2 / max(n - 1, 1)) / s, 0.0)
        out[k] = {'total': np.sqrt(np.mean(total2)) / m, 'bias': np.sqrt(bias2) / m,
                  'slow': np.sqrt(slow2) / m if s > 1 else float('nan'), 'fast': np.sqrt(fast2) / m}
    return out


def dlss_row(name, e, stage):
    def cell(c):
        slow = '-' if np.isnan(c['slow']) else f"{c['slow']:.4f}"
        return f"{c['total']:.4f} / {c['bias']:.4f} / {slow} / {c['fast']:.4f}"
    return f'| {name} | {stage} | ' + ' | '.join(cell(e[k]) for k in DLSS_SCALES) + ' |'


def to_srgb8(x):
    x = np.clip(x, 0.0, None)
    x = x / (1.0 + x)
    x = np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)
    return np.clip(x * 255.0 + 0.5, 0, 255).astype(np.uint8)


def cmd_dlss(directory, prefix, out, tags):
    """The post-DLSS noise metric (docs/stochastic_lighting.md "Running the benchmarks"). The truth before DLSS is the converged
    reference's stochastic lighting (R1's frames, averaged). The truth after DLSS is DLSS's output of the reference's per-frame
    estimate with a grid of rays per light (P1's and P2's frames, averaged): the converged reference is jitter-averaged, so
    DLSS would reconstruct its edges and texture differently from a frame whose lighting jitters with the rest of the image,
    while the per-frame estimate jitters with it and is all but noiseless. So DLSS's own reconstruction is not counted as
    error. Every error is relative to the reference's mean stochastic luminance, so before and after are on one scale. The
    per-frame reference's rows are the floor (before DLSS its fast part is jitter: the lighting of a jittered frame), and P1
    against P2 its drift."""
    r1pre, _ = dlss_frames(directory, prefix, 'R1')
    psets = dlss_sets(directory, prefix, 'P1') + dlss_sets(directory, prefix, 'P2')
    gpre = r1pre.mean(axis=0, dtype=np.float64)
    gpost = np.mean([post.mean(axis=0, dtype=np.float64) for _, post in psets], axis=0)
    mean_stochastic, mean_final = float(gpre.mean()), float(gpost.mean())
    # Display-referred by default: a pixel's error is weighted by the slope of y / (y + Y) at the truth, Y the truth's mean
    # final luminance, and given in percent of white, so a highlight near a light (luminance 5 and more, under 0.5% of pixels)
    # doesn't outweigh the whole image, and an error in a shadow counts for more than one in a lit area. Before DLSS the rest
    # of the image (the truth's final image less its stochastic lighting; the same resolution at DLAA) enters the slope, so
    # both stages are weighted as errors in the final image. The weight is fixed per pixel, so the decomposition stays exact
    # (the curve applied to each frame would add jitter's Jensen gap). SS_DLSS_LINEAR=1 measures linear luminance relative to
    # the mean stochastic luminance instead.
    linear = os.environ.get('SS_DLSS_LINEAR') == '1'
    if linear:
        before_space = after_space = lambda x: x
        m = mean_stochastic
    else:
        rest = np.maximum(gpost - gpre, 0.0)
        weight_before = (mean_final / (gpre + rest + mean_final) ** 2).astype(np.float32)
        weight_after = (mean_final / (gpost + mean_final) ** 2).astype(np.float32)
        before_space = lambda x: x * weight_before
        after_space = lambda x: x * weight_after
        m = 0.01
    tpre, tpost = before_space(gpre), after_space(gpost)
    print(f'truth: converged reference {r1pre.shape[0]} frames, per-frame reference {" + ".join(str(p.shape[0]) for p, _ in psets)} '
          f'frames, {gpre.shape[1]}x{gpre.shape[0]}; mean stochastic luminance {mean_stochastic:.4f}, mean final luminance {mean_final:.4f}')
    print('cells: total / bias / slow / fast RMS (slow: between sets; fast: within a set), ' +
          ('linear, relative to the mean stochastic luminance' if linear else 'display-referred (weighted by the slope of y / (y + mean final) at the truth), percent of white'))
    print('| Variant (sets x frames) | Stage | ' + ' | '.join(f'{k}x{k}' if k > 1 else 'Pixel' for k in DLSS_SCALES) + ' |')
    print('|' + '---|' * (2 + len(DLSS_SCALES)))
    print(dlss_row('per-frame reference (floor)', dlss_errors([before_space(p) for p, _ in psets], tpre, m), 'before'))
    print(dlss_row('per-frame reference (floor)', dlss_errors([after_space(q) for _, q in psets], tpost, m), 'after'))
    del r1pre, psets
    firsts, summary = {}, {}
    for tag in tags:
        sets = dlss_sets(directory, prefix, tag)
        if not sets:
            print(f'| {tag} | no frames | | | |')
            continue
        label = f'{tag} ({len(sets)} x {sets[0][0].shape[0]})'
        before = dlss_errors([before_space(p) for p, _ in sets], tpre, m)
        after = dlss_errors([after_space(q) for _, q in sets], tpost, m)
        print(dlss_row(label, before, 'before'))
        print(dlss_row(label, after, 'after'))
        ratio = float(np.mean([p.mean() for p, _ in sets])) / mean_stochastic
        shift = float(np.mean([q.mean() for _, q in sets]) - gpost.mean()) / mean_stochastic
        print(f'|  | mean | {ratio:.4f} before; after, the difference {shift:+.4f} of the mean stochastic luminance | | |')
        summary[tag] = (before, after)
        pre, post = sets[0]
        firsts[tag] = (pre[0].astype(np.float64), post[0].astype(np.float64),
                       ((post.astype(np.float64) - gpost[None]) ** 2).mean(axis=0))
        del sets, pre, post
    # Noise after DLSS, the headline: sqrt(slow^2 + fast^2) per scale.
    print('noise after DLSS, sqrt(slow^2 + fast^2), pixel / 8x8 / 32x32:')
    for tag, (_, after) in summary.items():
        print(f'  {tag:<16} ' + ' / '.join(f"{np.sqrt(np.nan_to_num(after[k]['slow']) ** 2 + after[k]['fast'] ** 2):.4f}" for k in DLSS_SCALES))
    if not firsts or not out:
        return
    # Crops where the first denoised variant's after-DLSS error is largest: per row (the reference, then each variant) its
    # first frame before DLSS, after DLSS, and the after-DLSS error of that frame (x8, around grey).
    from PIL import Image
    first = next((t for t in firsts if t != 'noisy'), next(iter(firsts)))
    err = stack_blocks(firsts[first][2][None], 32)[0]
    ch, cw = 8, 12  # 256 x 384 pixels
    best, at = -1.0, (0, 0)
    for y in range(0, err.shape[0] - ch + 1):
        for x in range(0, err.shape[1] - cw + 1):
            v = err[y:y + ch, x:x + cw].mean()
            if v > best:
                best, at = v, (y, x)
    y0, x0 = at[0] * 32, at[1] * 32
    sl = (slice(y0, y0 + ch * 32), slice(x0, x0 + cw * 32))
    exposure = 0.5 / max(float(gpost[sl].mean()), 1e-6)
    exposure_pre = 0.5 / max(float(gpre[sl].mean()), 1e-6)

    def row(cells):
        return np.concatenate([np.pad(c, 2, constant_values=255) for c in cells], axis=1)

    rows = [row([to_srgb8(gpre[sl] * exposure_pre), to_srgb8(gpost[sl] * exposure), np.full(gpost[sl].shape, 128, np.uint8)])]
    for tag, (pre0, post0, _) in firsts.items():
        diff = np.clip(128.0 + 8.0 * 128.0 * (post0[sl] - gpost[sl]) * exposure, 0, 255).astype(np.uint8)
        rows.append(row([to_srgb8(pre0[sl] * exposure_pre), to_srgb8(post0[sl] * exposure), diff]))
    path = os.path.join(out, f'{prefix}_crop.png')
    Image.fromarray(np.concatenate(rows, axis=0)).save(path)
    print(f'crop at pixel ({x0}, {y0}), 384x256: {path}; rows: reference, ' + ', '.join(firsts) +
          '; columns: before DLSS, after DLSS, after-DLSS error x8')


def self_noise(sets, m):
    """dlss_errors' slow and fast parts, which need no truth: fast is the per-pixel variance within a set, slow the variance of
    the sets' means about each other less fast's share of it. Per scale, RMS relative to m."""
    out = {}
    for k in DLSS_SCALES:
        means, fast2, n = [], [], 0
        for frames in sets:
            f = stack_blocks(frames, k)
            means.append(f.mean(axis=0))
            fast2.append(f.var(axis=0).mean())
            n = f.shape[0]
        means = np.stack(means)
        fast2 = float(np.mean(fast2))
        slow2 = max(float(means.var(axis=0, ddof=1).mean()) - fast2 / max(n - 1, 1), 0.0) if means.shape[0] > 1 else float('nan')
        out[k] = {'slow': np.sqrt(slow2) / m if means.shape[0] > 1 else float('nan'), 'fast': np.sqrt(fast2) / m}
    return out


def cmd_dlssnoise(directory, prefix, tags):
    """Noise after DLSS without a reference (stochastic_scaling.sh dlssnoise; docs/stochastic_lighting.md "Running the benchmarks"). Each variant's sets of consecutive frames, before and after DLSS, give its fast noise (within a set) and slow
    noise (between the sets' means), as in cmd_dlss, which never needed its truth for those two. The display weight is the slope
    of y / (y + Y) at the mean of every tag's frames but the undenoised input's (Y its mean final luminance; before DLSS the rest
    of the image, post less pre, enters it), in percent of white, the same weight for every variant. Not measured: bias and noise
    slower than a variant's span of sets (both are what all its sets share); take the bias from `still`. A variant's mean against
    the pool's is printed as relative brightness, not bias."""
    pooled_pre = pooled_post = None
    count = 0
    present = []
    for tag in tags:
        sets = dlss_sets(directory, prefix, tag)
        if not sets:
            print(f'{tag}: no frames')
            continue
        present.append(tag)
        if tag == 'noisy':
            continue
        for pre, post in sets:
            sp, sq = pre.sum(axis=0, dtype=np.float64), post.sum(axis=0, dtype=np.float64)
            pooled_pre = sp if pooled_pre is None else pooled_pre + sp
            pooled_post = sq if pooled_post is None else pooled_post + sq
            count += pre.shape[0]
        del sets
    if not count:
        print('dlssnoise: no denoised frames')
        return
    gpre, gpost = pooled_pre / count, pooled_post / count
    mean_stochastic, mean_final = float(gpre.mean()), float(gpost.mean())
    rest = np.maximum(gpost - gpre, 0.0)
    weight_before = (mean_final / (gpre + rest + mean_final) ** 2).astype(np.float32)
    weight_after = (mean_final / (gpost + mean_final) ** 2).astype(np.float32)
    print(f'pool: {count} frames, {gpre.shape[1]}x{gpre.shape[0]}; mean stochastic luminance {mean_stochastic:.4f}, mean final {mean_final:.4f}')
    print("cells: slow / fast RMS, display-referred (slope of y / (y + mean final) at the pool's mean), percent of white; "
          "slow: between a variant's sets, fast: within a set; no bias (self-referenced)")
    print('| Variant (sets x frames) | Stage | ' + ' | '.join(f'{k}x{k}' if k > 1 else 'Pixel' for k in DLSS_SCALES) + ' | Mean / pool |')
    print('|' + '---|' * (3 + len(DLSS_SCALES)))
    headline = {}
    for tag in present:
        sets = dlss_sets(directory, prefix, tag)
        label = f'{tag} ({len(sets)} x {sets[0][0].shape[0]})'
        before = self_noise([p * weight_before for p, _ in sets], 0.01)
        after = self_noise([q * weight_after for _, q in sets], 0.01)
        pre_ratio = float(np.mean([p.mean() for p, _ in sets])) / mean_stochastic
        post_ratio = float(np.mean([q.mean() for _, q in sets])) / mean_final
        cell = lambda c: ('-' if np.isnan(c['slow']) else f"{c['slow']:.4f}") + f" / {c['fast']:.4f}"
        print(f'| {label} | before | ' + ' | '.join(cell(before[k]) for k in DLSS_SCALES) + f' | {pre_ratio:.4f} |')
        print(f'| {label} | after | ' + ' | '.join(cell(after[k]) for k in DLSS_SCALES) + f' | {post_ratio:.4f} |')
        headline[tag] = after
        del sets
    print('noise after DLSS, sqrt(slow^2 + fast^2), pixel / 8x8 / 32x32; slow alone:')
    for tag, after in headline.items():
        total = ' / '.join(f"{np.sqrt(np.nan_to_num(after[k]['slow']) ** 2 + after[k]['fast'] ** 2):.4f}" for k in DLSS_SCALES)
        slow = ' / '.join(f"{np.nan_to_num(after[k]['slow']):.4f}" for k in DLSS_SCALES)
        print(f'  {tag:<16} {total}   slow {slow}')


def cmd_summary(md):
    for line in open(md, encoding='utf-8'):
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) >= 3 and cells[1] == 'GPU time' and cells[0] != 'Pass':
            print(f'{cells[0]}\t{cells[2]}')


SHORT = [
    ('StochasticGuidePass', 'Guide'),
    ('StochasticSamplePass', 'Sample'),
    ('StochasticSelectPass', 'Select'),
    ('StochasticTracePass', 'Trace'),
    ('StochasticShadePass', 'Shade'),
    ('StochasticReferencePass', 'Reference'),
    ('StochasticDenoiseTemporalPass', 'Temporal'),
    ('StochasticDenoiseSpatialPass', 'Spatial'),
    ('DeferredShadingPass', 'Deferred'),
    ('LightGridPreparePass', 'GridPrepare'),
    ('LightGridDepthBoundsPass', 'GridDepth'),
    ('LightCullingPass', 'Culling'),
]


SPLIT = ('StochasticSelectPass', 'StochasticTracePass', 'StochasticShadePass')


def cmd_sample(md):
    """The estimator's time in a profile summary: the single pass, or the split passes' sum."""
    times = {}
    for line in open(md, encoding='utf-8'):
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) >= 3 and cells[1] == 'GPU time' and cells[0] != 'Pass':
            times[cells[0]] = float(cells[2])
    if times.get('StochasticSamplePass'):
        print(f"{times['StochasticSamplePass']:.3f}")
    elif all(p in times for p in SPLIT):
        print(f"{sum(times[p] for p in SPLIT):.3f} (" + ' + '.join(f'{times[p]:.3f}' for p in SPLIT) + ')')
    else:
        print('-')


def cmd_table(tsv):
    rows, order = {}, []
    for line in open(tsv, encoding='utf-8'):
        parts = line.rstrip('\n').split('\t')
        if len(parts) != 6:
            continue
        key = tuple(parts[:4])  # range, scale, variant, dropped
        if key not in rows:
            rows[key] = {}
            order.append(key)
        rows[key][parts[4]] = float(parts[5])
    # The split estimator (docs/stochastic_lighting.md "Kernel split"): its Sample column is the sum of its three passes.
    for r in rows.values():
        if not r.get('StochasticSamplePass') and all(p in r for p in SPLIT):
            r['StochasticSamplePass'] = sum(r[p] for p in SPLIT)
    present = [(p, s) for p, s in SHORT if any(p in r for r in rows.values())]
    head = ['Range', 'Scale', 'Lights', 'Variant'] + [s for _, s in present] + ['Dropped']
    table = [head]
    for key in order:
        rng, scale, variant, dropped = key
        lights = str(16 * int(scale)) if scale.isdigit() else '?'
        table.append([rng, scale, lights, variant] + [f'{rows[key][p]:.2f}' if p in rows[key] else '-' for p, _ in present] + [dropped])
    widths = [max(len(r[i]) for r in table) for i in range(len(head))]
    print('| ' + ' | '.join(h.ljust(w) for h, w in zip(head, widths)) + ' |')
    print('|' + '|'.join('-' * (w + 2) for w in widths) + '|')
    for r in table[1:]:
        print('| ' + ' | '.join(c.rjust(w) for c, w in zip(r, widths)) + ' |')


def cmd_dropped(log, skip):
    """Most dropped (froxel, light) pairs reported in the log after the first `skip` lines, or '-' with no report."""
    most = None
    pattern = re.compile(r'Light culling: (\d+) \((?:cluster|froxel), light\) pairs dropped in \d+ (?:clusters|froxels), at most (\d+)')
    warn = re.compile(r'Light culling: (\d+) \(froxel, light\) pairs dropped in \d+ froxels \(')
    with open(log, encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f):
            if n < skip:
                continue
            m = pattern.search(line)
            if m:
                most = max(most or 0, int(m.group(1)), int(m.group(2)))
                continue
            m = warn.search(line)
            if m:
                most = max(most or 0, int(m.group(1)))
    print('-' if most is None else most)


def cmd_guide(directory, prefix):
    """The guide (docs/stochastic_lighting.md "Guide-normalized accumulation"): consecutive frames <prefix>_G_stochastic_guide_<i> with the
    upscaler's jitter and <prefix>_J_..._<i> without, and the unshadowed reference written as the guide (STOCHASTIC_REFERENCE_
    UNSHADOWED 2) in <prefix>_U_stochastic_diffuse_<i>. r is diffuse, g specular."""
    def frames(tag, target):
        paths = sorted(glob.glob(os.path.join(directory, f'{prefix}_{tag}_{target}_*.dds')), key=frame_index)
        return [read_dds(p, 2) for p in paths]
    for tag, label in (('G', 'jitter on'), ('J', 'jitter off')):
        g = frames(tag, 'stochastic_guide')
        if len(g) < 2:
            print(f'{label}: {len(g)} frames')
            continue
        for c, lobe in ((0, 'diffuse'), (1, 'specular')):
            first = g[0][..., c]
            mean = float(first.mean())
            changed, rel, block = [], [], []
            for i in range(1, len(g)):
                x, y = g[i - 1][..., c], g[i][..., c]
                changed.append(float((x != y).mean()))
                rel.append(float(np.abs(y - x).mean()) / max(mean, 1e-12))
                block.append(float(np.abs(blocks(y, 16) - blocks(x, 16)).mean()) / max(mean, 1e-12))
            print(f'{label}, {lobe}: {len(g)} frames, mean {mean:.4g}; frame to frame: pixels changed {np.mean(changed):.4%} '
                  f'(max {np.max(changed):.4%}), pixel |d| {np.mean(rel):.4g}, 16x16 block |d| {np.mean(block):.4g} (of the mean)')
    j, u = frames('J', 'stochastic_guide'), frames('U', 'stochastic_diffuse')
    if j and u:
        guide, truth = j[-1], u[-1]
        for c, lobe in ((0, 'diffuse'), (1, 'specular')):
            g, t = guide[..., c], truth[..., c]
            valid = np.isfinite(t) & (t < 65000.0) & (g > 0.0)
            ratio = t[valid] / g[valid]
            print(f'truth / guide, {lobe}: sums {t[valid].sum() / g[valid].sum():.5f}, pixel median {np.median(ratio):.5f}, '
                  f'p1 / p99 {np.percentile(ratio, 1):.5f} / {np.percentile(ratio, 99):.5f}, within 1e-3: {np.mean(np.abs(ratio - 1.0) < 1e-3):.3%}, '
                  f'guide 0 where truth > 0: {np.mean((g <= 0.0) & (t > 0.0)):.4%}, truth clipped: {np.mean(t >= 65000.0):.4%}')


def cmd_paired(directory, prefix, out=None):
    """The paired ratio's denominator (docs/stochastic_lighting.md "The paired ratio"), frozen scene: frames
    <prefix>_P_<target>_<i> of DENOISE_DEBUG_VIEW 7 (stochastic_diffuse: H_X before and after the spatial filter, diffuse and
    specular; stochastic_specular: the noisy x and the guide where it was taken) and stochastic_guide. x = lum_d(X / albedo) / G
    has expectation 1 per pixel, so its mean (and the G-weighted sum X / G) over pixels and frames, and the long history
    H_X = sum(a X) / sum(a G), should be 1. Deviations by 16x16 block, and by the block's guide (quartiles)."""
    def frames(target, channels):
        paths = sorted(glob.glob(os.path.join(directory, f'{prefix}_P_{target}_*.dds')), key=frame_index)
        return np.stack([read_dds(p, channels) for p in paths]).astype(np.float64) if paths else None
    d, s, g = frames('stochastic_diffuse', 4), frames('stochastic_specular', 4), frames('stochastic_guide', 2)
    if d is None or s is None or g is None:
        print(f'paired: frames missing (diffuse {d is not None}, specular {s is not None}, guide {g is not None})')
        return
    n = min(len(d), len(s), len(g))
    d, s, g = d[:n], s[:n], g[:n]
    print(f'paired: {n} frames, {d.shape[2]}x{d.shape[1]}')
    k = 16
    for c, lobe in ((0, 'diffuse'), (1, 'specular')):
        x, gs = s[..., 2 * c], s[..., 2 * c + 1]
        ok = (gs > 0.0) & (gs < 65000.0) & np.isfinite(x) & (x < 65000.0)
        print(f'{lobe}: noisy x mean {x[ok].mean():.4f} (per frame {np.min([x[i][ok[i]].mean() for i in range(n)]):.3f}-'
              f'{np.max([x[i][ok[i]].mean() for i in range(n)]):.3f}), sum X / sum G {(x * gs)[ok].sum() / gs[ok].sum():.4f}, '
              f'x = 0 {np.mean(x[ok] == 0.0):.1%}, guide capped {np.mean(gs >= 65000.0):.3%}, x per-pixel std {x[ok].std():.4f} '
              f'(p1 / p99 {np.percentile(x[ok], 1):.3f} / {np.percentile(x[ok], 99):.3f})')
        gp = g[..., c]
        valid = (gp > 0.0) & np.isfinite(gp)
        for label, h in (('temporal H_X', d[..., c]), ('filtered H_X', d[..., 2 + c])):
            okh = valid & np.isfinite(h)
            per_frame = [h[i][okh[i]].mean() for i in range(n)]
            weighted = (h * gp)[okh].sum() / gp[okh].sum()
            print(f'{lobe}: {label} mean {h[okh].mean():.4f} (frames {np.min(per_frame):.4f}-{np.max(per_frame):.4f}), '
                  f'guide-weighted {weighted:.4f}, below 0.05 {np.mean(h[okh] < 0.05):.2%}, above 2 {np.mean(h[okh] > 2.0):.2%}')
        # Where it deviates: 16x16 blocks of the frames' mean history (valid pixels only), against the block's mean guide.
        h = d[..., 2 + c]
        okh = valid & np.isfinite(h)
        hm = np.where(okh, h, 0.0).sum(axis=0)
        cnt = okh.sum(axis=0).astype(np.float64)
        bh, bc, bg = blocks(hm, k), blocks(cnt, k), blocks(np.where(valid, gp, 0.0).mean(axis=0), k)
        full = bc >= 0.9 * n
        ratio = bh[full] / bc[full]
        gb = bg[full]
        q = np.percentile(gb, [25, 50, 75])
        bins = [ratio[gb <= q[0]], ratio[(gb > q[0]) & (gb <= q[1])], ratio[(gb > q[1]) & (gb <= q[2])], ratio[gb > q[2]]]
        print(f'{lobe}: 16x16 blocks of filtered H_X: p5 {np.percentile(ratio, 5):.3f} median {np.median(ratio):.3f} p95 '
              f'{np.percentile(ratio, 95):.3f}; by guide quartile (dim to bright) ' + ', '.join(f'{b.mean():.3f}' for b in bins))
        low = full & (np.where(bc > 0, bh / np.maximum(bc, 1.0), 1.0) < 0.75)
        if low.any():
            ys, xs = np.nonzero(low)
            print(f'{lobe}: {low.sum()} blocks ({low.mean():.2%}) below 0.75, mean {(bh[low] / bc[low]).mean():.3f}, their guide '
                  f'{bg[low].mean():.4g} (all blocks {bg[full].mean():.4g}), their share of the guide {bg[low].sum() / bg[full].sum():.3%}; '
                  f'block rows {ys.min()}-{ys.max()}, columns {xs.min()}-{xs.max()}')
        if out:
            from PIL import Image
            img = np.zeros(bh.shape + (3,), dtype=np.float64)
            r = np.where(bc > 0, bh / np.maximum(bc, 1.0), 1.0)
            dev = np.clip((r - 1.0) / 0.25, -1.0, 1.0)  # red above 1, blue below, full at +-25%
            img[..., 0] = 1.0 - np.maximum(-dev, 0.0)
            img[..., 1] = 1.0 - np.abs(dev)
            img[..., 2] = 1.0 - np.maximum(dev, 0.0)
            path = os.path.join(out, f'{prefix}_{lobe}_hx.png')
            Image.fromarray((img * 255.0 + 0.5).astype(np.uint8)).resize((bh.shape[1] * 4, bh.shape[0] * 4), Image.NEAREST).save(path)
            print(f'{lobe}: block map {path} (red above 1, blue below, full colour at 25%)')


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    if cmd == 'summary':
        cmd_summary(argv[2])
    elif cmd == 'table':
        cmd_table(argv[2])
    elif cmd == 'sample':
        cmd_sample(argv[2])
    elif cmd == 'bias':
        cmd_bias(argv[2], argv[3])
    elif cmd == 'noise':
        cmd_noise(argv[2], argv[3], argv[4:])
    elif cmd == 'png':
        cmd_png(argv[2], argv[3], argv[4], argv[5:])
    elif cmd == 'pair':
        cmd_pair(argv[2], argv[3], argv[4:])
    elif cmd == 'dlss':
        cmd_dlss(argv[2], argv[3], argv[4], argv[5:])
    elif cmd == 'dlssnoise':
        cmd_dlssnoise(argv[2], argv[3], argv[4:])
    elif cmd == 'trail':
        cmd_trail(argv[2], argv[3], argv[4], argv[5:])
    elif cmd == 'cross':
        cmd_cross(argv[2], argv[3], argv[4], argv[5:])
    elif cmd == 'confidence':
        cmd_confidence(argv[2], argv[3], argv[4], argv[5:])
    elif cmd == 'still':
        cmd_still(argv[2], argv[3], argv[4:])
    elif cmd == 'guide':
        cmd_guide(argv[2], argv[3])
    elif cmd == 'paired':
        cmd_paired(argv[2], argv[3], argv[4] if len(argv) > 4 else None)
    elif cmd == 'dropped':
        cmd_dropped(argv[2], int(argv[3]) if len(argv) > 3 else 0)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
