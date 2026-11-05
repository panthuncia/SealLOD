#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Capture layers as plain arrays, and the comparisons run on them. No renderer and no GPU: CLodCapture
// writes the files, and everything here works on them later.
namespace clodeval::image {

// Interleaved floats. Layers of every format decode to this.
struct Plane {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t channels = 0;
	std::vector<float> data;

	size_t Pixels() const { return static_cast<size_t>(width) * height; }
	float& At(uint32_t x, uint32_t y, uint32_t channel = 0) { return data[(static_cast<size_t>(y) * width + x) * channels + channel]; }
	float At(uint32_t x, uint32_t y, uint32_t channel = 0) const { return data[(static_cast<size_t>(y) * width + x) * channels + channel]; }
};

// A 64-bit-per-pixel layer: the visibility key or a packed debug payload.
struct KeyImage {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint64_t> keys;
};

inline constexpr uint64_t kEmptyKey = ~uint64_t{ 0 };

// Both return false with outError set for a missing file or a format they do not handle.
bool ReadPlane(const std::filesystem::path& path, Plane& out, std::string& outError);
bool ReadKeys(const std::filesystem::path& path, KeyImage& out, std::string& outError);

// One channel writes R32_FLOAT and four write R32G32B32A32_FLOAT.
bool WritePlane(const std::filesystem::path& path, const Plane& plane, std::string& outError);

// rgb is 8 bits per channel, top row first, with no padding. Compressed with stored blocks only, so
// a file is about width * height * 3 bytes. These are for looking at, not keeping.
bool WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb, std::string& outError);

float HalfToFloat(uint16_t half);

// Averages factor x factor blocks. width and height must be multiples of factor.
Plane BoxDownsample(const Plane& plane, uint32_t factor);

// Rec. 709 luminance of an RGB(A) plane.
Plane Luminance(const Plane& rgb);

// HDR to display range: x / (1 + x) after exposure, then gamma 2.2. RGB in, RGB out.
Plane ToneMap(const Plane& hdr, float exposure);

// Mean squared error in [0, 1] units over pixels where mask is nonzero, and PSNR for a peak of 1.
// PSNR of identical images is reported as 100 rather than infinity, so results stay finite and comparable.
double MeanSquaredError(const Plane& a, const Plane& b, const std::vector<uint8_t>& mask);
double Psnr(const Plane& a, const Plane& b, const std::vector<uint8_t>& mask);

// Mean SSIM of two single-channel images in [0, 1], over 8x8 windows whose centre is in the mask.
double Ssim(const Plane& a, const Plane& b, const std::vector<uint8_t>& mask);

struct CoverageComparison {
	double referenceSum = 0.0;  // pixels' worth of coverage
	double testSum = 0.0;
	double ratio = 0.0;         // test / reference: above 1 means too dense
	double softIou = 0.0;       // sum(min) / sum(max): 1 means the same fractional coverage everywhere
	double excessFraction = 0.0;   // sum(max(test - reference, 0)) / reference
	double missingFraction = 0.0;  // sum(max(reference - test, 0)) / reference
};
CoverageComparison CompareCoverage(const Plane& reference, const Plane& test);

// The screen-space size of each distinct key among pixels where mask is nonzero: the larger of the
// bounding box's width and height.
std::vector<double> KeyExtents(const KeyImage& keys, const std::vector<uint8_t>& mask);

}
