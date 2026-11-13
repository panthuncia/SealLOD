"""Write a self-contained .glb lens test scene for the BasicRenderer demo (load it from a `--script` scenario or Scene Explorer). See README.md here.

Everything is built from geometry and factor-only materials (no textures), so the scene needs no texture processing
and every radiance in it is exact. Charts are self-lit (emissive white, black base colour) and sit in front of a black
backdrop, so the demo's sky and directional light do not change what the camera sees.

Placement is relative to the camera: content at depth d and lateral offset (x, y) lands at camera + (x, y, -d), in
metres, Y up, right-handed, looking down -Z. The camera defaults to the demo's start pose, (-3, 5, 1) looking down -Z
with an 80 degree vertical FOV; `--write-camera` writes a matching logs/demo_camera_state.json.

Emissive values above 1 are written straight into emissiveFactor (`--emissive-encoding raw`, the default): the
importer reads emissiveFactor unclamped and ignores KHR_materials_emissive_strength. That is outside the glTF spec;
`--emissive-encoding khr` writes the spec form instead, which today's importer renders at the clamped factor only.
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
FLOAT = 5126
UINT32 = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
VARIANTS = ("chart", "bokeh", "flare", "furnace", "focus", "all")


# --- Geometry: each mesh is (positions Nx3, normals Nx3, indices Mx3), counter-clockwise seen from the front. ---

def quad(width, height):
    """A width x height rectangle in the XY plane, centred on the origin, facing +Z (toward a camera at +Z)."""
    w, h = width / 2.0, height / 2.0
    positions = np.array([[-w, -h, 0], [w, -h, 0], [w, h, 0], [-w, h, 0]], np.float32)
    normals = np.tile(np.array([0, 0, 1], np.float32), (4, 1))
    return positions, normals, np.array([[0, 1, 2], [0, 2, 3]], np.uint32)


def polygon(points):
    """A convex polygon from 2D points (counter-clockwise) in the XY plane, facing +Z."""
    points = np.asarray(points, np.float32)
    positions = np.zeros((len(points), 3), np.float32)
    positions[:, :2] = points
    normals = np.tile(np.array([0, 0, 1], np.float32), (len(points), 1))
    indices = np.array([[0, i, i + 1] for i in range(1, len(points) - 1)], np.uint32)
    return positions, normals, indices


def disc(radius, segments=48):
    angles = np.linspace(0.0, 2.0 * math.pi, segments, endpoint=False)
    return polygon(np.stack([np.cos(angles), np.sin(angles)], 1) * radius)


def uv_sphere(radius, segments=24, rings=16, inward=False):
    theta = np.linspace(0.0, math.pi, rings + 1)
    phi = np.linspace(0.0, 2.0 * math.pi, segments + 1)
    t, p = np.meshgrid(theta, phi, indexing="ij")
    normals = np.stack([np.sin(t) * np.cos(p), np.cos(t), -np.sin(t) * np.sin(p)], -1).reshape(-1, 3)
    indices = []
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b = a + segments + 1
            if r != 0:
                indices.append([a, b, a + 1])
            if r != rings - 1:
                indices.append([a + 1, b, b + 1])
    indices = np.array(indices, np.uint32)
    if inward:
        normals = -normals
        indices = indices[:, ::-1].copy()
    return (normals * radius).astype(np.float32), normals.astype(np.float32), indices


def transformed(mesh, offset=(0, 0, 0), angle=0.0):
    """Rotate about Z by `angle` radians, then translate."""
    positions, normals, indices = mesh
    c, s = math.cos(angle), math.sin(angle)
    rotation = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], np.float32)
    return positions @ rotation.T + np.asarray(offset, np.float32), normals @ rotation.T, indices


def merged(meshes):
    positions, normals, indices, base = [], [], [], 0
    for p, n, i in meshes:
        positions.append(p)
        normals.append(n)
        indices.append(i + base)
        base += len(p)
    return np.concatenate(positions), np.concatenate(normals), np.concatenate(indices)


# --- Scene description ---

class SceneBuilder:
    def __init__(self, args):
        self.args = args
        self.camera = np.array(args.camera_origin, np.float64)
        self.materials = []
        self.material_index = {}
        self.meshes = []  # (name, mesh, material index)
        self.nodes = []  # (name, mesh index, translation)
        self.half_tan_v = math.tan(math.radians(args.fov_deg) / 2.0)
        self.half_tan_h = self.half_tan_v * args.aspect

    def material(self, name, base=(0, 0, 0), emissive=0.0, emissive_color=(1, 1, 1), double_sided=False):
        if name in self.material_index:
            return self.material_index[name]
        node = {
            "name": name,
            "pbrMetallicRoughness": {"baseColorFactor": [*map(float, base), 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0},
        }
        radiance = [float(c) * emissive for c in emissive_color]
        if emissive > 0.0:
            peak = max(radiance)
            if self.args.emissive_encoding == "khr" and peak > 1.0:
                node["emissiveFactor"] = [c / peak for c in radiance]
                node["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": peak}}
            else:
                node["emissiveFactor"] = radiance
        if double_sided:
            node["doubleSided"] = True
        self.material_index[name] = len(self.materials)
        self.materials.append(node)
        return self.material_index[name]

    def mesh(self, name, mesh, material):
        self.meshes.append((name, mesh, material))
        return len(self.meshes) - 1

    def place(self, name, mesh_index, lateral=(0.0, 0.0), depth=0.0):
        """Place a mesh at camera + (x, y, -depth)."""
        translation = self.camera + np.array([lateral[0], lateral[1], -depth])
        self.nodes.append((name, mesh_index, translation))

    def frame_half_extent(self, depth):
        return depth * self.half_tan_h, depth * self.half_tan_v

    # --- Content ---

    def white(self):
        if self.args.chart_lighting == "emissive":
            return self.material("chart_white", base=(0, 0, 0), emissive=self.args.chart_radiance)
        return self.material("chart_white", base=(0.9, 0.9, 0.9))

    def black(self):
        g = self.args.black_albedo
        return self.material("chart_black", base=(g, g, g))

    def add_chart(self, depth=None, width=None, lateral=(0.0, 0.0), prefix="chart"):
        """ISO-12233-style chart: slanted-edge squares, checkerboards of four pitches and a Siemens star, on a white panel."""
        a = self.args
        d = a.focus_distance if depth is None else depth
        width = a.chart_width if width is None else width
        height = width * 0.75
        lift = 0.001  # black features sit 1 mm in front of the white panel, so nothing is coplanar
        slant = math.radians(a.slant_deg)
        black = []

        # Slanted-edge squares: a large one in the centre, four smaller toward the panel corners.
        black.append(transformed(quad(width * 0.16, width * 0.16), (0, 0, lift), slant))
        for sx in (-1, 1):
            for sy in (-1, 1):
                black.append(transformed(quad(width * 0.08, width * 0.08), (sx * width * 0.42, sy * height * 0.40, lift), slant))

        # Checkerboards, one per quadrant, each the same size with cell pitches halving: 8, 16, 32, 64 cells across.
        board = width * 0.24
        for (cx, cy), cells in zip(((-0.22, 0.17), (0.22, 0.17), (-0.22, -0.17), (0.22, -0.17)), (8, 16, 32, 64)):
            pitch = board / cells
            origin_x, origin_y = cx * width - board / 2.0, cy * height * 4.0 / 3.0 - board / 2.0
            for i in range(cells):
                for j in range(cells):
                    if (i + j) % 2 == 0:
                        black.append(transformed(quad(pitch, pitch), (origin_x + (i + 0.5) * pitch, origin_y + (j + 0.5) * pitch, lift)))

        # Siemens star of 36 spokes between the two upper checkerboards' row and the slanted square.
        star_radius, spokes = width * 0.07, 36
        star_centre = (0.0, height * 0.32)
        for k in range(0, spokes, 2):
            a0, a1 = 2.0 * math.pi * k / spokes, 2.0 * math.pi * (k + 1) / spokes
            black.append(polygon([star_centre,
                                  (star_centre[0] + star_radius * math.cos(a0), star_centre[1] + star_radius * math.sin(a0)),
                                  (star_centre[0] + star_radius * math.cos(a1), star_centre[1] + star_radius * math.sin(a1))]))
            black[-1][0][:, 2] = lift

        panel = self.mesh(prefix + "_panel", quad(width, height), self.white())
        features = self.mesh(prefix + "_features", merged(black), self.black())
        self.place(prefix + "_panel", panel, lateral, depth=d)
        self.place(prefix + "_features", features, lateral, depth=d)

    def add_focus_charts(self):
        """Small charts at several depths, each at its own place in the frame (--focus-depths, --focus-positions as NDC x, y
        pairs), each 1/4 of the frame height across: for autofocus, pointed at one chart at a time."""
        a = self.args
        positions = a.focus_positions
        for index, depth in enumerate(a.focus_depths):
            x, y = positions[2 * index], positions[2 * index + 1]
            half_w, half_h = self.frame_half_extent(depth)
            self.add_chart(depth, 0.25 * 2.0 * half_h, (x * half_w, y * half_h), f"focus_chart_{depth:g}m")

    def add_corner_patches(self):
        """Black/white slanted 2x2 checkers near the four frame corners at the focus distance (lateral CA)."""
        a = self.args
        d = a.focus_distance
        half_w, half_h = self.frame_half_extent(d)
        cell = max(0.05, 0.06 * half_h)
        slant = math.radians(a.slant_deg)
        white = self.white()
        black_material = self.black()
        for sx in (-1, 1):
            for sy in (-1, 1):
                name = f"corner_{'r' if sx > 0 else 'l'}{'t' if sy > 0 else 'b'}"
                centre = (sx * half_w * a.corner_fraction, sy * half_h * a.corner_fraction)
                tiles = [transformed(quad(cell, cell), (x * cell / 2.0, y * cell / 2.0, 0.001), 0.0) for x, y in ((-1, -1), (1, 1))]
                tiles = [transformed(t, (0, 0, 0), slant) for t in tiles]
                backing = transformed(quad(cell * 2.6, cell * 2.6), (0, 0, 0), slant)
                self.place(name + "_white", self.mesh(name + "_white", backing, white), centre, d)
                self.place(name + "_black", self.mesh(name + "_black", merged(tiles), black_material), centre, d)

    def add_bokeh(self):
        """Small emissive spheres on a grid at each depth, spanning the frame from centre to edges."""
        a = self.args
        sphere = self.mesh("bokeh_sphere", uv_sphere(a.sphere_radius), self.material("bokeh", emissive=a.emissive))
        columns, rows = a.bokeh_grid
        for layer, depth in enumerate(a.bokeh_depths):
            half_w, half_h = self.frame_half_extent(depth)
            # Each layer is shifted by a fraction of a cell so the layers do not hide each other in projection.
            shift = layer / max(1, len(a.bokeh_depths))
            for i in range(columns):
                for j in range(rows):
                    u = -1.0 + 2.0 * (i + 0.5 * shift) / (columns - 0.5) if columns > 1 else 0.0
                    v = -1.0 + 2.0 * (j + 0.5 * shift) / (rows - 0.5) if rows > 1 else 0.0
                    self.place(f"bokeh_d{depth:g}_{i}_{j}", sphere, (u * half_w * a.edge_fraction, v * half_h * a.edge_fraction), depth)

    def add_flare(self):
        a = self.args
        half_w, half_h = self.frame_half_extent(a.sun_distance)
        sun = self.mesh("sun", disc(a.sun_radius), self.material("sun", emissive=a.sun_radiance, emissive_color=(1.0, 0.95, 0.85)))
        self.place("sun", sun, (half_w * a.sun_position[0], half_h * a.sun_position[1]), a.sun_distance)
        # Ground: a large dim plane below the camera, extending well past the sun.
        extent = a.sun_distance * 2.0
        p, n, i = quad(extent, extent)
        ground_mesh = (np.stack([p[:, 0], np.zeros(4, np.float32), -p[:, 1]], 1).astype(np.float32),
                       np.tile(np.array([0, 1, 0], np.float32), (4, 1)), i)
        ground = self.mesh("ground", ground_mesh, self.material("ground", base=(0.1, 0.1, 0.1), emissive=a.ground_radiance))
        self.nodes.append(("ground", ground, self.camera + np.array([0.0, -a.camera_height, -extent / 2.0 + 1.0])))

    def add_furnace(self):
        a = self.args
        dome = self.mesh("furnace", uv_sphere(a.furnace_radius, 48, 32, inward=True),
                         self.material("furnace", base=(0, 0, 0), emissive=a.furnace_radiance, double_sided=True))
        self.place("furnace", dome)

    def add_backdrop(self, depth):
        half_w, half_h = self.frame_half_extent(depth)
        backdrop = self.mesh("backdrop", quad(half_w * 3.0, half_h * 3.0), self.material("backdrop", base=(self.args.black_albedo,) * 3))
        self.place("backdrop", backdrop, depth=depth)


def build_scene(args):
    builder = SceneBuilder(args)
    variant = args.variant
    if variant in ("chart", "all"):
        builder.add_chart()
        builder.add_corner_patches()
    if variant in ("bokeh", "all"):
        builder.add_bokeh()
    if variant == "flare":
        builder.add_flare()
    if variant == "furnace":
        builder.add_furnace()
    if variant == "focus":
        builder.add_focus_charts()
    if variant != "furnace" and not args.no_backdrop:
        depths = {"chart": [args.focus_distance], "bokeh": args.bokeh_depths, "flare": [args.sun_distance], "focus": args.focus_depths,
                  "all": [args.focus_distance, *args.bokeh_depths]}[variant]
        builder.add_backdrop(max(depths) * 1.5 + 1.0)
    return builder


# --- GLB writing ---

def write_glb(builder, args, path):
    blob = bytearray()
    buffer_views, accessors, meshes = [], [], []

    def add_view(data, target):
        while len(blob) % 4:
            blob.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        return len(buffer_views) - 1

    def add_accessor(array, kind, target):
        view = add_view(array.tobytes(), target)
        accessor = {"bufferView": view, "componentType": FLOAT if array.dtype == np.float32 else UINT32,
                    "count": int(array.shape[0]), "type": kind}
        if kind == "VEC3" and target == ARRAY_BUFFER:
            accessor["min"] = [float(v) for v in array.min(0)]
            accessor["max"] = [float(v) for v in array.max(0)]
        accessors.append(accessor)
        return len(accessors) - 1

    for name, (positions, normals, indices), material in builder.meshes:
        normals = normals / np.linalg.norm(normals, axis=1, keepdims=True)
        position = add_accessor(np.ascontiguousarray(positions, np.float32), "VEC3", ARRAY_BUFFER)
        normal = add_accessor(np.ascontiguousarray(normals, np.float32), "VEC3", ARRAY_BUFFER)
        index = add_accessor(np.ascontiguousarray(indices.reshape(-1), np.uint32), "SCALAR", ELEMENT_ARRAY_BUFFER)
        meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": position, "NORMAL": normal},
                                                     "indices": index, "material": material, "mode": 4}]})

    nodes = [{"name": name, "mesh": mesh, "translation": [float(v) for v in translation]}
             for name, mesh, translation in builder.nodes]
    gltf = {
        "asset": {"version": "2.0", "generator": "BasicRenderer scripts/lens/make_lens_test_scene.py",
                  "extras": {"variant": args.variant, "focusDistance": args.focus_distance,
                             "cameraOrigin": list(args.camera_origin), "cameraForward": [0, 0, -1],
                             "verticalFovDeg": args.fov_deg, "aspect": args.aspect, "units": "metres"}},
        "scene": 0,
        "scenes": [{"name": f"lens_{args.variant}", "nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": builder.materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(blob)}],
    }
    if any("extensions" in m for m in builder.materials):
        gltf["extensionsUsed"] = ["KHR_materials_emissive_strength"]

    while len(blob) % 4:
        blob.append(0)
    gltf["buffers"][0]["byteLength"] = len(blob)
    text = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    total = 12 + 8 + len(text) + 8 + len(blob)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as file:
        file.write(struct.pack("<III", GLB_MAGIC, 2, total))
        file.write(struct.pack("<II", len(text), CHUNK_JSON) + text)
        file.write(struct.pack("<II", len(blob), CHUNK_BIN) + bytes(blob))
    return gltf


def validate_glb(path):
    """Structural checks of a GLB: header, chunks, and every index the JSON holds. Returns a list of problems."""
    problems = []
    data = Path(path).read_bytes()
    if len(data) < 20:
        return ["file shorter than a GLB header"]
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2:
        problems.append(f"bad magic/version {magic:#x}/{version}")
    if length != len(data):
        problems.append(f"header length {length} != file size {len(data)}")
    json_length, json_type = struct.unpack_from("<II", data, 12)
    if json_type != CHUNK_JSON or json_length % 4:
        problems.append("first chunk is not a 4-aligned JSON chunk")
    gltf = json.loads(data[20:20 + json_length].decode("utf-8"))
    bin_at = 20 + json_length
    bin_length, bin_type = struct.unpack_from("<II", data, bin_at)
    if bin_type != CHUNK_BIN:
        problems.append("second chunk is not BIN")
    binary = data[bin_at + 8:bin_at + 8 + bin_length]
    if gltf.get("asset", {}).get("version") != "2.0":
        problems.append("asset.version is not 2.0")
    if gltf["buffers"][0]["byteLength"] > bin_length:
        problems.append("buffer byteLength exceeds BIN chunk")
    sizes = {FLOAT: 4, UINT32: 4}
    widths = {"SCALAR": 1, "VEC3": 3}
    views = gltf["bufferViews"]
    for i, view in enumerate(views):
        if view["byteOffset"] % 4 or view["byteOffset"] + view["byteLength"] > bin_length:
            problems.append(f"bufferView {i} misaligned or out of range")
    for i, accessor in enumerate(gltf["accessors"]):
        view = views[accessor["bufferView"]]
        need = accessor["count"] * sizes[accessor["componentType"]] * widths[accessor["type"]]
        if need > view["byteLength"]:
            problems.append(f"accessor {i} overruns its bufferView")
    for m, mesh in enumerate(gltf["meshes"]):
        for primitive in mesh["primitives"]:
            attributes = primitive["attributes"]
            position = gltf["accessors"][attributes["POSITION"]]
            if "min" not in position or "max" not in position:
                problems.append(f"mesh {m}: POSITION lacks min/max")
            if gltf["accessors"][attributes["NORMAL"]]["count"] != position["count"]:
                problems.append(f"mesh {m}: NORMAL count != POSITION count")
            index = gltf["accessors"][primitive["indices"]]
            view = views[index["bufferView"]]
            values = np.frombuffer(binary, "<u4", index["count"], view["byteOffset"])
            if index["count"] % 3 or (values.size and values.max() >= position["count"]):
                problems.append(f"mesh {m}: indices out of range or not triangles")
            pos_view = views[position["bufferView"]]
            positions = np.frombuffer(binary, "<f4", position["count"] * 3, pos_view["byteOffset"]).reshape(-1, 3)
            if not np.allclose(positions.min(0), position["min"]) or not np.allclose(positions.max(0), position["max"]):
                problems.append(f"mesh {m}: POSITION min/max disagree with data")
            if not 0 <= primitive["material"] < len(gltf["materials"]):
                problems.append(f"mesh {m}: material out of range")
    for n, node in enumerate(gltf["nodes"]):
        if not 0 <= node["mesh"] < len(gltf["meshes"]):
            problems.append(f"node {n}: mesh out of range")
    for material in gltf["materials"]:
        if "extensions" in material and "KHR_materials_emissive_strength" not in gltf.get("extensionsUsed", []):
            problems.append(f"material {material['name']}: extension not listed in extensionsUsed")
    return problems


def write_camera_state(args, path):
    """logs/demo_camera_state.json for the demo: its forward is rotation * (0, 0, -1), so identity looks down -Z."""
    state = {"rememberCameraPose": True, "position": [float(v) for v in args.camera_origin], "rotation": [0.0, 0.0, 0.0, 1.0]}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")


def floats(count):
    def parse(text):
        values = [float(v) for v in text.split(",")]
        if count and len(values) != count:
            raise argparse.ArgumentTypeError(f"expected {count} comma-separated numbers")
        return values
    return parse


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--variant", choices=VARIANTS, default="chart")
    parser.add_argument("--out", type=Path, help="output .glb (default models/lens_tests/<variant>.glb)")
    parser.add_argument("--focus-distance", type=float, default=3.0, help="chart and corner-patch depth, metres")
    parser.add_argument("--camera-origin", type=floats(3), default=[-3.0, 5.0, 1.0],
                        help="camera position the scene is placed in front of (default: the demo's start pose)")
    parser.add_argument("--fov-deg", type=float, default=80.0, help="vertical FOV used to place content at frame edges")
    parser.add_argument("--aspect", type=float, default=16.0 / 9.0)
    parser.add_argument("--chart-width", type=float, default=2.0)
    parser.add_argument("--slant-deg", type=float, default=5.0)
    parser.add_argument("--chart-lighting", choices=("emissive", "diffuse"), default="emissive")
    parser.add_argument("--chart-radiance", type=float, default=1.0, help="emissive radiance of chart white")
    parser.add_argument("--corner-fraction", type=float, default=0.85, help="corner patch position, fraction of frame half-extent")
    parser.add_argument("--sphere-radius", type=float, default=0.015)
    parser.add_argument("--bokeh-depths", type=floats(0), default=[1.5, 3.0, 12.0])
    parser.add_argument("--focus-depths", type=floats(0), default=[2.2, 0.8, 1.5, 4.0, 7.0], help="focus variant: chart depths, metres")
    parser.add_argument("--focus-positions", type=floats(0), default=[0.0, 0.0, -0.5, 0.5, 0.5, 0.5, -0.5, -0.5, 0.5, -0.5],
                        help="focus variant: each chart's centre as NDC x, y pairs (+y up)")
    parser.add_argument("--bokeh-grid", type=floats(2), default=[7, 5], help="columns,rows per depth")
    parser.add_argument("--edge-fraction", type=float, default=0.92, help="bokeh grid extent, fraction of frame half-extent")
    parser.add_argument("--emissive", type=float, default=20.0, help="bokeh sphere radiance")
    parser.add_argument("--sun-distance", type=float, default=50.0)
    parser.add_argument("--sun-radius", type=float, default=0.5)
    parser.add_argument("--sun-radiance", type=float, default=5000.0)
    parser.add_argument("--sun-position", type=floats(2), default=[0.8, 0.55], help="fraction of frame half-extent")
    parser.add_argument("--ground-radiance", type=float, default=0.02)
    parser.add_argument("--camera-height", type=float, default=1.6, help="flare ground plane distance below the camera")
    parser.add_argument("--furnace-radius", type=float, default=10.0)
    parser.add_argument("--furnace-radiance", type=float, default=1.0)
    parser.add_argument("--emissive-encoding", choices=("raw", "khr"), default="raw")
    parser.add_argument("--black-albedo", type=float, default=0.02,
                        help="base colour of black chart features and the backdrop (pure black ones do not render in the demo today)")
    parser.add_argument("--no-backdrop", action="store_true", help="omit the black backdrop (the demo sky shows through)")
    parser.add_argument("--write-camera", type=Path, help="also write a demo_camera_state.json for --camera-origin")
    parser.add_argument("--validate", action="store_true", help="re-parse the written GLB and check its structure")
    args = parser.parse_args()
    args.bokeh_grid = [int(v) for v in args.bokeh_grid]
    out = args.out or Path(__file__).resolve().parents[2] / "models" / "lens_tests" / f"{args.variant}.glb"

    builder = build_scene(args)
    gltf = write_glb(builder, args, out)
    triangles = sum(len(mesh[1][2]) for mesh in builder.meshes)
    print(f"{out}: {args.variant}, {len(gltf['nodes'])} nodes, {len(gltf['meshes'])} meshes, "
          f"{len(gltf['materials'])} materials, {triangles} unique triangles, {out.stat().st_size} bytes")
    if args.write_camera:
        write_camera_state(args, args.write_camera)
        print(f"{args.write_camera}: camera at {args.camera_origin} looking down -Z")
    if args.validate:
        problems = validate_glb(out)
        for problem in problems:
            print(f"  invalid: {problem}", file=sys.stderr)
        if problems:
            return 1
        print("  valid (structural checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
