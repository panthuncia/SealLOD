#include "Import/SkeletonArtifactCache.h"
#include "Import/SkeletonArtifactValidation.h"

#include "Animation/Skeleton.h"
#include "Mesh/ClusterLODTypes.h"

#include <DirectXMath.h>
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace {

DirectX::XMFLOAT4X4 Matrix(float x, float y, float z)
{
	DirectX::XMFLOAT4X4 result{};
	DirectX::XMStoreFloat4x4(&result, DirectX::XMMatrixTranslation(x, y, z));
	return result;
}

bool Check(bool condition, const char* message)
{
	if (!condition) std::cerr << message << '\n';
	return condition;
}

}

int main()
{
	const auto cacheRoot = std::filesystem::temp_directory_path() /
		("BasicRendererSkeletonArtifactTests_" + std::to_string(GetCurrentProcessId()));
	std::filesystem::remove_all(cacheRoot);
	_putenv_s("SARP_CACHE_ROOT", cacheRoot.string().c_str());

	ClusterLODAssemblySkeletonData source;
	source.jointNames = { "Root", "Root/Branch" };
	source.parentIndices = { -1, 0 };
	source.inverseBindMatrices = { Matrix(0.0f, 0.0f, 0.0f), Matrix(0.0f, -2.0f, 0.0f) };
	source.restLocalMatrices = { Matrix(0.0f, 0.0f, 0.0f), Matrix(0.0f, 2.0f, 0.0f) };
	source.bindGlobalMatrices = { Matrix(0.0f, 0.0f, 0.0f), Matrix(0.0f, 2.0f, 0.0f) };
	source.windSimulationGroupIndices = { 0u, 1u };
	source.windProfileIdentity = "C:\\Install\\SARPOverrideAssets\\Trees\\Test.usd";
	source.dynamicWindMetadata.enabled = true;
	source.dynamicWindMetadata.groups.resize(2);
	source.dynamicWindMetadata.groups[0].flags = DynamicWindMetadata::GroupFlagTrunk;
	source.dynamicWindMetadata.groups[0].role = DynamicWindSimulationGroupRole::Trunk;
	source.dynamicWindMetadata.groups[1].role = DynamicWindSimulationGroupRole::DetailBranch;
	source.dynamicWindMetadata.bones.resize(2);
	source.dynamicWindMetadata.bones[1].chainOriginBoneIndex = 1u;
	source.dynamicWindMetadata.bones[1].chainBoneCount = 1u;

	std::string error;
	const auto first = SkeletonArtifactCache::Save(source, &error);
	if (!Check(first.has_value(), error.c_str())) return 1;
	const auto second = SkeletonArtifactCache::Save(source, &error);
	if (!Check(second.has_value() && second->id == first->id, "canonical artifact ID was not deterministic")) return 1;

	const auto data = SkeletonArtifactCache::Load(*first, &error);
	if (!Check(data != nullptr, error.c_str()) ||
		!Check(data->jointNames == source.jointNames, "joint names did not round-trip") ||
		!Check(data->evaluationOrder == std::vector<std::uint32_t>({ 0u, 1u }), "evaluation order was not cached") ||
		!Check(data->windProfileIdentity == "sarpoverrideassets/trees/test.usd", "profile identity was not normalized") ||
		!Check(data->windBoneInvariants.size() == 2u, "wind invariants were not cached") ||
		!Check(data->lodVariants.size() >= 2u && data->lodVariants.size() <= 16u, "skeleton LOD variants were not generated") ||
		!Check(data->lodVariants[0].lodToBaseBone == std::vector<std::uint32_t>({ 0u, 1u }), "LOD0 is not identity") ||
		!Check(data->lodVariants.back().lodToBaseBone == std::vector<std::uint32_t>({ 0u }), "detail branch was not removed") ||
		!Check(data->lodVariants.back().baseToLodBone == std::vector<std::uint32_t>({ 0u, 0u }), "removed branch did not map to retained ancestor")) return 1;
	if (!Check(SkeletonArtifactValidation::Validate(*first, &error), error.c_str())) return 1;
	const SkeletonArtifactReference wrongCount{ first->id, first->schemaVersion, first->jointCount + 1u };
	if (!Check(!SkeletonArtifactCache::Load(wrongCount, &error), "registry accepted a mismatched joint count")) return 1;
	const SkeletonArtifactReference wrongSchema{ first->id, 1u, first->jointCount };
	if (!Check(!SkeletonArtifactCache::Load(wrongSchema, &error), "schema-v1 artifact reference was not rejected")) return 1;
	if (!Check(!SkeletonArtifactValidation::Validate(wrongSchema, &error), "validator accepted a stale artifact reference")) return 1;

	const auto artifactPath = cacheRoot / "skeleton" / ("skeleton_" + first->id.ToString() + ".brskel");
	const auto validArtifactBytes = [&]() {
		std::ifstream stream(artifactPath, std::ios::binary);
		return std::vector<char>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}();
	std::filesystem::remove(artifactPath);
	if (!Check(!SkeletonArtifactValidation::Validate(*first, &error), "validator accepted a missing artifact")) return 1;
	{
		std::ofstream stream(artifactPath, std::ios::binary | std::ios::trunc);
		stream.write(validArtifactBytes.data(), static_cast<std::streamsize>(validArtifactBytes.size() / 2u));
	}
	if (!Check(!SkeletonArtifactValidation::Validate(*first, &error), "validator accepted a truncated artifact")) return 1;
	{
		std::ofstream stream(artifactPath, std::ios::binary | std::ios::trunc);
		stream.write(validArtifactBytes.data(), static_cast<std::streamsize>(validArtifactBytes.size()));
	}
	if (!Check(SkeletonArtifactValidation::Validate(*first, &error), error.c_str())) return 1;

	const auto skeletonA = SkeletonArtifactCache::ResolveSkeleton(*first, &error);
	const auto skeletonB = SkeletonArtifactCache::ResolveSkeleton(*first, &error);
	if (!Check(skeletonA != nullptr, error.c_str()) ||
		!Check(skeletonA == skeletonB, "registry did not share the base skeleton") ||
		!Check(skeletonA->GetBoneCount() == 2u, "resolved skeleton joint count is wrong") ||
		!Check(skeletonA->GetWindBoneInvariants().size() == 2u, "resolved skeleton lost wind invariants") ||
		!Check(skeletonA->GetSkeletonLodVariants().size() == data->lodVariants.size(), "resolved skeleton lost LOD variants")) return 1;

	ClusterLODAssemblySkeletonData chain;
	constexpr std::uint32_t trunkBones = 8u;
	for (std::uint32_t bone = 0; bone <= trunkBones; ++bone) {
		chain.jointNames.push_back("Bone" + std::to_string(bone));
		chain.parentIndices.push_back(bone == 0u ? -1 : static_cast<std::int32_t>(bone - 1u));
		chain.bindGlobalMatrices.push_back(Matrix(0.0f, static_cast<float>(bone), 0.0f));
		chain.inverseBindMatrices.push_back(Matrix(0.0f, -static_cast<float>(bone), 0.0f));
		chain.restLocalMatrices.push_back(Matrix(0.0f, bone == 0u ? 0.0f : 1.0f, 0.0f));
		chain.windSimulationGroupIndices.push_back(bone == 0u ? 0xFFFFFFFFu : 0u);
	}
	chain.dynamicWindMetadata.enabled = true;
	chain.dynamicWindMetadata.groups.resize(1u);
	chain.dynamicWindMetadata.groups[0].flags = DynamicWindMetadata::GroupFlagTrunk;
	chain.dynamicWindMetadata.groups[0].role = DynamicWindSimulationGroupRole::Trunk;
	chain.dynamicWindMetadata.bones.resize(chain.jointNames.size());
	for (std::uint32_t bone = 1u; bone <= trunkBones; ++bone) {
		chain.dynamicWindMetadata.bones[bone].chainOriginBoneIndex = 1u;
		chain.dynamicWindMetadata.bones[bone].indexInBoneChain = bone - 1u;
		chain.dynamicWindMetadata.bones[bone].chainBoneCount = trunkBones;
	}
	const auto chainRef = SkeletonArtifactCache::Save(chain, &error);
	const auto chainData = chainRef ? SkeletonArtifactCache::Load(*chainRef, &error) : nullptr;
	if (!Check(chainData != nullptr, error.c_str()) || !Check(chainData->lodVariants.size() >= 3u, "chain LOD variants missing")) return 1;
	for (std::size_t lod = 1u; lod < chainData->lodVariants.size(); ++lod) {
		const auto& finer = chainData->lodVariants[lod - 1u];
		const auto& coarser = chainData->lodVariants[lod];
		if (!Check(std::ranges::includes(finer.lodToBaseBone, coarser.lodToBaseBone), "skeleton LODs are not nested") ||
			!Check(static_cast<float>(finer.lodToBaseBone.size()) / static_cast<float>(coarser.lodToBaseBone.size()) <= 1.75f + 1.0e-4f,
				"adjacent skeleton LOD ratio exceeds configured maximum")) return 1;
	}
	if (!Check(chainData->lodVariants.back().lodToBaseBone.size() == 3u, "far LOD is not root plus two animated segments")) return 1;

	std::filesystem::remove_all(cacheRoot);
	return 0;
}
