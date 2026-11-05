#include "Core/Image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <unordered_map>

namespace clodeval::image {
namespace fs = std::filesystem;

namespace {
	// DXGI_FORMAT values. This file does not include a D3D header, so CLodEval stays free of one.
	constexpr uint32_t kRgba32Float = 2, kRgba16Float = 10, kRg32Uint = 17, kRgba8Unorm = 28, kRgba8UnormSrgb = 29, kRg16Float = 34, kR32Float = 41;
	constexpr size_t kHeaderBytes = 148;  // magic + DDS_HEADER + DDS_HEADER_DXT10

	struct RawDds {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t format = 0;
		std::vector<char> bytes;
		const char* Pixels() const { return bytes.data() + kHeaderBytes; }
	};

	bool ReadRaw(const fs::path& path, RawDds& out, std::string& outError)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input) {
			outError = std::format("cannot open '{}'", path.string());
			return false;
		}
		const auto size = static_cast<size_t>(input.tellg());
		if (size < kHeaderBytes) {
			outError = std::format("'{}' is too short to be a DDS", path.string());
			return false;
		}
		out.bytes.resize(size);
		input.seekg(0);
		input.read(out.bytes.data(), static_cast<std::streamsize>(size));
		uint32_t fourCc = 0;
		std::memcpy(&out.height, out.bytes.data() + 12, 4);
		std::memcpy(&out.width, out.bytes.data() + 16, 4);
		std::memcpy(&fourCc, out.bytes.data() + 84, 4);
		std::memcpy(&out.format, out.bytes.data() + 128, 4);
		if (std::memcmp(out.bytes.data(), "DDS ", 4) != 0 || fourCc != 0x30315844u) {
			outError = std::format("'{}' is not a DX10 DDS", path.string());
			return false;
		}
		return true;
	}

	uint32_t Crc32(const uint8_t* data, size_t size, uint32_t seed = 0xFFFFFFFFu)
	{
		static const std::array<uint32_t, 256> table = [] {
			std::array<uint32_t, 256> values{};
			for (uint32_t index = 0; index < 256u; ++index) {
				uint32_t value = index;
				for (int bit = 0; bit < 8; ++bit) {
					value = (value & 1u) != 0u ? 0xEDB88320u ^ (value >> 1u) : value >> 1u;
				}
				values[index] = value;
			}
			return values;
		}();
		uint32_t crc = seed;
		for (size_t index = 0; index < size; ++index) {
			crc = table[(crc ^ data[index]) & 0xFFu] ^ (crc >> 8u);
		}
		return crc;
	}

	void AppendBigEndian(std::vector<uint8_t>& out, uint32_t value)
	{
		for (int shift = 24; shift >= 0; shift -= 8) {
			out.push_back(static_cast<uint8_t>(value >> shift));
		}
	}

	void AppendChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& payload)
	{
		AppendBigEndian(out, static_cast<uint32_t>(payload.size()));
		const size_t crcStart = out.size();
		out.insert(out.end(), type, type + 4);
		out.insert(out.end(), payload.begin(), payload.end());
		AppendBigEndian(out, Crc32(out.data() + crcStart, out.size() - crcStart) ^ 0xFFFFFFFFu);
	}
}

float HalfToFloat(uint16_t half)
{
	const int exponent = (half >> 10) & 0x1F;
	const int mantissa = half & 0x3FF;
	const float sign = (half & 0x8000) != 0 ? -1.0f : 1.0f;
	if (exponent == 0) {
		return sign * std::ldexp(static_cast<float>(mantissa), -24);
	}
	if (exponent == 31) {
		return mantissa == 0 ? sign * HUGE_VALF : std::nanf("");
	}
	return sign * std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
}

