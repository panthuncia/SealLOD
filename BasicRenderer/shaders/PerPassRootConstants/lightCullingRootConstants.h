#ifndef LIGHT_CULLING_ROOT_CONSTANTS_H
#define LIGHT_CULLING_ROOT_CONSTANTS_H

// The light grid's passes (lightCulling.hlsl, docs/lighting.md "The light grid").
#define LIGHT_GRID_INDEX_CAPACITY UintRootConstant0  // entries of Builtin::Light::GridIndices
#define LIGHT_GRID_RECORD_CAPACITY UintRootConstant1 // entries of Builtin::Light::CullingRecords
#define LIGHT_GRID_FLAGS UintRootConstant2

// Build each froxel's forward count (lights reaching it anywhere in front of the tile's farthest opaque pixel), for
// forward passes. Without it the forward count equals the opaque count.
#define LIGHT_GRID_FLAG_FORWARD_LISTS 1u

// Froxels a light culling thread group covers (one thread each): the group's lights are first tested against the union
// of these froxels' bounds, then each froxel tests only the survivors.
#define LIGHT_GRID_COARSE_X 4u
#define LIGHT_GRID_COARSE_Y 4u
#define LIGHT_GRID_COARSE_Z 4u
#define LIGHT_GRID_COARSE_THREADS (LIGHT_GRID_COARSE_X * LIGHT_GRID_COARSE_Y * LIGHT_GRID_COARSE_Z)
#define LIGHT_GRID_PREPARE_THREADS 64u
#define LIGHT_GRID_DEPTH_BOUNDS_TILE 16u // pixels square per depth bounds group; the grid tile size is a multiple of it

// Builtin::Light::CullingStats words, zeroed each frame by LightCullingPass and read back by LightManager::PollCullingStats.
#define LIGHT_CULLING_STATS_DROPPED_LIGHTS 0      // (froxel, light) pairs that found no room in GridIndices
#define LIGHT_CULLING_STATS_OVERFLOWED_CLUSTERS 1 // froxels that dropped at least one light
#define LIGHT_CULLING_STATS_STORED_LIGHTS 2       // (froxel, light) pairs stored
#define LIGHT_CULLING_STATS_NEEDED_LIGHTS 3       // entries GridIndices needed: the allocator, may exceed its capacity
#define LIGHT_CULLING_STATS_WORDS 4

#ifdef __cplusplus
#include <cstdint>
// The grid's shape: tile size in pixels (SARP_LIGHT_GRID_TILE) and depth slices (SARP_LIGHT_GRID_SLICES). Changing either
// rebuilds the graph, which sizes the grid's buffers (BuildLightClusteringPipeline).
inline constexpr const char* LightGridTileSizeSettingName = "lightGridTileSize";
inline constexpr const char* LightGridSlicesSettingName = "lightGridSlices";
inline constexpr uint32_t LightGridDefaultTileSize = 32u;
inline constexpr uint32_t LightGridDefaultSlices = 24u;
// Entries of Builtin::Light::GridIndices and Builtin::Light::CullingRecords the graph registers. Renderer::Update grows
// them (doubling, then a graph rebuild) when CullingStats says more indices were needed, or there are more active lights.
inline constexpr const char* LightGridIndexCapacitySettingName = "lightGridIndexCapacity";
inline constexpr const char* LightGridRecordCapacitySettingName = "lightGridRecordCapacity";
inline constexpr uint32_t LightGridMinIndexCapacity = 1u << 20;
inline constexpr uint32_t LightGridMinRecordCapacity = 1024u;

struct LightGridShape {
	uint32_t tilesX = 1, tilesY = 1, slices = 1, tileSize = 1;
	uint32_t Cells() const { return tilesX * tilesY * slices; }
	uint32_t Tiles() const { return tilesX * tilesY; }
};

inline LightGridShape MakeLightGridShape(uint32_t resolutionX, uint32_t resolutionY, uint32_t tileSize, uint32_t slices) {
	LightGridShape shape;
	// A multiple of the depth bounds group (LIGHT_GRID_DEPTH_BOUNDS_TILE), which must lie inside one tile.
	shape.tileSize = (tileSize < 16u ? 16u : tileSize + 15u) / 16u * 16u;
	shape.tilesX = (resolutionX + shape.tileSize - 1u) / shape.tileSize;
	shape.tilesY = (resolutionY + shape.tileSize - 1u) / shape.tileSize;
	shape.tilesX = shape.tilesX < 1u ? 1u : shape.tilesX;
	shape.tilesY = shape.tilesY < 1u ? 1u : shape.tilesY;
	shape.slices = slices < 8u ? 8u : (slices > 256u ? 256u : slices); // More than LIGHT_GRID_NEAR_SLICES.
	return shape;
}
#endif

#endif // LIGHT_CULLING_ROOT_CONSTANTS_H
