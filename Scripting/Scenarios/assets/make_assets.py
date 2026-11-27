"""Writes the small models the example scenarios use: cube.glb, plane.glb, sphere.glb and skinned_bar.glb, beside
this script.

    python make_assets.py

The outputs are committed (a few kilobytes), so a scenario run needs no generation step. Standard library only. Each
model is one mesh at the origin, positions and normals only, with a plain base-colour material: metres, +Y up.
skinned_bar.glb is the one skinned model: a 1 m bar with two joints, "base" and "tip", and a looping clip that swings
the tip.
"""

import json
import math
import struct
from pathlib import Path

GLB_MAGIC, CHUNK_JSON, CHUNK_BIN = 0x46546C67, 0x4E4F534A, 0x004E4942
FLOAT, UINT32, UINT16 = 5126, 5125, 5123
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963


def cube(half=0.5):
    positions, normals, indices = [], [], []
    for axis in range(3):
        for sign in (-1.0, 1.0):
            n = [0.0, 0.0, 0.0]
            n[axis] = sign
            u = [0.0, 0.0, 0.0]
            v = [0.0, 0.0, 0.0]
            u[(axis + 1) % 3] = 1.0
            v[(axis + 2) % 3] = 1.0
            if sign < 0:
                u, v = v, u  # keep counter-clockwise winding seen from outside
            base = len(positions)
            for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                positions.append([half * (n[i] + a * u[i] + b * v[i]) for i in range(3)])
                normals.append(n)
            indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, indices


def plane(half=10.0):
    positions = [[-half, 0.0, half], [half, 0.0, half], [half, 0.0, -half], [-half, 0.0, -half]]
    return positions, [[0.0, 1.0, 0.0]] * 4, [0, 1, 2, 0, 2, 3]


def sphere(radius=0.5, segments=32, rings=16):
    positions, normals, indices = [], [], []
    for ring in range(rings + 1):
        theta = math.pi * ring / rings
        for segment in range(segments + 1):
            phi = 2.0 * math.pi * segment / segments
            n = [math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi)]
            normals.append(n)
            positions.append([radius * c for c in n])
    for ring in range(rings):
        for segment in range(segments):
            a = ring * (segments + 1) + segment
            b = a + segments + 1
            indices += [a, a + 1, b, a + 1, b + 1, b]
    return positions, normals, indices


def skinned_bar(radius=0.08, height=1.0, sides=12, rings=16):
    """An open tube along +Y. Joint 0 ("base") holds the lower part and joint 1 ("tip", at half height) the upper part,
    blended across the middle half. Returns the mesh and its skin: per-vertex joints and weights, the joints, and a
    4 s clip that swings the tip 40 degrees either way about Z."""
    positions, normals, indices, joints, weights = [], [], [], [], []
    for ring in range(rings + 1):
        y = height * ring / rings
        tip = min(1.0, max(0.0, (y / height - 0.25) / 0.5))
        for side in range(sides + 1):
            phi = 2.0 * math.pi * side / sides
            n = [math.cos(phi), 0.0, math.sin(phi)]
            normals.append(n)
            positions.append([radius * n[0], y, radius * n[2]])
            joints.append([0, 1, 0, 0])
            weights.append([1.0 - tip, tip, 0.0, 0.0])
    for ring in range(rings):
        for side in range(sides):
            a = ring * (sides + 1) + side
            b = a + sides + 1
            indices += [a, b, a + 1, a + 1, b, b + 1]
    half = 0.5 * height
    swing = math.radians(40.0)
    skin = {
        "joints": joints,
        "weights": weights,
        # name, parent (an index into this list, or None), translation from the parent
        "nodes": [("base", None, [0.0, 0.0, 0.0]), ("tip", 0, [0.0, half, 0.0])],
        # column-major inverse bind matrices: each joint's rest translation undone
        "inverse_bind": [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -half, 0, 1]],
        # joint, key times, rotations (x, y, z, w)
        "clip": (1, [0.0, 1.0, 2.0, 3.0, 4.0],
                 [[0.0, 0.0, math.sin(0.5 * a), math.cos(0.5 * a)] for a in (0.0, swing, 0.0, -swing, 0.0)]),
    }
    return (positions, normals, indices), skin


