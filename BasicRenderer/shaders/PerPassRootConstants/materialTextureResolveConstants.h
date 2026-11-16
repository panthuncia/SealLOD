#ifndef MATERIAL_TEXTURE_RESOLVE_CONSTANTS_H
#define MATERIAL_TEXTURE_RESOLVE_CONSTANTS_H

// Builtin::Material::ResolvedTextureDescriptors, written by ResolveMaterialTextureDescriptorsPass and read by material
// evaluation: for each row of the material evaluation table, the descriptor each texture slot samples this frame, as
// ResolveMaterialTextureDescriptor resolves it. Two uint4 a row. The table covers the material tables' capacity (the
// materialResolvedTextureCapacity setting, at least the minimum here): the renderer rebuilds the graph when the material
// tables grow past it, so material evaluation reads it without a range check. A per-texture fallback branch made the
// pass 3% slower than no table; the table alone is 7% faster (Zorah, moving camera, 2026-11-14).
#define MATERIAL_RESOLVED_TEXTURE_MIN_CAPACITY 65536u
#define MATERIAL_RESOLVED_TEXTURE_UINT4_PER_ROW 2u
#define MATERIAL_RESOLVED_TEXTURE_BASE_COLOR 0u
#define MATERIAL_RESOLVED_TEXTURE_NORMAL 1u
#define MATERIAL_RESOLVED_TEXTURE_METALLIC 2u
#define MATERIAL_RESOLVED_TEXTURE_ROUGHNESS 3u
#define MATERIAL_RESOLVED_TEXTURE_OPACITY 4u
#define MATERIAL_RESOLVED_TEXTURE_AO 5u
#define MATERIAL_RESOLVED_TEXTURE_EMISSIVE 6u
#define MATERIAL_RESOLVED_TEXTURE_HEIGHT 7u
#define MATERIAL_RESOLVED_TEXTURE_RESOLVE_GROUP_SIZE 64u

#ifdef __cplusplus
// Rows the table is registered with (RegisterVisUtilResources). Renderer::Update grows it past the material tables'
// capacity (MaterialManager::GetMaterialBufferCapacity) and rebuilds the graph.
inline constexpr const char* MaterialResolvedTextureCapacitySettingName = "materialResolvedTextureCapacity";
#endif

#endif // MATERIAL_TEXTURE_RESOLVE_CONSTANTS_H
