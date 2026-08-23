#pragma once

enum VertexFlags {
    VERTEX_COLORS = 1 << 0,
    VERTEX_NORMALS = 1 << 1,
	VERTEX_TEXCOORDS = 1 << 2,
	VERTEX_SKINNED = 1 << 3,
	VERTEX_TANGENTS = 1 << 4,
};
