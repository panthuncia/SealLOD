#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <BasicRenderer/Assets/ClusterLODTextureColor.h>

// Base colour of a source asset's materials, for measuring texture appearance against the leaves (quality.texture). glTF, binary
// (.glb) or JSON (.gltf), with images embedded, in buffers or in files beside it: the material of each primitive, its base colour
// factor, texture and texture transform (KHR_texture_transform), and the alpha mode. Images are decoded on first use and kept as
// 8-bit sRGB mip chains: PNG and JPEG with stb_image (mips box filtered in linear light), KTX2 with the generator's decoder
// (clod_texture::DecodeKtx2, the file's own mips). Nothing here needs the renderer or a GPU. See docs/clod-appearance-stage6.md.
namespace clodeval {

// Decoded base colour images and the colour math live with the generator's (BasicRenderer/Assets/ClusterLODTextureColor.h), so
// quality.texture and the builder's colour-weighted texture term measure colour the same way.
using SourceTexture = clod_texture::Texture;

struct SourceMaterial {
	std::array<float, 4> baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
	int32_t baseColorImage = -1;  // index into the asset's images, or -1
	uint32_t texCoord = 0;        // the UV set the base colour texture reads
	// KHR_texture_transform as a 2x3 affine matrix, row-major (u' = m0 u + m1 v + m2, v' = m3 u + m4 v + m5), and the factor it
	// scales UV footprints by (the square root of its determinant).
	bool uvTransformed = false;
	std::array<double, 6> uvTransform{ 1.0, 0.0, 0.0, 0.0, 1.0, 0.0 };
	double uvFootprintScale = 1.0;
	bool masked = false;          // alphaMode MASK
	float alphaCutoff = 0.5f;
	bool doubleSided = false;
};

class SourceMaterials {
public:
	// Returns nullptr and sets outError for anything but a readable .glb or .gltf.
	static std::shared_ptr<SourceMaterials> Open(const std::filesystem::path& path, std::string& outError);

	// The material of a cache mesh named after its glTF primitive ("/glTF/Mesh/<m>/Primitive/<p>", what CLodCacheTool records as
	// prim_path and CachedMesh::Name returns, optionally with a ":<subset>" or "#<branch>" suffix). Null if unknown.
	const SourceMaterial* MaterialForMesh(std::string_view meshName) const;

	// Decodes the image on first use. Thread-safe. Null if it cannot be decoded.
	const SourceTexture* Image(int32_t image) const;

	const std::filesystem::path& Path() const { return m_path; }

private:
	std::filesystem::path m_path;
	std::vector<SourceMaterial> m_materials;
	std::vector<std::vector<int32_t>> m_primitiveMaterials;  // [mesh][primitive] -> material, -1 for none
	// Where each image some material uses as base colour is: embedded bytes, or a range of a file read when it is decoded.
	struct ImageSource {
		std::vector<uint8_t> bytes;
		std::filesystem::path file;
		uint64_t offset = 0;
		uint64_t length = 0;  // 0 with a file: all of it
	};
	std::vector<ImageSource> m_imageSources;
	mutable std::vector<std::unique_ptr<SourceTexture>> m_images;
	mutable std::unique_ptr<std::once_flag[]> m_imageOnce;
};

// A simulated per-group appearance bake (quality.texture's offline prototype of stage 6's baked atlas). Groups at or above minDepth
// (and, with minDeltaE > 0, only those whose own texture error at their finest cut reaches it) draw their base colour from a
// per-triangle texel tile resampled from the leaves instead of from their UVs (or, remapped, through a tile of leaf UVs). Texel size is texelScale times the finest cut the
// triangles draw at, never finer than the source texel there. Analysis only: nothing is built.
struct TextureBake {
	std::string name;
	double texelScale = 1.0;
	int32_t minDepth = 1;
	double minDeltaE = 0.0;
	// false: texels hold colour (a baked atlas, BC7 with mips). true: texels hold the matched leaf's UV (a remap atlas, two 16-bit
	// UNORM per texel, no mips); the material is sampled through it, filtered by the cut like the leaves.
	bool remap = false;
};

using clod_texture::SampleLinear;
using clod_texture::TextureLod;

// A material's base colour at (u, v) (before its texture transform, which this applies): factor times texture, linear RGB and
// alpha. lod is for the untransformed footprint times material.uvFootprintScale.
std::array<float, 4> BaseColor(const SourceMaterial& material, const SourceTexture* texture, double u, double v, double lod);

using clod_texture::DeltaE;
using clod_texture::LinearToLab;

}
