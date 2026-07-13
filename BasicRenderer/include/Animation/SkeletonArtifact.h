#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Animation/DynamicWindMetadata.h"

struct SkeletonArtifactId
{
	std::array<std::uint8_t, 32> digest{};

	bool Empty() const noexcept;
	std::string ToString() const;
	friend bool operator==(const SkeletonArtifactId&, const SkeletonArtifactId&) = default;
};

struct SkeletonArtifactReference
{
	SkeletonArtifactId id{};
	std::uint32_t schemaVersion = 0;
	std::uint32_t jointCount = 0;

	bool Empty() const noexcept { return id.Empty() || jointCount == 0; }
};

struct PackedSkeletonTransform
{
	DirectX::XMFLOAT3 position{};
	DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
};

struct SkeletonWindBoneInvariant
{
	DirectX::XMFLOAT3 bindOrigin{};
	std::uint32_t flags = 0;
	DirectX::XMFLOAT3 branchAxis{ 0.0f, 0.0f, 1.0f };
	float normalizedChainPosition = 0.0f;
	DirectX::XMFLOAT3 branchTangent{ 1.0f, 0.0f, 0.0f };
	std::uint32_t phaseSeed = 0;
};

struct SkeletonArtifactData
{
	std::vector<std::string> jointNames;
	std::vector<std::int32_t> parentIndices;
	std::vector<std::uint32_t> evaluationOrder;
	std::vector<std::uint32_t> rootIndices;
	std::vector<PackedSkeletonTransform> restLocalTransforms;
	std::vector<DirectX::XMFLOAT4X4> inverseBindMatrices;
	std::vector<DirectX::XMFLOAT4X4> bindGlobalMatrices;
	std::vector<std::uint32_t> windSimulationGroupIndices;
	std::vector<SkeletonWindBoneInvariant> windBoneInvariants;
	std::string windProfileIdentity;
	DynamicWindMetadata dynamicWindMetadata;
	std::uint32_t maximumHierarchyDepth = 0;
	std::uint32_t validationFlags = 0;
};

inline constexpr std::uint32_t SKELETON_ARTIFACT_SCHEMA_VERSION = 1;
inline constexpr std::uint32_t SKELETON_ARTIFACT_VALID_FINITE = 1u << 0u;
inline constexpr std::uint32_t SKELETON_ARTIFACT_VALID_HIERARCHY = 1u << 1u;
inline constexpr std::uint32_t SKELETON_ARTIFACT_VALID_WIND = 1u << 2u;

namespace std {
template<> struct hash<SkeletonArtifactId>
{
	size_t operator()(const SkeletonArtifactId& id) const noexcept
	{
		size_t result = 0;
		for (std::uint8_t byte : id.digest) result = (result * 131u) ^ byte;
		return result;
	}
};
}
