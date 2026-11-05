#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <DirectXMath.h>

// Does a voxelization still stop the rays its triangles stop? Every occupied cell draws solid, so tracing cells gives what any
// view of them shows, and a build can ask this before it commits to a representation.
//
// The rays are short: each stays inside one block of 4x4x4 cells, so a surface elsewhere on the same line of sight cannot hide a
// hole here. A pair of block and direction is a "wall" when the triangles stop nearly every ray of it, which takes a continuous
// surface wider than the block. Aggregate geometry at the same scale, such as needles, stops some rays in every direction but
// does not form walls, and voxels are a good model of it. Pruning cells by mean coverage holds for aggregates and leaves gaps in
// walls, which the voxel width as an error metric cannot show.
namespace CLodCache {

struct VoxelSurfaceTrace {
	float meanSource = 0.0f;  // mean fraction of rays the triangles stop, over block and direction pairs
	float meanVoxel = 0.0f;  // the same for cells
	float deficit = 0.0f;  // the sum of max(0, source - cells), over the sum of source
	float wallShare = 0.0f;  // share of the rays the triangles stop that lie in walls
	float wallDeficit = 0.0f;  // the sum of max(0, source - cells) over walls, over the source in walls
	float wallMissed = 0.0f;  // fraction of walls where the cells stop under half of what the triangles stop

	// Rays the triangles stop in walls that the cells do not, over all rays the triangles stop.
	float SurfaceLoss() const { return wallShare * wallDeficit; }
};

// Each span of triangleLists holds three positions per triangle. Cells are coordinates on a grid of cellWidth starting at origin, and may
// repeat. With no cells the result is what any voxelization could lose at this width, which needs no voxelization at all.
VoxelSurfaceTrace TraceVoxelSurface(
	std::span<const std::span<const DirectX::XMFLOAT3>> triangleLists,
	std::span<const std::array<int32_t, 3>> cells,
	const DirectX::XMFLOAT3& origin,
	float cellWidth);

}
