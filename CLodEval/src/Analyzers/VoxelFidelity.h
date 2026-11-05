#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Core/CachedMesh.h"

// Compares what a voxel group draws against the triangles it stands for, with no renderer and no camera. Every occupied
// cell draws solid, so projecting cells and triangles along a few directions gives what any view of the group would show.
// The features describe the source surface at the group's cell size, for finding what predicts a bad result.
namespace clodeval {

struct VoxelGroupFidelity {
	uint32_t group = 0;
	int32_t depth = 0;
	int32_t voxelLevel = 0;  // steps above triangles
	float width = 0.0f;
	float enterError = 0.0f, leaveError = 0.0f;
	uint32_t cells = 0;
	uint32_t touchedCells = 0;  // cells a source triangle passes through
	uint64_t sourceTriangles = 0;

	// Measured, averaged over directions, as fractions of the source's projected area.
	float coverageRatio = 0.0f;  // cell area over source area
	float holes = 0.0f;  // source area no cell covers
	float excess = 0.0f;  // cell area with no source behind it
	float worstHoles = 0.0f;
	// Rays the cells miss that the triangles hit, counted region by region so excess elsewhere cannot hide them: over square
	// tiles of the projection, the sum of max(0, source - cells) over the sum of source. By tile width in cells.
	float deficit2 = 0.0f, deficit4 = 0.0f, deficit8 = 0.0f;
	// The same, over only the tiles the source almost fills, where a loss means an actual surface has gaps in it.
	float solidDeficit4 = 0.0f;
	float solidShare4 = 0.0f;  // share of the source's projected area that lies in such tiles

	// The same question asked locally: short rays through one block of 4x4x4 cells, so another part of the mesh along the
	// same line of sight cannot fill a hole in this one. Over (block, direction) pairs.
	float blockSource = 0.0f;  // mean fraction of rays the triangles stop in the block
	float blockVoxel = 0.0f;  // mean fraction the cells stop
	float blockDeficit = 0.0f;  // sum of max(0, source - cells) over sum of source
	float wallDeficit = 0.0f;  // the same over pairs where the triangles stop nearly every ray: a wall across the block
	float wallShare = 0.0f;  // share of all source hits that lie in such pairs
	float wallMissed = 0.0f;  // fraction of pairs with a wall in which the cells stop under half the rays

	// Features.
	float keptFraction = 0.0f;  // cells over touched cells
	float areaPerCell = 0.0f;  // source area in a touched cell, in units of width^2
	float cellCoherence = 0.0f;  // how much source normals in one cell agree: 1 flat, 1/3 isotropic
	float neighbourhoodCoherence = 0.0f;  // the same across the cells within two cells
	float neighbourhoodFill = 0.0f;  // touched cells within two cells, over the 25 a flat sheet would touch
};

std::vector<VoxelGroupFidelity> MeasureVoxelFidelity(const CachedMesh& mesh);

// One group's cells as the page holds them, per segment, with what each segment refines into.
struct VoxelSegmentCells {
	int32_t refinedGroup = -1;
	float width = 0.0f;
	std::vector<std::array<float, 3>> cellMins;
};
std::vector<VoxelSegmentCells> ReadVoxelSegmentCells(const CachedMesh& mesh, uint32_t group);

}
