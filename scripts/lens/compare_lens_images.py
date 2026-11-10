"""Compare two HDR captures (DDS, as DirectXTex SaveToDDSFile writes them) for the lens validation suite. See README.md here.

`a` is the image under test, `b` the reference. Metrics are on RGB (alpha is ignored), over pixels finite in both:
  mape         mean |a - b| / (|b| + eps), all channels; per-channel values alongside
  rmse         root mean square of a - b, in linear radiance
  tonemapped   mean |T(a) - T(b)| with T = sRGB encode of Reinhard x / (1 + x) after --exposure, in [0, 1] display
               units: a crude perceptual proxy, not FLIP
Exit status: 0 pass, 1 fail (threshold exceeded or non-finite pixels), 2 when the images cannot be read or compared.
"""
import argparse
import json
import struct
import sys
import tempfile
import zlib
from pathlib import Path

import numpy as np

DDS_MAGIC = b"DDS "
DDS_HEADER_BYTES = 4 + 124
DX10_HEADER_BYTES = 20
DDPF_FOURCC = 0x4
# DXGI format: (numpy element type, channels).
DXGI_FORMATS = {
    2: ("<f4", 4),  # R32G32B32A32_FLOAT
    6: ("<f4", 3),  # R32G32B32_FLOAT
    10: ("<f2", 4),  # R16G16B16A16_FLOAT
    16: ("<f4", 2),  # R32G32_FLOAT
    41: ("<f4", 1),  # R32_FLOAT
}
# Legacy D3DFORMAT FourCCs DirectXTex writes for float formats without DDS_FLAGS_FORCE_DX10_EXT.
LEGACY_FOURCC = {116: 2, 113: 10, 115: 16, 114: 41}  # A32B32G32R32F, A16B16G16R16F, G32R32F, R32F
DXGI_NAMES = {2: "R32G32B32A32_FLOAT", 6: "R32G32B32_FLOAT", 10: "R16G16B16A16_FLOAT", 16: "R32G32_FLOAT", 41: "R32_FLOAT"}


class ImageError(Exception):
    pass


def read_dds(path):
    """Top mip of a 2D DDS as float32 (height, width, channels), and its DXGI format."""
    with open(path, "rb") as file:
        header = file.read(DDS_HEADER_BYTES)
        if len(header) != DDS_HEADER_BYTES or header[:4] != DDS_MAGIC:
            raise ImageError(f"{path}: not a DDS file")
        words = struct.unpack_from("<31I", header, 4)
        height, width = words[2], words[3]
        pf_flags, fourcc = struct.unpack_from("<II", header, 80)
        if not pf_flags & DDPF_FOURCC:
            raise ImageError(f"{path}: uncompressed legacy pixel formats are not supported")
        if header[84:88] == b"DX10":
            dx10 = file.read(DX10_HEADER_BYTES)
            format_id, dimension, _misc, array_size = struct.unpack_from("<4I", dx10)
            if dimension != 3 or array_size not in (0, 1):
                raise ImageError(f"{path}: not a single 2D texture")
        elif fourcc in LEGACY_FOURCC:
            format_id = LEGACY_FOURCC[fourcc]
        else:
            raise ImageError(f"{path}: unsupported FourCC {header[84:88]!r}")
        if format_id not in DXGI_FORMATS:
            raise ImageError(f"{path}: unsupported DXGI format {format_id}")
        element, channels = DXGI_FORMATS[format_id]
        data = np.fromfile(file, element, width * height * channels)
    if data.size != width * height * channels:
        raise ImageError(f"{path}: expected {width}x{height}x{channels} elements, found {data.size}")
    return data.reshape(height, width, channels).astype(np.float32), format_id


def write_dds(path, image, legacy=False):
    """RGBA32F DDS, as DirectXTex writes it: legacy FourCC 116 header, or a DX10 one."""
    image = np.ascontiguousarray(image, np.float32)
    height, width, _ = image.shape
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000  # caps, height, width, pitch, pixel format
    fourcc = struct.pack("<I", 116) if legacy else b"DX10"
    header = struct.pack("<7I", 124, flags, height, width, width * 16, 0, 1) + b"\0" * 44
    header += struct.pack("<2I", 32, DDPF_FOURCC) + fourcc + b"\0" * 20
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    with open(path, "wb") as file:
        file.write(DDS_MAGIC + header)
        if not legacy:
            file.write(struct.pack("<5I", 2, 3, 0, 1, 0))
        file.write(image.tobytes())


