#pragma once

// KTX2 base colour images decoded on the CPU into clod_texture::Texture (ClusterLODTextureColor.h), for the stage 6
// colour-weighted texture tests (docs/clod-appearance-stage6.md) and CLodEval's quality.texture. PNG and JPEG are decoded by
// their callers (stb_image); this is the one KTX2 path both share, so the builder and the evaluator see the same pixels.
//
// The result holds what the renderer samples from a base colour texture: its bytes as stored, read as sRGB colour and linear
// alpha (the renderer views 8-bit base colour formats through their sRGB twin, whatever the file's transfer function), and the
// file's own mip levels (what the GPU filters between), extended to 1x1 with clod_texture::BuildMips when the chain stops
// short. Supported: BC1, BC2, BC3 and BC7 (UNORM or sRGB), R8G8B8A8 and B8G8R8A8 (UNORM or sRGB), and Basis Universal
// (ETC1S or UASTC, transcoded to RGBA8); Zstandard and zlib supercompression. Anything else (BC4/5/6H, one and two channel,
// 16-bit and float formats, arrays, cubes and volumes) does not decode, and callers keep the binary texture test.

#include <BasicRenderer/Assets/ClusterLODTextureColor.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace clod_texture {

	// The revision of the KTX2 decoder: importers mix it into the appearance hash of meshes whose base colour is a KTX2 it reads
	// (Ktx2DecoderIdentity), so caches built before it decoded them (binary fallback) are not reused. Bump it when the decoded
	// pixels of some KTX2 change.
	inline constexpr uint32_t kKtx2DecoderRevision = 1u;

	// Whether bytes start with the KTX2 file identifier.
	bool IsKtx2(std::span<const uint8_t> bytes);

	// kKtx2DecoderRevision when bytes are a KTX2 whose header names a format and layout DecodeKtx2 reads, else 0. Reads only the
	// header: a file that passes can still fail to decode (truncated or corrupt).
	uint32_t Ktx2DecoderIdentity(std::span<const uint8_t> bytes);

	// Decodes a KTX2 (see above). Null on failure, with the reason in *error when given.
	std::shared_ptr<Texture> DecodeKtx2(std::span<const uint8_t> bytes, std::string* error = nullptr);

}
