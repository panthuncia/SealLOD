#pragma once

// Direct static preparation of ready assets: which parts of an asset import as static
// groups, the shader variants their templates need, and the prepared recipe rows of a
// group.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include <BasicRenderer/Assets/Material.h>
#include <BasicRenderer/Assets/RenderableAsset.h>
#include <BasicRenderer/Diagnostics/TerrainRvtTelemetry.h>
#include <BasicRenderer/Pipeline/CLodOptions.h>
#include <BasicRenderer/Pipeline/MaterialEvalVariants.h>
#include <BasicRenderer/Pipeline/MeshDrawWorkload.h>
#include <BasicRenderer/Pipeline/ShaderVariantRequestService.h>
#include <BasicRenderer/Scene/VertexFlags.h>
#include <BasicRenderer/StaticImport/StaticSceneRecipe.h>
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <BasicRenderer/Streaming/StaticObjectRequestService.h>
#include <BasicRenderer/Assets/Geometry/Mesh.h>
#include <BasicRenderer/Scene/Animation/Skeleton.h>

namespace br::staticimport
{
struct ClassifiedDirectStaticPart
{
	std::size_t partIndex{ 0 };
	std::string name;
	std::uint32_t skinnedShapeIndex{ 0 };
	br::render::StaticGroupBuildInfo buildInfo;
	std::vector<std::uint32_t> meshOrdinals;
};

struct DirectStaticShaderVariantOptions
{
	CLodSoftwareRasterMode softwareRasterMode{ CLodSoftwareRasterMode::Compute };
	CLodVSMRasterMode vsmRasterMode{ CLodVSMRasterMode::Standard };
	CLodTransparencyMode transparencyMode{ CLodTransparencyMode::Disabled };
	bool reyesEnabled{ true };
};

inline MaterialRasterFlags ComposeDirectStaticRasterFlags(Mesh& mesh, const Material& material)
{
	MaterialRasterFlags flags = material.Technique().rasterFlags;
	if ((mesh.GetPerMeshCBData().vertexFlags & VERTEX_SKINNED) != 0u) flags |= MaterialRasterFlagsSkinned;
	return flags;
}

inline void AddUniqueDirectStaticShaderRequest(
	std::vector<ShaderVariantRequest>& requests,
	ShaderVariantRequest request)
{
	request = NormalizeShaderVariantRequest(request);
	if (std::ranges::find(requests, request) == requests.end()) requests.push_back(request);
}

inline std::vector<ShaderVariantRequest> CollectDirectStaticShaderVariantRequests(
	std::span<const br::render::StaticGroupBuildInfo> groups,
	const DirectStaticShaderVariantOptions& options)
{
	std::vector<ShaderVariantRequest> requests;
	for (const auto& group : groups) {
		for (const auto& meshTemplate : group.meshTemplates) {
			if (!meshTemplate.mesh || !meshTemplate.material) continue;
			auto& mesh = *meshTemplate.mesh;
			const auto& material = *meshTemplate.material;
			const auto& materialData = material.GetData();
			const auto eval = ComposeMaterialEvalVariantSet(
				material.Technique().compileFlags,
				mesh.GetPerMeshCBData().vertexFlags,
				mesh.IsCLodMesh(),
				materialData.geometricDisplacementEnabled != 0u,
				materialData.geometricDisplacementMin,
				materialData.geometricDisplacementMax,
				IsTerrainRvtTelemetryDebugEnabled());
			const auto addMaterialEvaluationVariants = [&](MaterialCompileFlags flags) {
				AddUniqueDirectStaticShaderRequest(requests, {
					.kind = ShaderVariantKind::MaterialEvaluation,
					.materialCompileFlags = flags });
				// EvaluateMaterialGroupsPass selects a color-only specialization for
				// visibility-only cohorts. This is a renderer-added dimension and is
				// therefore absent from the source material technique key. Prewarm the
				// paired specialization alongside every scene-derived evaluation key.
				AddUniqueDirectStaticShaderRequest(requests, {
					.kind = ShaderVariantKind::MaterialEvaluation,
					.materialCompileFlags = static_cast<MaterialCompileFlags>(
						static_cast<std::uint64_t>(flags) |
						static_cast<std::uint64_t>(MaterialCompileMaterialEvalColorOnly)) });
			};
			addMaterialEvaluationVariants(eval.regular);
			if (!mesh.IsCLodMesh()) continue;
			if (eval.hasDistinctReyes) {
				addMaterialEvaluationVariants(eval.reyes);
			}
			const auto rasterFlags = ComposeDirectStaticRasterFlags(mesh, material);
			AddUniqueDirectStaticShaderRequest(requests, {
				.kind = ShaderVariantKind::ClusterLODRaster,
				.materialRasterFlags = rasterFlags,
				.singleView = true });
			if (CLodSoftwareRasterUsesCompute(options.softwareRasterMode)) {
				AddUniqueDirectStaticShaderRequest(requests, {
					.kind = ShaderVariantKind::ClusterLODSoftwareRaster,
					.materialRasterFlags = rasterFlags,
					.rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer });
			}
			if (!CLodVSMRasterModeUsesLegacyRasterOnly(options.vsmRasterMode)) {
				AddUniqueDirectStaticShaderRequest(requests, {
					.kind = ShaderVariantKind::ClusterLODVirtualShadowRaster,
					.materialRasterFlags = rasterFlags });
				if (CLodSoftwareRasterUsesCompute(options.softwareRasterMode)) {
					AddUniqueDirectStaticShaderRequest(requests, {
						.kind = ShaderVariantKind::ClusterLODSoftwareRaster,
						.materialRasterFlags = rasterFlags,
						.rasterOutputKind = CLodRasterOutputKind::VirtualShadow });
				}
			}
			if (options.transparencyMode == CLodTransparencyMode::LinkedListDeepVisibility) {
				AddUniqueDirectStaticShaderRequest(requests, {
					.kind = ShaderVariantKind::ClusterLODDeepVisibilityRaster,
					.materialRasterFlags = rasterFlags });
			} else if (options.transparencyMode == CLodTransparencyMode::AVBOIT) {
				for (const auto kind : { ShaderVariantKind::ClusterLODAVBOITOccupancy,
						 ShaderVariantKind::ClusterLODAVBOITRaster,
						 ShaderVariantKind::ClusterLODAVBOITShade }) {
					AddUniqueDirectStaticShaderRequest(requests, {
						.kind = kind,
						.materialRasterFlags = rasterFlags });
				}
			}
		}
	}
	return requests;
}

inline std::uint64_t MakeDirectStaticPartStableID(std::uint64_t objectID, std::size_t partIndex)
{
	std::uint64_t hash = 14695981039346656037ull;
	auto mix = [&hash](std::uint64_t value) {
		for (std::uint32_t byteIndex = 0; byteIndex < 8; ++byteIndex) {
			hash ^= static_cast<unsigned char>((value >> (byteIndex * 8u)) & 0xFFu);
			hash *= 1099511628211ull;
		}
	};
	mix(0x5341525052454E44ull);
	mix(objectID);
	mix(static_cast<std::uint64_t>(partIndex));
	return hash == 0 ? 1 : hash;
}

inline std::vector<ClassifiedDirectStaticPart> ClassifyDirectStaticGroupFromReadyParts(
	std::uint64_t stableSourceGroupID,
	std::uint64_t allocationScopeID,
	std::span<const DirectX::XMMATRIX> objectWorldMatrices,
	std::span<const br::import::RenderableAssetPart> parts)
{
	std::vector<ClassifiedDirectStaticPart> result;
	result.reserve(parts.size());
	for (std::size_t partIndex = 0; partIndex < parts.size(); ++partIndex) {
		const auto& part = parts[partIndex];
		if (part.meshes.empty()) continue;
		const bool hasUnsupportedSkin = std::ranges::any_of(part.meshes, [](const auto& mesh) {
			return mesh && mesh->HasBaseSkin() && !mesh->GetBaseSkin()->HasWindSimulationGroups();
		});
		if (hasUnsupportedSkin) continue;

		ClassifiedDirectStaticPart classified;
		classified.partIndex = partIndex;
		classified.name = part.name;
		classified.skinnedShapeIndex = part.skinnedShapeIndex;
		classified.buildInfo.stableGroupID = MakeDirectStaticPartStableID(stableSourceGroupID, partIndex);
		classified.buildInfo.allocationScopeID = allocationScopeID;
		for (std::size_t meshIndex = 0; meshIndex < part.meshes.size(); ++meshIndex) {
			const auto& mesh = part.meshes[meshIndex];
			if (!mesh || (mesh->HasBaseSkin() && !mesh->GetBaseSkin()->HasWindSimulationGroups())) continue;
			auto material = mesh->material ? mesh->material : Material::GetDefaultMaterial();
			if (!material) continue;
			br::render::StaticMeshTemplateRef meshTemplate;
			meshTemplate.mesh = mesh;
			meshTemplate.material = std::move(material);
			classified.buildInfo.meshTemplates.push_back(std::move(meshTemplate));
			classified.meshOrdinals.push_back(static_cast<std::uint32_t>(meshIndex));
		}
		if (classified.buildInfo.meshTemplates.empty()) continue;
		classified.buildInfo.instanceTransforms.reserve(objectWorldMatrices.size());
		for (const auto& objectWorld : objectWorldMatrices) {
			classified.buildInfo.instanceTransforms.push_back(part.localMatrix * objectWorld);
		}
		if (!classified.buildInfo.instanceTransforms.empty()) result.push_back(std::move(classified));
	}
	return result;
}

struct PreparedStaticRecipeGroup
{
	std::vector<br::staticimport::recipe::PreparedPart> parts;
	std::vector<br::render::StaticGroupBuildInfo> shaderBuildInfos;
};

inline PreparedStaticRecipeGroup PrepareStaticRecipeGroupFromReadyParts(
	std::uint64_t stableSourceGroupID,
	std::uint64_t allocationScopeID,
	std::span<const DirectX::XMMATRIX> objectWorldMatrices,
	std::span<const br::import::RenderableAssetPart> readyParts)
{
	PreparedStaticRecipeGroup result;
	auto classifiedParts = ClassifyDirectStaticGroupFromReadyParts(
		stableSourceGroupID, allocationScopeID, objectWorldMatrices, readyParts);
	result.parts.reserve(classifiedParts.size());
	result.shaderBuildInfos.reserve(classifiedParts.size());
	for (auto& classified : classifiedParts) {
		const auto partIndex = classified.partIndex;
		auto meshOrdinals = std::move(classified.meshOrdinals);
		std::vector<br::render::StaticGroupBuildInfo> oneBuild;
		oneBuild.push_back(std::move(classified.buildInfo));
		auto plan = br::render::StaticObjectRequestService::PrepareStaticGroupsBulkPlan(oneBuild);
		if (plan.groups.size() != 1 || plan.groups.front().meshTemplates.size() != meshOrdinals.size()) continue;
		const auto& prepared = plan.groups.front();
		br::staticimport::recipe::PreparedPart cached;
		cached.sourceGroupID = stableSourceGroupID;
		cached.stableGroupID = prepared.stableGroupID;
		cached.allocationScopeID = prepared.allocationScopeID;
		cached.perObjectRowStride = sizeof(PerObjectCB);
		cached.perObjectRows.resize(prepared.perObjectCBs.size() * sizeof(PerObjectCB));
		std::memcpy(cached.perObjectRows.data(), prepared.perObjectCBs.data(), cached.perObjectRows.size());
		cached.normalMatrices.resize(prepared.normalMatrices.size());
		for (std::size_t i = 0; i < prepared.normalMatrices.size(); ++i) {
			std::memcpy(cached.normalMatrices[i].values, std::addressof(prepared.normalMatrices[i]), sizeof(DirectX::XMFLOAT4X4));
		}
		for (std::size_t i = 0; i < prepared.meshTemplates.size(); ++i) {
			br::staticimport::recipe::PreparedTemplate meshTemplate;
			meshTemplate.partIndex = static_cast<std::uint32_t>(partIndex);
			meshTemplate.meshIndex = meshOrdinals[i];
			for (const auto& workload : prepared.meshTemplates[i].workloadKeys) {
				meshTemplate.workloadKeys.push_back({
					static_cast<std::uint64_t>(workload.compileFlags), workload.renderPhase.ToString(), workload.clodOnly });
			}
			cached.templates.push_back(std::move(meshTemplate));
		}
		result.parts.push_back(std::move(cached));
		result.shaderBuildInfos.push_back(std::move(oneBuild.front()));
	}
	return result;
}
}
