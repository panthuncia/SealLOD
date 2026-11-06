#ifndef CLOD_RAY_TRACING_COVERAGE_HLSLI
#define CLOD_RAY_TRACING_COVERAGE_HLSLI

#include "include/reyesPatchCommon.hlsli"

// Draws the ray tracing view can represent with rigid CLAS at their page positions. Everything else keeps
// rasterizing. Must depend only on the draw, so the two views agree: the RT view keeps exactly what raster drops when
// raytraced primary visibility is on.
// Part-library assemblies stay out; merge hierarchy roots (CLOD_MESH_FLAG_PART_PORTAL_ROOT) trace, each portal occurrence
// its own instance.
bool CLodMeshAssemblyTraceable(CLodMeshMetadata clodMeshMetadata)
{
    return clodMeshMetadata.assemblyInstanceCount == 0u || (clodMeshMetadata.meshFlags & CLOD_MESH_FLAG_PART_PORTAL_ROOT) != 0u;
}

bool CLodDrawIsRayTracingCovered(PerMeshBuffer perMesh, CLodMeshMetadata clodMeshMetadata)
{
    if ((perMesh.vertexFlags & VERTEX_SKINNED) != 0u || !CLodMeshAssemblyTraceable(clodMeshMetadata))
    {
        return false;
    }
    StructuredBuffer<MaterialInfo> rtMaterials =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const MaterialInfo rtMaterial = rtMaterials[perMesh.materialDataIndex];
    return !(ReyesGeometricDisplacementEnabled(rtMaterial)
        && max(0.0f, rtMaterial.geometricDisplacementMax - rtMaterial.geometricDisplacementMin) > 1e-5f);
}

// Path tracing has no raster to agree with: displaced materials trace their base mesh rather than disappear.
bool CLodDrawIsPathTraced(PerMeshBuffer perMesh, CLodMeshMetadata clodMeshMetadata)
{
    return (perMesh.vertexFlags & VERTEX_SKINNED) == 0u && CLodMeshAssemblyTraceable(clodMeshMetadata);
}

#endif // CLOD_RAY_TRACING_COVERAGE_HLSLI
