#pragma once

#include <BasicRenderer/Pipeline/DrawWorkload.h>
#include <BasicRenderer/Scene/VertexFlags.h>

#include <cstdint>

struct MaterialEvalVariantSet {
	MaterialCompileFlags regular = MaterialCompileNone;
	MaterialCompileFlags reyes = MaterialCompileNone;
	bool hasDistinctReyes = false;
};

inline MaterialCompileFlags GetMaterialEvaluationShaderKey(MaterialCompileFlags flags)
{
	constexpr std::uint64_t shaderAffectingFlags =
		MaterialCompileFlags::MaterialCompileBlend |
		MaterialCompileFlags::MaterialCompileAlphaTest |
		MaterialCompileFlags::MaterialCompileDoubleSided |
		MaterialCompileFlags::MaterialCompileBaseColorTexture |
		MaterialCompileFlags::MaterialCompileNormalMap |
		MaterialCompileFlags::MaterialCompileMetallicTexture |
		MaterialCompileFlags::MaterialCompileRoughnessTexture |
		MaterialCompileFlags::MaterialCompileAOTexture |
		MaterialCompileFlags::MaterialCompileEmissiveTexture |
		MaterialCompileFlags::MaterialCompileParallax |
		MaterialCompileFlags::MaterialCompileGeometricDisplacement |
		MaterialCompileFlags::MaterialCompileOpacityTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRCoatColorTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRCoatWeightTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRCoatRoughnessTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRFuzzColorTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRFuzzWeightTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRFuzzRoughnessTexture |
		MaterialCompileFlags::MaterialCompileVoxel |
		MaterialCompileFlags::MaterialCompileHeightFromBaseAlpha |
		MaterialCompileFlags::MaterialCompileTerrain |
		MaterialCompileFlags::MaterialCompileClodReyesPatch |
		MaterialCompileFlags::MaterialCompileClodVertexColor |
		MaterialCompileFlags::MaterialCompileClodSkinning |
		MaterialCompileFlags::MaterialCompileTerrainRvtTelemetry |
		MaterialCompileFlags::MaterialCompileMaterialEvalColorOnly;
	return static_cast<MaterialCompileFlags>(static_cast<std::uint64_t>(flags) & shaderAffectingFlags);
}

inline MaterialEvalVariantSet ComposeMaterialEvalVariantSet(
	MaterialCompileFlags materialFlags,
	uint32_t vertexFlags,
	bool isClodMesh,
	bool geometricDisplacementEnabled,
	float geometricDisplacementMin,
	float geometricDisplacementMax,
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
	const float displacementSpan = geometricDisplacementMax - geometricDisplacementMin;
	const bool canProduceReyesPatches =
		isClodMesh &&
		geometricDisplacementEnabled &&
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
