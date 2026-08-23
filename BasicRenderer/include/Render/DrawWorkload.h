#pragma once

#include <functional>
#include <unordered_set>
#include <vector>

#include "Materials/Material.h"
#include "Materials/TechniqueDescriptor.h"
#include "Mesh/Mesh.h"
#include "Render/TerrainRvtTelemetry.h"
#include "Render/ShaderVariantRequestService.h"
#include "../generated/BuiltinRenderPasses.h"

inline bool IsAlphaBlendTechnique(const TechniqueDescriptor& technique) {
    return (technique.compileFlags & MaterialCompileBlend) != 0;
}

inline bool IsAlphaTestTechnique(const TechniqueDescriptor& technique) {
    return (technique.compileFlags & MaterialCompileAlphaTest) != 0;
}

inline bool ShouldSkipSourcePassForCLodAlphaBlend(const RenderPhase& pass) {
    return pass == Engine::Primary::OITAccumulationPass
        || pass == Engine::Primary::GBufferPass;
}

inline bool ShouldAddSupplementalCLodShadowWorkload(const Mesh& mesh, const RenderPhase& pass) {
    return mesh.IsCLodMesh() && pass == Engine::Primary::ShadowMapsPass;
}

struct MaterialEvalVariantSet {
    MaterialCompileFlags regular = MaterialCompileNone;
    MaterialCompileFlags reyes = MaterialCompileNone;
    bool hasDistinctReyes = false;
};

inline MaterialEvalVariantSet ComposeMaterialEvalVariantSet(
    MaterialCompileFlags materialFlags,
    uint32_t vertexFlags,
    bool isClodMesh,
    const PerMaterialCB& materialData,
    bool terrainTelemetryDebug = false)
{
    MaterialCompileFlags regular = materialFlags;
    if ((vertexFlags & VertexFlags::VERTEX_SKINNED) != 0u) {
        regular |= MaterialCompileFlags::MaterialCompileClodSkinning;
    }
    if (terrainTelemetryDebug) {
        regular |= MaterialCompileFlags::MaterialCompileTerrainRvtTelemetry;
    }

    const bool terrain =
        (materialFlags & MaterialCompileFlags::MaterialCompileTerrain) != 0;
    const bool geometricDisplacement =
        (materialFlags & MaterialCompileFlags::MaterialCompileGeometricDisplacement) != 0;
    const bool heightFromBaseAlpha =
        (materialFlags & MaterialCompileFlags::MaterialCompileHeightFromBaseAlpha) != 0;
    const float displacementSpan =
        materialData.geometricDisplacementMax - materialData.geometricDisplacementMin;
    const bool canProduceReyesPatches =
        isClodMesh &&
        materialData.geometricDisplacementEnabled != 0u &&
        displacementSpan > 1.0e-5f &&
        (terrain || (geometricDisplacement && !heightFromBaseAlpha));

    MaterialEvalVariantSet result{
        .regular = GetMaterialEvaluationShaderKey(regular),
        .reyes = GetMaterialEvaluationShaderKey(regular),
        .hasDistinctReyes = canProduceReyesPatches,
    };
    if (result.hasDistinctReyes) {
        result.reyes |= MaterialCompileFlags::MaterialCompileClodReyesPatch;
    }
    return result;
}

inline MaterialEvalVariantSet ComposeMaterialEvalVariantSet(const Mesh& mesh, const Material& material)
{
    return ComposeMaterialEvalVariantSet(
        material.Technique().compileFlags,
        mesh.GetPerMeshCBData().vertexFlags,
        mesh.IsCLodMesh(),
        material.GetData(),
        IsTerrainRvtTelemetryDebugEnabled());
}

struct MaterialEvalVariantOptions {
    bool includeColorOnly = true;
    bool includeReyes = true;
    bool includeVoxel = false;
    bool includeSkinning = false;
};

