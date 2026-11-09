#pragma once

// An import profile: what a scene's files do not say about themselves, given to the importer by whoever prepared the
// scene. A profile travels with a scene cache (its manifest embeds it), so the cache tool and the renderer read the scene
// the same way.
//
// {
//   "name": "zorah-texproc",
//   "skipMeshes": "SM_Kite_VAT.*",                       // ECMAScript regex over glTF mesh names; matches are not imported
//   "materialChannels": [                                // first matching rule wins
//     { "texture": "metallicRoughness",                  // the glTF slot
//       "channels": 2,                                   // optional: only textures with this many channels (BC5: 2)
//       "nameSuffix": "_MetallicRoughness",              // optional: only images whose name or URI stem ends so
//       "metallic": 1, "roughness": 0 }                  // the channels to read, 0=R 1=G 2=B 3=A
//   ]
// }

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace br::import
{
	struct ImportProfileChannelRule
	{
		// The glTF texture slot the rule applies to. Only "metallicRoughness" is defined.
		std::string texture;
		// Conditions; unset ones match anything.
		std::optional<std::uint32_t> channels;
		std::string nameSuffix;
		// Channel indices to read.
		std::optional<std::uint32_t> metallic;
		std::optional<std::uint32_t> roughness;
	};

	struct ImportProfile
	{
		std::string name;
		std::string skipMeshes;
		std::vector<ImportProfileChannelRule> materialChannels;

		// Stable over runs; feeds cache keys of anything the profile changes.
		[[nodiscard]] std::uint64_t Hash() const;
		// Whether a mesh of this name is skipped.
		[[nodiscard]] bool SkipsMesh(std::string_view meshName) const;
		// The first metallic-roughness rule matching a texture, or null.
		[[nodiscard]] const ImportProfileChannelRule* FindMetallicRoughnessRule(std::uint32_t textureChannels, std::string_view imageName) const;
	};

	// Parses a profile's JSON text. Throws std::runtime_error naming what is wrong.
	[[nodiscard]] ImportProfile ParseImportProfile(std::string_view jsonText);
	// Reads and parses a profile file.
	[[nodiscard]] ImportProfile LoadImportProfile(const std::string& path);
	// The profile as JSON text, as ParseImportProfile reads it.
	[[nodiscard]] std::string ImportProfileToJson(const ImportProfile& profile);
}
