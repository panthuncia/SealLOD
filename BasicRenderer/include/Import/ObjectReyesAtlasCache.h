#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace br::import {

inline constexpr std::uint32_t kObjectReyesAtlasCacheSchemaVersion = 6u;

struct ObjectReyesAtlasCacheIdentity
{
	std::uint32_t schemaVersion{ kObjectReyesAtlasCacheSchemaVersion };
	std::string canonicalNifPath;
	std::string nifContentHash;
	std::string reyesConfigHash;
	std::uint64_t clodAssetSettingsHash{ 0 };

	bool IsComplete() const
	{
		return schemaVersion == kObjectReyesAtlasCacheSchemaVersion &&
			!canonicalNifPath.empty() && !nifContentHash.empty() &&
			!reyesConfigHash.empty();
	}
};

struct ObjectReyesAtlasMeshBinding
{
	std::uint32_t payloadMeshIndex{ 0 };
	std::string ddsPath;
	std::uint32_t atlasUvSetIndex{ 0 };
	std::uint32_t width{ 0 };
	std::uint32_t height{ 0 };
	float displacementMin{ 0.0f };
	float displacementMax{ 0.0f };
	std::string storageFormat;
	std::string sourceMaterialName;
	std::uint32_t sourceMaterialIndex{ UINT32_MAX };
};

struct ObjectReyesAtlasVariantManifest
{
	std::uint32_t schemaVersion{ kObjectReyesAtlasCacheSchemaVersion };
	ObjectReyesAtlasCacheIdentity identity;
	std::uint64_t textureOverrideHash{ 0 };
	std::vector<ObjectReyesAtlasMeshBinding> meshes;
};

} // namespace br::import