bool ReadPlane(const fs::path& path, Plane& out, std::string& outError)
{
	RawDds raw;
	if (!ReadRaw(path, raw, outError)) {
		return false;
	}
	uint32_t channels = 0;
	size_t bytesPerPixel = 0;
	switch (raw.format) {
	case kRgba32Float: channels = 4; bytesPerPixel = 16; break;
	case kR32Float: channels = 1; bytesPerPixel = 4; break;
	case kRgba16Float: channels = 4; bytesPerPixel = 8; break;
	case kRg16Float: channels = 2; bytesPerPixel = 4; break;
	case kRgba8Unorm:
	case kRgba8UnormSrgb: channels = 4; bytesPerPixel = 4; break;
	default:
		outError = std::format("'{}' has DXGI format {}, which is not a float layer", path.string(), raw.format);
		return false;
	}
	const size_t pixels = static_cast<size_t>(raw.width) * raw.height;
	if (raw.bytes.size() < kHeaderBytes + pixels * bytesPerPixel) {
		outError = std::format("'{}' is truncated", path.string());
		return false;
	}
	out.width = raw.width;
	out.height = raw.height;
	out.channels = channels;
	out.data.resize(pixels * channels);
	const char* source = raw.Pixels();
	const size_t values = out.data.size();
	if (raw.format == kRgba32Float || raw.format == kR32Float) {
		std::memcpy(out.data.data(), source, values * sizeof(float));
	}
	else if (raw.format == kRgba16Float || raw.format == kRg16Float) {
		for (size_t index = 0; index < values; ++index) {
			uint16_t half = 0;
			std::memcpy(&half, source + index * 2u, 2);
			out.data[index] = HalfToFloat(half);
		}
	}
	else {
		for (size_t index = 0; index < values; ++index) {
			out.data[index] = static_cast<uint8_t>(source[index]) / 255.0f;
		}
	}
	return true;
}

bool ReadKeys(const fs::path& path, KeyImage& out, std::string& outError)
{
	RawDds raw;
	if (!ReadRaw(path, raw, outError)) {
		return false;
	}
	if (raw.format != kRg32Uint) {
		outError = std::format("'{}' has DXGI format {}, not R32G32_UINT", path.string(), raw.format);
		return false;
	}
	const size_t pixels = static_cast<size_t>(raw.width) * raw.height;
	if (raw.bytes.size() < kHeaderBytes + pixels * 8u) {
		outError = std::format("'{}' is truncated", path.string());
		return false;
	}
	out.width = raw.width;
	out.height = raw.height;
	out.keys.resize(pixels);
	std::memcpy(out.keys.data(), raw.Pixels(), pixels * 8u);
	return true;
}

