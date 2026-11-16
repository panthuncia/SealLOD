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
    stochastic_scaling_analysis.py trail DIR PREFIX OUT TAG...    consecutive DENOISE_DEBUG_PAIR frames of the sweep scene: stuck history

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


def read_dds(path):
    """Returns an HxWx3 float32 array from an uncompressed float DDS."""
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
    dtype, channels = layout
    count = w * h * channels
    a = np.frombuffer(b, dtype=dtype, count=count, offset=off).reshape(h, w, channels)
    if channels == 1:
        return a[..., 0].astype(np.float32)
    return a[..., :3].astype(np.float32)


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


def cmd_trail(directory, prefix, out, tags):
    """Moving-light trails: consecutive DENOISE_DEBUG_PAIR frames (r denoised, g noisy luminance; files <tag>_stochastic_diffuse_<i>)
    of the sweep scene. The noisy input is unbiased each frame, so a block whose input stays dark while its denoised output
    stays lit is history the denoiser kept after the light left. Per 8x8 block and frame, the input's truth is its mean over
    a window of +-W frames (the light moves a few blocks a frame, so the window blurs it a little; the stuck history this is
    after lasts far longer than the window). Reported, relative to the mean of the input over all frames:
      excess   the denoised output above the windowed input, as displayed (x / (1 + x) of the block means), summed over
               blocks and frames, over the input's total (departed light, plus noise); deficit the same below it (arrival
               lag, plus noise); displayed: the output's displayed total over the input's;
      stuck    the denoised energy in blocks whose windowed input is dark (< DARK of the mean), over the denoised total;
      age      for those dark blocks, frames since their input was last lit (> LIT of the mean), weighted by their output;
               long ages mean history that does not decay. p95 and the longest age still above STUCK of the mean.
      lag      per block whose input changes by more than 2x (in log2(1 + x / mean)), the delay in frames that best lines
               the denoised series up with the input's: the denoiser's lag, which noise doesn't move.
    Writes <prefix>_<tag>_trail.png: the last frame's denoised luminance in grey, stuck blocks in red by brightness."""
    from PIL import Image
    k, half = 8, 4
    DARK, LIT, STUCK = 0.02, 0.10, 0.25
    for tag in tags:
        paths = sorted(glob.glob(os.path.join(directory, f'{prefix}_{tag}_stochastic_diffuse_*.dds')),
                       key=lambda p: int(p.rsplit('_', 1)[1].split('.')[0]))
        if not paths:
            print(f'{tag}: no frames')
            continue
        rb, gb, last = [], [], None
        for p in paths:
            x = read_dds(p)
            r, g = x[..., 0].astype(np.float64), x[..., 1].astype(np.float64)
            h, w = (r.shape[0] // k) * k, (r.shape[1] // k) * k
            rb.append(blocks(r[:h, :w], k))
            gb.append(blocks(g[:h, :w], k))
            last = r[:h, :w]
        rb, gb = np.stack(rb), np.stack(gb)
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
        # Lag: per block whose input changes a lot over the capture, the delay (0..MAX_LAG frames) that best lines its
        # denoised series up with its input series (smoothed over +-1 frame), by least squares in log2(1 + x) so the
        # brightest moments don't decide alone. Weighted by the input's range; noise doesn't shift the best delay.
        MAX_LAG = 12
        c1 = np.cumsum(np.concatenate([np.zeros((1,) + gb.shape[1:]), gb]), axis=0)
        lo1, hi1 = np.clip(np.arange(n) - 1, 0, n), np.clip(np.arange(n) + 2, 0, n)
        smooth = np.log2(1.0 + (c1[hi1] - c1[lo1]) / (hi1 - lo1)[:, None, None] / m)
        out_l = np.log2(1.0 + rb / m)
        span = smooth.max(axis=0) - smooth.min(axis=0)
        moving = span > 1.0
        lags = np.zeros(0)
        if moving.any() and n > 2 * MAX_LAG:
            costs = np.stack([((out_l[MAX_LAG:] - smooth[MAX_LAG - d:n - d]) ** 2).mean(axis=0) for d in range(MAX_LAG + 1)])
            best = costs.argmin(axis=0)
            lags, lag_w = best[moving].astype(np.float64), span[moving]
        lag_mean = float((lags * lag_w).sum() / lag_w.sum()) if lags.size else 0.0
        lag_p90 = float(np.percentile(lags, 90)) if lags.size else 0.0
        print(f'{tag:<16} frames {n}  input mean {m:.4f}  denoised/input {rb.mean() / m:.4f} (displayed {shown:.4f})  excess {excess:.4f}  '
              f'deficit {deficit:.4f}  stuck energy {stuck_energy:.4f}  stuck blocks {100 * stuck_blocks:.2f}%  '
              f'age (frames since lit) mean {mean_age:.1f} p95 {p95_age:.0f} max {max_age:.0f}  '
              f'lag mean {lag_mean:.2f} p90 {lag_p90:.0f} frames ({lags.size} blocks)')
        # Overlay: last frame, grey x/(1+x), stuck blocks of the last frame tinted red by their output.
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
        print(f'{"":<16} {path}')


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
    """The post-DLSS noise metric (docs/stochastic_lighting.md "Noise after DLSS"). The truth before DLSS is the converged
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


def cmd_summary(md):
    for line in open(md, encoding='utf-8'):
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) >= 3 and cells[1] == 'GPU time' and cells[0] != 'Pass':
            print(f'{cells[0]}\t{cells[2]}')


SHORT = [
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
    elif cmd == 'trail':
        cmd_trail(argv[2], argv[3], argv[4], argv[5:])
    elif cmd == 'dropped':
        cmd_dropped(argv[2], int(argv[3]) if len(argv) > 3 else 0)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