def write_png(path, rgb8):
    """8-bit RGB PNG (height, width, 3)."""
    height, width, _ = rgb8.shape
    raw = b"".join(b"\0" + rgb8[row].tobytes() for row in range(height))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                   + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def srgb_encode(linear):
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, 12.92 * linear, 1.055 * np.power(linear, 1.0 / 2.4) - 0.055)


def tonemap(rgb, exposure):
    x = np.maximum(rgb * exposure, 0.0)
    return srgb_encode(x / (1.0 + x))


def turbo(t):
    """Google's Turbo colormap, polynomial approximation, t in [0, 1] -> RGB in [0, 1]."""
    t = np.clip(t, 0.0, 1.0)[..., None]
    r = np.array([0.13572138, 4.61539260, -42.66032258, 132.13108234, -152.94239396, 59.28637943])
    g = np.array([0.09140261, 2.19418839, 4.84296658, -14.18503333, 4.27729857, 2.82956604])
    b = np.array([0.10667330, 12.64194608, -60.58204836, 110.36276771, -89.90310912, 27.34824973])
    powers = np.concatenate([t ** k for k in range(6)], -1)
    return np.clip(np.stack([powers @ r, powers @ g, powers @ b], -1), 0.0, 1.0)


def compare(a, b, eps, exposure):
    if a.shape[:2] != b.shape[:2]:
        raise ImageError(f"size mismatch: {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}")
    channels = min(3, a.shape[2], b.shape[2])
    a, b = a[..., :channels].astype(np.float64), b[..., :channels].astype(np.float64)
    finite = np.isfinite(a).all(-1) & np.isfinite(b).all(-1)
    nonfinite_a = int((~np.isfinite(a).all(-1)).sum())
    nonfinite_b = int((~np.isfinite(b).all(-1)).sum())
    if not finite.any():
        raise ImageError("no pixel is finite in both images")
    av, bv = a[finite], b[finite]
    difference = av - bv
    relative = np.abs(difference) / (np.abs(bv) + eps)
    tone = np.abs(tonemap(av, exposure) - tonemap(bv, exposure))
    names = "RGB"[:channels]
    report = {
        "width": int(a.shape[1]),
        "height": int(a.shape[0]),
        "pixels_compared": int(finite.sum()),
        "nonfinite_a": nonfinite_a,
        "nonfinite_b": nonfinite_b,
        "eps": eps,
        "mape": float(relative.mean()),
        "mape_per_channel": {names[c]: float(relative[:, c].mean()) for c in range(channels)},
        "mean_relative_error_per_channel": {names[c]: float((difference[:, c].mean()) / (np.abs(bv[:, c]).mean() + eps))
                                            for c in range(channels)},
        "rmse": float(np.sqrt((difference ** 2).mean())),
        "tonemapped_mae": float(tone.mean()),
        "tonemapped_metric": f"mean |sRGB(Reinhard(x * {exposure:g}))| difference, display units in [0, 1]; not FLIP",
        "mean_a": [float(v) for v in av.mean(0)],
        "mean_b": [float(v) for v in bv.mean(0)],
    }
    per_pixel = np.zeros(a.shape[:2])
    per_pixel[finite] = relative.mean(-1)
    return report, per_pixel, finite


def write_difference(path, per_pixel, finite, scale):
    rgb = turbo(per_pixel / scale)
    rgb[~finite] = (1.0, 0.0, 1.0)  # magenta marks pixels non-finite in either image
    write_png(path, (rgb * 255.0 + 0.5).astype(np.uint8))


def run(path_a, path_b, args):
    a, format_a = read_dds(path_a)
    b, format_b = read_dds(path_b)
    report, per_pixel, finite = compare(a, b, args.eps, args.exposure)
    report["a"] = {"path": str(path_a), "format": DXGI_NAMES[format_a]}
    report["b"] = {"path": str(path_b), "format": DXGI_NAMES[format_b]}
    report["threshold_mape"] = args.threshold_mape
    failures = []
    if report["nonfinite_a"] or report["nonfinite_b"]:
        failures.append(f"non-finite pixels: {report['nonfinite_a']} in a, {report['nonfinite_b']} in b")
    if args.threshold_mape is not None and report["mape"] > args.threshold_mape:
        failures.append(f"MAPE {report['mape']:.5f} > {args.threshold_mape}")
    report["passed"] = not failures
    report["failures"] = failures
    if args.diff:
        write_difference(args.diff, per_pixel, finite, args.diff_scale)
        report["diff"] = str(args.diff)
    return report