bool WritePlane(const fs::path& path, const Plane& plane, std::string& outError)
{
	if (plane.channels != 1u && plane.channels != 4u) {
		outError = "only one or four channels can be written";
		return false;
	}
	const uint32_t format = plane.channels == 1u ? kR32Float : kRgba32Float;
	const uint32_t pitch = plane.width * plane.channels * 4u;
	std::array<uint32_t, 31> header{};
	header[0] = 124;
	header[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000;
	header[2] = plane.height;
	header[3] = plane.width;
	header[4] = pitch;
	header[6] = 1;
	header[18] = 32;
	header[19] = 0x4;
	header[20] = 0x30315844u;
	header[26] = 0x1000;
	const std::array<uint32_t, 5> dx10{ format, 3, 0, 1, 0 };
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write("DDS ", 4);
	output.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
	output.write(reinterpret_cast<const char*>(dx10.data()), sizeof(dx10));
	output.write(reinterpret_cast<const char*>(plane.data.data()), static_cast<std::streamsize>(plane.data.size() * sizeof(float)));
	output.flush();
	if (!output) {
		outError = std::format("failed to write '{}'", path.string());
		return false;
	}
	return true;
}

bool WritePng(const fs::path& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb, std::string& outError)
{
	const size_t rowBytes = static_cast<size_t>(width) * 3u;
	if (rgb.size() < rowBytes * height) {
		outError = "WritePng was given too little data";
		return false;
	}
	std::vector<uint8_t> raw;
	raw.reserve((rowBytes + 1u) * height);
	for (uint32_t y = 0; y < height; ++y) {
		raw.push_back(0);  // no filter
		raw.insert(raw.end(), rgb.begin() + static_cast<ptrdiff_t>(y * rowBytes), rgb.begin() + static_cast<ptrdiff_t>((y + 1u) * rowBytes));
	}

	std::vector<uint8_t> deflated{ 0x78, 0x01 };
	uint32_t adlerA = 1, adlerB = 0;
	for (size_t offset = 0; offset < raw.size();) {
		const size_t length = std::min<size_t>(raw.size() - offset, 65535u);
		const bool last = offset + length == raw.size();
		deflated.push_back(last ? 1 : 0);
		deflated.push_back(static_cast<uint8_t>(length));
		deflated.push_back(static_cast<uint8_t>(length >> 8u));
		deflated.push_back(static_cast<uint8_t>(~length));
		deflated.push_back(static_cast<uint8_t>(~length >> 8u));
		deflated.insert(deflated.end(), raw.begin() + static_cast<ptrdiff_t>(offset), raw.begin() + static_cast<ptrdiff_t>(offset + length));
		for (size_t index = offset; index < offset + length; ++index) {
			adlerA = (adlerA + raw[index]) % 65521u;
			adlerB = (adlerB + adlerA) % 65521u;
		}
		offset += length;
	}
	AppendBigEndian(deflated, (adlerB << 16u) | adlerA);

	std::vector<uint8_t> file{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	std::vector<uint8_t> header;
	AppendBigEndian(header, width);
	AppendBigEndian(header, height);
	header.insert(header.end(), { 8, 2, 0, 0, 0 });
	AppendChunk(file, "IHDR", header);
	AppendChunk(file, "IDAT", deflated);
	AppendChunk(file, "IEND", {});

	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
	output.flush();
	if (!output) {
		outError = std::format("failed to write '{}'", path.string());
		return false;
	}
	return true;
}

Plane BoxDownsample(const Plane& plane, uint32_t factor)
{
	Plane result;
	result.width = plane.width / factor;
	result.height = plane.height / factor;
	result.channels = plane.channels;
	result.data.assign(result.Pixels() * result.channels, 0.0f);
	const float scale = 1.0f / static_cast<float>(factor * factor);
	for (uint32_t y = 0; y < result.height * factor; ++y) {
		for (uint32_t x = 0; x < result.width * factor; ++x) {
			for (uint32_t channel = 0; channel < plane.channels; ++channel) {
				result.At(x / factor, y / factor, channel) += plane.At(x, y, channel) * scale;
			}
		}
	}
	return result;
}

Plane Luminance(const Plane& rgb)
{
	Plane result;
	result.width = rgb.width;
	result.height = rgb.height;
	result.channels = 1;
	result.data.resize(result.Pixels());
	for (size_t index = 0; index < result.data.size(); ++index) {
		const float* pixel = rgb.data.data() + index * rgb.channels;
		result.data[index] = 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
	}
	return result;
}

Plane ToneMap(const Plane& hdr, float exposure)
{
	Plane result;
	result.width = hdr.width;
	result.height = hdr.height;
	result.channels = 3;
	result.data.resize(result.Pixels() * 3u);
	for (size_t index = 0; index < result.Pixels(); ++index) {
		for (size_t channel = 0; channel < 3u; ++channel) {
			const float value = std::max(hdr.data[index * hdr.channels + channel], 0.0f) * exposure;
			result.data[index * 3u + channel] = std::pow(value / (1.0f + value), 1.0f / 2.2f);
		}
	}
	return result;
}

double MeanSquaredError(const Plane& a, const Plane& b, const std::vector<uint8_t>& mask)
{
	double sum = 0.0;
	size_t samples = 0;
	for (size_t index = 0; index < a.Pixels(); ++index) {
		if (mask[index] == 0u) {
			continue;
		}
		for (uint32_t channel = 0; channel < a.channels; ++channel) {
			const double difference = static_cast<double>(a.data[index * a.channels + channel]) - b.data[index * b.channels + channel];
			sum += difference * difference;
			++samples;
		}
	}
	return samples != 0u ? sum / static_cast<double>(samples) : 0.0;
}

double Psnr(const Plane& a, const Plane& b, const std::vector<uint8_t>& mask)
{
	const double error = MeanSquaredError(a, b, mask);
	return error > 1.0e-10 ? -10.0 * std::log10(error) : 100.0;
}

double Ssim(const Plane& a, const Plane& b, const std::vector<uint8_t>& mask)
{
	constexpr uint32_t kWindow = 8u;
	constexpr double kC1 = 0.01 * 0.01, kC2 = 0.03 * 0.03;
	const uint32_t width = a.width, height = a.height;
	if (width < kWindow || height < kWindow) {
		return 0.0;
	}
	// Summed-area tables of a, b, a*a, b*b and a*b, so each window costs the same however large the image is.
	const size_t stride = width + 1u;
	std::array<std::vector<double>, 5> tables;
	for (auto& table : tables) {
		table.assign(stride * (height + 1u), 0.0);
	}
	for (uint32_t y = 0; y < height; ++y) {
		std::array<double, 5> row{};
		for (uint32_t x = 0; x < width; ++x) {
			const double va = a.At(x, y), vb = b.At(x, y);
			const std::array<double, 5> values{ va, vb, va * va, vb * vb, va * vb };
			for (size_t table = 0; table < 5u; ++table) {
				row[table] += values[table];
				tables[table][(y + 1u) * stride + x + 1u] = tables[table][y * stride + x + 1u] + row[table];
			}
		}
	}
	const auto windowSum = [&](size_t table, uint32_t x, uint32_t y) {
		const auto& values = tables[table];
		return values[(y + kWindow) * stride + x + kWindow] - values[y * stride + x + kWindow] - values[(y + kWindow) * stride + x] + values[y * stride + x];
	};
	double total = 0.0;
	size_t windows = 0;
	const double count = kWindow * kWindow;
	for (uint32_t y = 0; y + kWindow <= height; ++y) {
		for (uint32_t x = 0; x + kWindow <= width; ++x) {
			if (mask[static_cast<size_t>(y + kWindow / 2u) * width + x + kWindow / 2u] == 0u) {
				continue;
			}
			const double meanA = windowSum(0, x, y) / count, meanB = windowSum(1, x, y) / count;
			const double varianceA = windowSum(2, x, y) / count - meanA * meanA;
			const double varianceB = windowSum(3, x, y) / count - meanB * meanB;
			const double covariance = windowSum(4, x, y) / count - meanA * meanB;
			total += ((2.0 * meanA * meanB + kC1) * (2.0 * covariance + kC2)) /
				((meanA * meanA + meanB * meanB + kC1) * (varianceA + varianceB + kC2));
			++windows;
		}
	}
	return windows != 0u ? total / static_cast<double>(windows) : 0.0;
}

CoverageComparison CompareCoverage(const Plane& reference, const Plane& test)
{
	CoverageComparison result;
	double minimum = 0.0, maximum = 0.0, excess = 0.0, missing = 0.0;
	for (size_t index = 0; index < reference.Pixels(); ++index) {
		const double r = reference.data[index * reference.channels];
		const double t = test.data[index * test.channels];
		result.referenceSum += r;
		result.testSum += t;
		minimum += std::min(r, t);
		maximum += std::max(r, t);
		excess += std::max(t - r, 0.0);
		missing += std::max(r - t, 0.0);
	}
	if (result.referenceSum > 0.0) {
		result.ratio = result.testSum / result.referenceSum;
		result.excessFraction = excess / result.referenceSum;
		result.missingFraction = missing / result.referenceSum;
	}
	result.softIou = maximum > 0.0 ? minimum / maximum : 0.0;
	return result;
}

std::vector<double> KeyExtents(const KeyImage& keys, const std::vector<uint8_t>& mask)
{
	struct Box {
		uint32_t minX, minY, maxX, maxY;
	};
	std::unordered_map<uint64_t, Box> boxes;
	constexpr uint64_t kMetaMask = (uint64_t{ 1 } << 33u) - 1u;  // the key without its depth
	for (uint32_t y = 0; y < keys.height; ++y) {
		for (uint32_t x = 0; x < keys.width; ++x) {
			const size_t index = static_cast<size_t>(y) * keys.width + x;
			if (mask[index] == 0u || keys.keys[index] == kEmptyKey) {
				continue;
			}
			const auto [found, inserted] = boxes.try_emplace(keys.keys[index] & kMetaMask, Box{ x, y, x, y });
			if (!inserted) {
				found->second.minX = std::min(found->second.minX, x);
				found->second.minY = std::min(found->second.minY, y);
				found->second.maxX = std::max(found->second.maxX, x);
				found->second.maxY = std::max(found->second.maxY, y);
			}
		}
	}
	std::vector<double> extents;
	extents.reserve(boxes.size());
	for (const auto& [key, box] : boxes) {
		extents.push_back(static_cast<double>(std::max(box.maxX - box.minX, box.maxY - box.minY) + 1u));
	}
	return extents;
}

}
