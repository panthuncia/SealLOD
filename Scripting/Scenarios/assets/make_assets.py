"""Writes the small models the example scenarios use: cube.glb, plane.glb and sphere.glb, beside this script.

    python make_assets.py

The outputs are committed (a few kilobytes), so a scenario run needs no generation step. Standard library only. Each
model is one mesh at the origin, positions and normals only, with a plain base-colour material: metres, +Y up.
"""

import json
import math
import struct
from pathlib import Path

GLB_MAGIC, CHUNK_JSON, CHUNK_BIN = 0x46546C67, 0x4E4F534A, 0x004E4942
FLOAT, UINT32 = 5126, 5125
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


def write_glb(path, name, mesh, base_color, roughness=0.6, metallic=0.0):
    positions, normals, indices = mesh
    blob = bytearray()
    views, accessors = [], []

    def add(values, fmt, kind, target, count):
        while len(blob) % 4:
            blob.append(0)
        data = struct.pack("<" + fmt * len(values), *values)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        accessor = {"bufferView": len(views) - 1, "componentType": FLOAT if fmt == "f" else UINT32, "count": count, "type": kind}
        accessors.append(accessor)
        return accessor

    flat_positions = [c for p in positions for c in p]
    position = add(flat_positions, "f", "VEC3", ARRAY_BUFFER, len(positions))
    position["min"] = [min(p[i] for p in positions) for i in range(3)]
    position["max"] = [max(p[i] for p in positions) for i in range(3)]
    add([c for n in normals for c in n], "f", "VEC3", ARRAY_BUFFER, len(normals))
    add(indices, "I", "SCALAR", ELEMENT_ARRAY_BUFFER, len(indices))
    while len(blob) % 4:
        blob.append(0)

    gltf = {
        "asset": {"version": "2.0", "generator": "BasicRenderer Scripting/Scenarios/assets/make_assets.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0, "mode": 4}]}],
        "materials": [{"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*base_color, 1.0], "metallicFactor": metallic, "roughnessFactor": roughness}}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
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
    for name in ("cube.glb", "plane.glb", "sphere.glb"):
        print(here / name, (here / name).stat().st_size, "bytes")


if __name__ == "__main__":
    main()