def print_report(report):
    print(f"{report['a']['path']} vs {report['b']['path']} ({report['width']}x{report['height']}, "
          f"{report['a']['format']} / {report['b']['format']})")
    per = "  ".join(f"{k} {v:.5f}" for k, v in report["mape_per_channel"].items())
    bias = "  ".join(f"{k} {v:+.5f}" for k, v in report["mean_relative_error_per_channel"].items())
    print(f"  MAPE            {report['mape']:.5f}   ({per})")
    print(f"  mean rel. bias  {bias}")
    print(f"  RMSE            {report['rmse']:.6g}")
    print(f"  tonemapped MAE  {report['tonemapped_mae']:.5f}   (Reinhard + sRGB proxy, not FLIP)")
    if report["nonfinite_a"] or report["nonfinite_b"]:
        print(f"  non-finite      {report['nonfinite_a']} in a, {report['nonfinite_b']} in b")
    if "diff" in report:
        print(f"  difference      {report['diff']}")
    print("  PASS" if report["passed"] else "  FAIL: " + "; ".join(report["failures"]))


def self_test():
    rng = np.random.default_rng(1)
    height, width = 24, 32
    reference = np.ones((height, width, 4), np.float32)
    reference[..., :3] = rng.uniform(0.1, 10.0, (height, width, 3))
    under_test = reference.copy()
    under_test[..., :3] *= 1.0 + rng.choice([-0.02, 0.02], (height, width, 3))
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        write_dds(directory / "a.dds", under_test)
        write_dds(directory / "b.dds", reference, legacy=True)
        read_back, format_id = read_dds(directory / "a.dds")
        assert format_id == 2 and np.array_equal(read_back, under_test), "DX10 round trip"
        read_back, format_id = read_dds(directory / "b.dds")
        assert format_id == 2 and np.array_equal(read_back, reference), "legacy FourCC round trip"

        args = argparse.Namespace(eps=1e-4, exposure=1.0, threshold_mape=0.03, diff=directory / "d.png", diff_scale=0.1)
        report = run(directory / "a.dds", directory / "b.dds", args)
        assert abs(report["mape"] - 0.02) < 1e-3, report["mape"]
        assert all(abs(v - 0.02) < 1e-3 for v in report["mape_per_channel"].values())
        assert report["passed"] and report["tonemapped_mae"] > 0.0
        png = (directory / "d.png").read_bytes()
        assert png[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", png[16:24]) == (width, height), "PNG header"

        args.threshold_mape = 0.01
        assert not run(directory / "a.dds", directory / "b.dds", args)["passed"], "threshold must fail"

        identical = run(directory / "b.dds", directory / "b.dds", argparse.Namespace(**{**vars(args), "diff": None}))
        assert identical["mape"] == 0.0 and identical["rmse"] == 0.0 and identical["tonemapped_mae"] == 0.0

        broken = under_test.copy()
        broken[0, 0, 0] = np.nan
        write_dds(directory / "nan.dds", broken)
        report = run(directory / "nan.dds", directory / "b.dds", argparse.Namespace(**{**vars(args), "threshold_mape": None, "diff": None}))
        assert report["nonfinite_a"] == 1 and not report["passed"], "non-finite pixels must fail"

        write_dds(directory / "small.dds", reference[:8])
        try:
            run(directory / "small.dds", directory / "b.dds", args)
            raise AssertionError("size mismatch must raise")
        except ImageError:
            pass
    print("self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("a", nargs="?", type=Path, help="image under test")
    parser.add_argument("b", nargs="?", type=Path, help="reference image")
    parser.add_argument("--threshold-mape", type=float, help="fail when MAPE exceeds this")
    parser.add_argument("--eps", type=float, default=0.01, help="MAPE denominator offset, linear radiance")
    parser.add_argument("--exposure", type=float, default=1.0, help="scale applied before the tonemapped metric")
    parser.add_argument("--diff", type=Path, help="write a false-colour per-pixel relative error PNG (Turbo)")
    parser.add_argument("--diff-scale", type=float, default=0.2, help="relative error mapped to the top of the colormap")
    parser.add_argument("--json", type=Path, help="write the report as JSON")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.a or not args.b:
        parser.error("two images are required")
    try:
        report = run(args.a, args.b, args)
    except (ImageError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print_report(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
