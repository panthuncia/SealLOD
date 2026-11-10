#pragma once

// .brlens: a lens's polynomial transport (LensPolynomial.h), one polynomial per degree, for the backward transmission
// path (camera rays from the rear plane to the front plane), with the sampling domain it was fitted over. Written by
// `LensBake fitpoly`, read by the renderer's LensManager (lensMode polynomial).
//
// Binary, little-endian, sequential (strings are u32 length + UTF-8 bytes):
//   char[8]  magic "BRLENS\0\0"
//   u32      version: kBrLensVersion (3). Older versions (1 and 2 held MLP networks and ghost paths) are rejected.
//   u64      prescription source hash (LensPrescription::sourceHash)
//   str      lens name
//   f64 x8   domain: input (rear) plane z, output (front) plane z, lambda min, lambda max (nm), sensor radius, sensor z
//            min, sensor z max, rear half extent (mm)
//   u32      polynomial count, then per polynomial:
//              u32 degree, u32 term count, u32 even terms, f32 center[4], f32 invScale[4],
//              u8 exponents[terms][4] (r, dx, dy, lambda powers),
//              f32 coefficients[4 * even + 2 * (terms - even)] (blocks px[E] py[O] dx[E] dy[O] dz[E] tp[E])
//   str      metadata JSON (fit settings and accuracy; informational)
//   u32      CRC-32 of every preceding byte

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "LensCore/LensDataset.h"
#include "LensCore/LensPolynomial.h"

namespace lens {

	inline constexpr uint32_t kBrLensVersion = 3;
	inline constexpr char kBrLensMagic[8] = { 'B', 'R', 'L', 'E', 'N', 'S', '\0', '\0' };

	struct BrLensFile {
		uint32_t version = kBrLensVersion;
		uint64_t sourceHash = 0;
		std::string lensName;
		SamplingDomain domain; // backward transmission
		std::vector<LensPolynomial> polynomials;
		std::string metadataJson;

		// The polynomial of this degree, or null.
		const LensPolynomial* Find(uint32_t degree) const;
	};

	uint32_t Crc32(const uint8_t* bytes, size_t size);

	std::vector<uint8_t> EncodeBrLens(const BrLensFile& file);
	// expectedHash: when given, a file fitted to a different prescription is rejected.
	std::optional<BrLensFile> DecodeBrLens(const std::vector<uint8_t>& bytes, const std::optional<uint64_t>& expectedHash, std::string* error);
	bool WriteBrLens(const std::filesystem::path& path, const BrLensFile& file, std::string* error);
	std::optional<BrLensFile> ReadBrLens(const std::filesystem::path& path, const std::optional<uint64_t>& expectedHash, std::string* error);

} // namespace lens
