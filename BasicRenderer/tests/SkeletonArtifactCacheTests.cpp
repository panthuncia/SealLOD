#include "Import/SkeletonArtifactCache.h"

#include "Animation/Skeleton.h"
#include "Mesh/ClusterLODTypes.h"

#include <DirectXMath.h>
#include <Windows.h>

#include <filesystem>
#include <iostream>

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
		!Check(data->windBoneInvariants.size() == 2u, "wind invariants were not cached")) return 1;
	const SkeletonArtifactReference wrongCount{ first->id, first->schemaVersion, first->jointCount + 1u };
	if (!Check(!SkeletonArtifactCache::Load(wrongCount, &error), "registry accepted a mismatched joint count")) return 1;

	const auto skeletonA = SkeletonArtifactCache::ResolveSkeleton(*first, &error);
	const auto skeletonB = SkeletonArtifactCache::ResolveSkeleton(*first, &error);
	if (!Check(skeletonA != nullptr, error.c_str()) ||
		!Check(skeletonA == skeletonB, "registry did not share the base skeleton") ||
		!Check(skeletonA->GetBoneCount() == 2u, "resolved skeleton joint count is wrong") ||
		!Check(skeletonA->GetWindBoneInvariants().size() == 2u, "resolved skeleton lost wind invariants")) return 1;

	std::filesystem::remove_all(cacheRoot);
	return 0;
}