def write_glb(path, name, mesh, base_color, roughness=0.6, metallic=0.0, skin=None):
    positions, normals, indices = mesh
    blob = bytearray()
    views, accessors = [], []

    def add(values, fmt, kind, target, count):
        while len(blob) % 4:
            blob.append(0)
        data = struct.pack("<" + fmt * len(values), *values)
        view = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        views.append(view)
        blob.extend(data)
        component = {"f": FLOAT, "I": UINT32, "H": UINT16}[fmt]
        accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
        accessors.append(accessor)
        return accessor

    flat_positions = [c for p in positions for c in p]
    position = add(flat_positions, "f", "VEC3", ARRAY_BUFFER, len(positions))
    position["min"] = [min(p[i] for p in positions) for i in range(3)]
    position["max"] = [max(p[i] for p in positions) for i in range(3)]
    add([c for n in normals for c in n], "f", "VEC3", ARRAY_BUFFER, len(normals))
    add(indices, "I", "SCALAR", ELEMENT_ARRAY_BUFFER, len(indices))

    attributes = {"POSITION": 0, "NORMAL": 1}
    nodes = [{"name": name, "mesh": 0}]
    scene_nodes = [0]
    extra = {}
    if skin:
        attributes["JOINTS_0"] = len(accessors)
        add([j for joint in skin["joints"] for j in joint], "H", "VEC4", ARRAY_BUFFER, len(positions))
        attributes["WEIGHTS_0"] = len(accessors)
        add([w for weight in skin["weights"] for w in weight], "f", "VEC4", ARRAY_BUFFER, len(positions))
        inverse_bind = len(accessors)
        add([m for matrix in skin["inverse_bind"] for m in matrix], "f", "MAT4", None, len(skin["inverse_bind"]))
        first_joint = len(nodes)
        for joint_name, parent, translation in skin["nodes"]:
            nodes.append({"name": joint_name, "translation": translation})
            if parent is None:
                scene_nodes.append(len(nodes) - 1)
            else:
                nodes[first_joint + parent].setdefault("children", []).append(len(nodes) - 1)
        joint_nodes = list(range(first_joint, len(nodes)))
        nodes[0]["skin"] = 0
        extra["skins"] = [{"name": name, "joints": joint_nodes, "inverseBindMatrices": inverse_bind, "skeleton": first_joint}]
        joint, times, rotations = skin["clip"]
        time_accessor = len(accessors)
        keys = add(times, "f", "SCALAR", None, len(times))
        keys["min"], keys["max"] = [min(times)], [max(times)]
        rotation_accessor = len(accessors)
        add([c for rotation in rotations for c in rotation], "f", "VEC4", None, len(rotations))
        extra["animations"] = [{
            "name": "swing",
            "samplers": [{"input": time_accessor, "output": rotation_accessor, "interpolation": "LINEAR"}],
            "channels": [{"sampler": 0, "target": {"node": joint_nodes[joint], "path": "rotation"}}],
        }]
    while len(blob) % 4:
        blob.append(0)

    gltf = {
        "asset": {"version": "2.0", "generator": "BasicRenderer Scripting/Scenarios/assets/make_assets.py"},
        "scene": 0,
        "scenes": [{"nodes": scene_nodes}],
        "nodes": nodes,
        "meshes": [{"name": name, "primitives": [{"attributes": attributes, "indices": 2, "material": 0, "mode": 4}]}],
        "materials": [{"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*base_color, 1.0], "metallicFactor": metallic, "roughnessFactor": roughness}}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
        **extra,
    }
    text = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as file:
        file.write(struct.pack("<III", GLB_MAGIC, 2, 12 + 8 + len(text) + 8 + len(blob)))
        file.write(struct.pack("<II", len(text), CHUNK_JSON) + text)
        file.write(struct.pack("<II", len(blob), CHUNK_BIN) + bytes(blob))


def main():
    here = Path(__file__).resolve().parent
    write_glb(here / "cube.glb", "cube", cube(), (0.8, 0.3, 0.2))
    write_glb(here / "plane.glb", "plane", plane(), (0.55, 0.55, 0.55), roughness=0.9)
    write_glb(here / "sphere.glb", "sphere", sphere(), (0.85, 0.85, 0.9), roughness=0.25, metallic=1.0)
    bar, bar_skin = skinned_bar()
    write_glb(here / "skinned_bar.glb", "skinned_bar", bar, (0.3, 0.6, 0.35), skin=bar_skin)
    for name in ("cube.glb", "plane.glb", "sphere.glb", "skinned_bar.glb"):
        print(here / name, (here / name).stat().st_size, "bytes")


if __name__ == "__main__":
    main()