// Keep offline shader warming in lockstep with the variants assembled by the
// live CLod workload path. The input is the material technique key before
// renderer-added evaluation dimensions are applied.
inline std::vector<MaterialCompileFlags> ExpandMaterialEvalVariants(
    MaterialCompileFlags materialFlags,
    const MaterialEvalVariantOptions& options = {}) {
    constexpr std::uint64_t dimensions =
        static_cast<std::uint64_t>(MaterialCompileVoxel) |
        static_cast<std::uint64_t>(MaterialCompileClodReyesPatch) |
        static_cast<std::uint64_t>(MaterialCompileClodSkinning) |
        static_cast<std::uint64_t>(MaterialCompileMaterialEvalColorOnly);
    const auto base = static_cast<std::uint64_t>(materialFlags) & ~dimensions;
    std::vector<MaterialCompileFlags> result;
    result.reserve(16);
    const unsigned colorCount = options.includeColorOnly ? 2u : 1u;
    const unsigned reyesCount = options.includeReyes ? 2u : 1u;
    const unsigned voxelCount = options.includeVoxel ? 2u : 1u;
    const unsigned skinCount = options.includeSkinning ? 2u : 1u;
    for (unsigned color = 0; color < colorCount; ++color) {
        for (unsigned reyes = 0; reyes < reyesCount; ++reyes) {
            for (unsigned voxel = 0; voxel < voxelCount; ++voxel) {
                for (unsigned skin = 0; skin < skinCount; ++skin) {
                    std::uint64_t flags = base;
                    if (color) flags |= MaterialCompileMaterialEvalColorOnly;
                    if (reyes) flags |= MaterialCompileClodReyesPatch;
                    if (voxel) flags |= MaterialCompileVoxel;
                    if (skin) flags |= MaterialCompileClodSkinning;
                    result.push_back(GetMaterialEvaluationShaderKey(static_cast<MaterialCompileFlags>(flags)));
                }
            }
        }
    }
    return result;
}

template<class F>
void ForEachMeshDrawWorkload(const Mesh& mesh, const Material& material, F&& callback) {
    const auto& technique = material.Technique();
    const bool isClodMesh = mesh.IsCLodMesh();
    const bool alphaBlend = IsAlphaBlendTechnique(technique);
    const bool alphaTest = IsAlphaTestTechnique(technique);
    const bool clodAlphaBlend = isClodMesh && alphaBlend && !alphaTest;
    const bool skinned =
        (mesh.GetPerMeshCBData().vertexFlags & VertexFlags::VERTEX_SKINNED) != 0u;

    for (const auto& pass : technique.passes) {
        if (clodAlphaBlend && ShouldSkipSourcePassForCLodAlphaBlend(pass)) {
            continue;
        }

        const bool clodOnly =
            isClodMesh
            && pass == Engine::Primary::GBufferPass
            && (!alphaBlend || alphaTest);
        callback(DrawWorkloadKey {
            technique.compileFlags,
            pass,
            clodOnly,
            false
        });

        // Keep the legacy shadow path alive while the CLod shadow variant is under construction.
        // CLod shadow work is mirrored into a dedicated clodOnly workload so the future CLod VSM
        // path can come online without changing mesh pass classification again.
        if (ShouldAddSupplementalCLodShadowWorkload(mesh, pass)) {
            callback(DrawWorkloadKey {
                technique.compileFlags,
                pass,
                true,
                skinned
            });
        }
    }

    if (clodAlphaBlend) {
        callback(DrawWorkloadKey {
            technique.compileFlags,
            Engine::Primary::CLodTransparentPass,
            true,
            false
        });
    }
}

template<class F>
void ForEachMeshDrawWorkload(const Mesh& mesh, F&& callback) {
    ForEachMeshDrawWorkload(mesh, *mesh.material, std::forward<F>(callback));
}

template<class F>
void ForEachMeshRenderPhase(const Mesh& mesh, const Material& material, F&& callback) {
    std::unordered_set<RenderPhase, RenderPhase::Hasher> uniquePhases;
    ForEachMeshDrawWorkload(mesh, material, [&](const DrawWorkloadKey& workloadKey) {
        if (uniquePhases.insert(workloadKey.renderPhase).second) {
            callback(workloadKey.renderPhase);
        }
    });
}

template<class F>
void ForEachMeshRenderPhase(const Mesh& mesh, F&& callback) {
    ForEachMeshRenderPhase(mesh, *mesh.material, std::forward<F>(callback));
}
