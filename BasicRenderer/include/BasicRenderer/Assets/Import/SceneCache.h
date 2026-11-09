#pragma once

// A scene as a CLod cache holds it: everything the renderer needs to place a scene whose geometry is all in the cache,
// without opening the source. CLodCacheTool writes it into the scene manifest's "scene" section (SceneManifest.h);
// br::import::LoadImportedAsset loads a manifest that has one from the cache alone, reading only the files the scene's
// textures stream from.
//
// It is the importer's own intermediate form, not a copy of its output: the glTF importer describes a file into it and
// builds its renderables from it whether or not the description came from a cache, so a cached scene's materials and
// texture requests (and their cache keys) are the ones a live import makes.
//
// Paths are relative to the source scene's directory, so the cache and the source each move freely as long as the
// files the source references stay beside it.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <nlohmann/json_fwd.hpp>

#include "BasicRenderer/Assets/MaterialDescription.h"
#include <BasicRenderer/Assets/TextureTypes.h>

namespace br::import
{
	// Where an image's encoded bytes are.
	struct SceneCacheImage
	{
		enum class Kind : std::uint8_t {
			File,      // a file of its own (a glTF image URI)
			FileRange, // a range of a file (a .glb or .bin bufferView)
			Embedded,  // in memory (a data URI or an inline buffer): only a live import can read it
		};
		Kind kind = Kind::File;
		// File / FileRange: the file, relative to the source's directory, in generic form; empty is the source itself.
		std::string file;
		std::uint64_t offset = 0;
		std::uint64_t length = 0;
		// FileRange: the glTF bufferView and mime type, which name the image's texture identity.
		std::uint32_t bufferView = 0;
		std::string mimeType;
		// The bytes are KTX2, so they stream through the KTX2 provider (decided by the bytes, not the mime type).
		bool ktx2 = false;
		// The glTF image index (Embedded images are read through it).
		std::uint32_t sourceImage = 0;
	};

	// A material texture: an image, its sampler, and how the material uses it.
	struct SceneCacheTexture
	{
		std::uint32_t image = 0;
		// glTF sampler values (defaults when the texture names none).
		int wrapS = 10497;
		int wrapT = 10497;
		int minFilter = 9987;
		int magFilter = 9729;
		bool preferSRGB = false;
		TextureSemantic semantic = TextureSemantic::Unknown;
		bool preservePackedChannels = false;
		NormalMapConvention normalConvention = NormalMapConvention::DirectX;
	};

	// The texture slots of a material, in SceneCacheMaterial::textures order.
	enum class SceneCacheTextureSlot : std::uint8_t {
		BaseColor, Metallic, Roughness, Emissive, Opacity, AO, Height, Normal, Count
	};

	struct SceneCacheMaterial
	{
		// The source's material index (the shared material cache's key).
		std::uint32_t sourceMaterial = 0;
		// The material without textures; each slot's texture is an index into SceneCacheDescription::textures, or -1.
		MaterialDescription description;
		std::array<std::int32_t, static_cast<size_t>(SceneCacheTextureSlot::Count)> textures{ -1, -1, -1, -1, -1, -1, -1, -1 };
	};

	// One primitive's CLod cache entry and the vertex format its mesh is made with.
	struct SceneCachePrimitive
	{
		std::uint32_t mesh = 0;
		std::uint32_t primitive = 0;
		std::string primPath;
		bool doubleSidedVoxelSourceNormals = false;
		std::uint32_t vertexFlags = 0;
		std::uint32_t vertexSize = 0;
		std::uint32_t skinningVertexSize = 0;
		std::uint32_t uvSetCount = 0;
		// Index into SceneCacheDescription::materials; -1 is the default material.
		std::int32_t material = -1;
	};

	// A placed mesh: a node of the source.
	struct SceneCachePart
	{
		std::string name;
		DirectX::XMFLOAT4X4 localMatrix{};
		// Empty: one placement at localMatrix. Otherwise each placement, localMatrix already applied.
		std::vector<DirectX::XMFLOAT4X4> instanceMatrices;
		// Indices into SceneCacheDescription::primitives, in the mesh's primitive order.
		std::vector<std::uint32_t> primitives;
	};

	struct SceneCacheDescription
	{
		std::string sourceIdentifier;
		std::vector<SceneCacheImage> images;
		std::vector<SceneCacheTexture> textures;
		std::vector<SceneCacheMaterial> materials;
		std::vector<SceneCachePrimitive> primitives;
		std::vector<SceneCachePart> parts;
		// Why the scene cannot load from the cache alone (embedded images, skins), or empty when it can.
		std::string notCacheableReason;
	};

	// The manifest's "scene" section. Materials keep every field of a legacy (glTF-style) MaterialDescription except its
	// textures, which are the slot indices; an OpenPBR material is refused (std::runtime_error), as nothing makes one yet.
	[[nodiscard]] nlohmann::json SceneCacheToJson(const SceneCacheDescription& scene);
	// Throws std::runtime_error naming what is wrong.
	[[nodiscard]] SceneCacheDescription SceneCacheFromJson(const nlohmann::json& section);
}
