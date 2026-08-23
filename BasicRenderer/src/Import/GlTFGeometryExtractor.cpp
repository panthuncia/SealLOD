#include "Import/GlTFGeometryExtractor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>

#include <DirectXMath.h>
#include <meshoptimizer.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "Import/CLodCacheLoader.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Mesh/ClusterLODTypes.h"
#include "Mesh/VertexLayout.h"
#include "Mesh/VertexFlags.h"
#include "Mesh/DefaultCLodSettings.h"
#include "Utilities/CachePathUtilities.h"

using nlohmann::json;
using namespace DirectX;

namespace {

constexpr uint32_t kMaxSkinInfluences = 8u;

struct PackedSkinningInfluences
{
	XMUINT4 joints0{ 0, 0, 0, 0 };
	XMUINT4 joints1{ 0, 0, 0, 0 };
	XMFLOAT4 weights0{ 0, 0, 0, 0 };
	XMFLOAT4 weights1{ 0, 0, 0, 0 };
};

// Internal types

struct GLBHeader {
	uint32_t magic = 0;
	uint32_t version = 0;
	uint32_t length = 0;
};

struct GLBChunkSpan {
	uint32_t type = 0;
	uint32_t length = 0;
	uint64_t dataOffset = 0;
};

struct BufferViewInfo {
	size_t bufferIndex = 0;
	size_t byteOffset = 0;
	size_t byteLength = 0;
	size_t byteStride = 0;
};

struct AccessorInfo {
	size_t bufferViewIndex = 0;
	size_t byteOffset = 0;
	size_t count = 0;
	int componentType = 0;
	std::string type;
	bool normalized = false;
};

enum class BufferBacking {
	FileSpan,
	DataUri,
	Fallback, // URI-less buffer used by EXT_meshopt_compression; not directly readable
};

struct BufferSource {
	BufferBacking backing = BufferBacking::FileSpan;
	std::filesystem::path filePath;
	uint64_t fileOffset = 0;
	uint64_t fileLength = 0;
	std::string dataUri;
};

struct ParsedDocument {
	json gltf;
	std::vector<BufferSource> buffers;
	bool hasMeshoptCompression = false;

	// Cache of decompressed bufferView data for EXT_meshopt_compression.
	// Keyed by bufferView index. Protected by mutex for parallel access.
	std::unordered_map<size_t, std::vector<uint8_t>> decompressedBufferViews;
	std::unique_ptr<std::mutex> decompressedViewsMutex = std::make_unique<std::mutex>();
};

// Constants

constexpr uint32_t kGlbMagic = 0x46546C67;
constexpr uint32_t kJsonChunkType = 0x4E4F534A;
constexpr uint32_t kBinChunkType = 0x004E4942;
constexpr int kTrianglesMode = 4;

// File helpers

uint64_t GetFileSize(const std::filesystem::path& path) {
	std::error_code ec;
	const auto size = std::filesystem::file_size(path, ec);
	if (ec) {
		throw std::runtime_error("Failed to query file size: " + path.string());
	}
	return static_cast<uint64_t>(size);
}

std::vector<uint8_t> ReadFileRange(const std::filesystem::path& path, uint64_t offset, uint64_t size) {
	std::vector<uint8_t> out;
	auto& scheduler = TaskSchedulerManager::GetInstance();
	auto ioScope = scheduler.CreateScope("GlTFGeometryExtractor::ReadFileRange");
	scheduler.SubmitBlockingIo(ioScope, TaskDomain::AssetImport, "GlTFGeometryExtractor::ReadFileRange", [&](const br::TaskContext&) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			throw std::runtime_error("Failed to open file: " + path.string());
		}

		const uint64_t fileSize = GetFileSize(path);
		if (offset > fileSize || size > fileSize - offset) {
			throw std::runtime_error("Read range out of file bounds: " + path.string());
		}

		file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		out.resize(static_cast<size_t>(size));
		if (size > 0) {
			file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
			if (!file) {
				throw std::runtime_error("Failed to read file range: " + path.string());
			}
		}
	}, TaskLane::Streaming, {});
	ioScope.Wait();

	return out;
}

uint32_t ReadU32LE(const std::filesystem::path& path, uint64_t offset) {
	auto bytes = ReadFileRange(path, offset, sizeof(uint32_t));
	uint32_t value = 0;
	std::memcpy(&value, bytes.data(), sizeof(uint32_t));
	return value;
}

// GLB parsing

GLBHeader ReadGLBHeader(const std::filesystem::path& path) {
	auto bytes = ReadFileRange(path, 0, 12);
	GLBHeader header;
	std::memcpy(&header.magic, bytes.data(), sizeof(uint32_t));
	std::memcpy(&header.version, bytes.data() + 4, sizeof(uint32_t));
	std::memcpy(&header.length, bytes.data() + 8, sizeof(uint32_t));

	if (header.magic != kGlbMagic) {
		throw std::runtime_error("Invalid GLB magic for: " + path.string());
	}
	if (header.version != 2) {
		throw std::runtime_error("Unsupported GLB version for: " + path.string());
	}

	return header;
}

std::vector<GLBChunkSpan> ReadGLBChunkSpans(const std::filesystem::path& path, uint64_t fileSize) {
	std::vector<GLBChunkSpan> chunks;
	uint64_t offset = 12;

	while (offset < fileSize) {
		if (offset + 8 > fileSize) {
			throw std::runtime_error("Invalid GLB chunk header range");
		}

		const uint32_t chunkLength = ReadU32LE(path, offset);
		const uint32_t chunkType = ReadU32LE(path, offset + 4);
		offset += 8;

		if (offset + chunkLength > fileSize) {
			throw std::runtime_error("Invalid GLB chunk length");
		}

		GLBChunkSpan chunk;
		chunk.type = chunkType;
		chunk.length = chunkLength;
		chunk.dataOffset = offset;
		chunks.push_back(chunk);

		offset += chunkLength;
	}

	return chunks;
}

// Base64 / data URI

int Base64CharToValue(unsigned char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	if (c == '=') return -2;
	return -1;
}

std::vector<uint8_t> DecodeBase64(const std::string& encoded) {
	std::vector<uint8_t> decoded;
	decoded.reserve((encoded.size() * 3) / 4);

	int val = 0;
	int valb = -8;

	for (unsigned char c : encoded) {
		if (std::isspace(c) != 0) {
			continue;
		}

		int d = Base64CharToValue(c);
		if (d == -1) {
			throw std::runtime_error("Invalid base64 data");
		}
		if (d == -2) {
			break;
		}

		val = (val << 6) + d;
		valb += 6;
		if (valb >= 0) {
			decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
			valb -= 8;
		}
	}

	return decoded;
}

std::vector<uint8_t> DecodeDataUri(const std::string& uri) {
	const auto commaPos = uri.find(',');
	if (commaPos == std::string::npos) {
		throw std::runtime_error("Invalid data URI");
	}

	const std::string metadata = uri.substr(0, commaPos);
	const std::string payload = uri.substr(commaPos + 1);

	if (metadata.find(";base64") == std::string::npos) {
		throw std::runtime_error("Only base64 data URIs are supported");
	}

	return DecodeBase64(payload);
}

// Accessor helpers

size_t NumComponentsForType(const std::string& type) {
	if (type == "SCALAR") return 1;
	if (type == "VEC2") return 2;
	if (type == "VEC3") return 3;
	if (type == "VEC4") return 4;
	if (type == "MAT2") return 4;
	if (type == "MAT3") return 9;
	if (type == "MAT4") return 16;
	throw std::runtime_error("Unsupported accessor type: " + type);
}

size_t BytesPerComponent(int componentType) {
	switch (componentType) {
	case 5120:
	case 5121:
		return 1;
	case 5122:
	case 5123:
		return 2;
	case 5125:
	case 5126:
		return 4;
	default:
		throw std::runtime_error("Unsupported component type");
	}
}

AccessorInfo GetAccessorInfo(const json& gltf, size_t accessorIndex) {
	const auto& accessors = gltf.at("accessors");
	if (accessorIndex >= accessors.size()) {
		throw std::runtime_error("Accessor index out of range");
	}

	const auto& accessor = accessors[accessorIndex];
	if (!accessor.contains("bufferView")) {
		throw std::runtime_error("Sparse accessors are not yet supported");
	}

	AccessorInfo info;
	info.bufferViewIndex = accessor.at("bufferView").get<size_t>();
	info.byteOffset = accessor.value<size_t>("byteOffset", static_cast<size_t>(0));
	info.count = accessor.at("count").get<size_t>();
	info.componentType = accessor.at("componentType").get<int>();
	info.type = accessor.at("type").get<std::string>();
	info.normalized = accessor.value<bool>("normalized", false);
	return info;
}

BufferViewInfo GetBufferViewInfo(const json& gltf, size_t bufferViewIndex) {
	const auto& bufferViews = gltf.at("bufferViews");
	if (bufferViewIndex >= bufferViews.size()) {
		throw std::runtime_error("BufferView index out of range");
	}

	const auto& bufferView = bufferViews[bufferViewIndex];
	BufferViewInfo info;
	info.bufferIndex = bufferView.at("buffer").get<size_t>();
	info.byteOffset = bufferView.value<size_t>("byteOffset", static_cast<size_t>(0));
	info.byteLength = bufferView.at("byteLength").get<size_t>();
	info.byteStride = bufferView.value<size_t>("byteStride", static_cast<size_t>(0));
	return info;
}

template <typename T>
T ReadTyped(const std::vector<uint8_t>& buffer, size_t byteOffset) {
	if (byteOffset + sizeof(T) > buffer.size()) {
		throw std::runtime_error("Buffer read out of bounds");
	}

	T value{};
	std::memcpy(&value, buffer.data() + byteOffset, sizeof(T));
	return value;
}

double ReadComponentAsDouble(const std::vector<uint8_t>& source, int componentType, size_t byteOffset, bool normalized = false) {
	switch (componentType) {
	case 5120: {
		const int8_t value = ReadTyped<int8_t>(source, byteOffset);
		if (!normalized) {
			return static_cast<double>(value);
		}
		return std::max(static_cast<double>(value) / 127.0, -1.0);
	}
	case 5121: {
		const uint8_t value = ReadTyped<uint8_t>(source, byteOffset);
		if (!normalized) {
			return static_cast<double>(value);
		}
		return static_cast<double>(value) / 255.0;
	}
	case 5122: {
		const int16_t value = ReadTyped<int16_t>(source, byteOffset);
		if (!normalized) {
			return static_cast<double>(value);
		}
		return std::max(static_cast<double>(value) / 32767.0, -1.0);
	}
	case 5123: {
		const uint16_t value = ReadTyped<uint16_t>(source, byteOffset);
		if (!normalized) {
			return static_cast<double>(value);
		}
		return static_cast<double>(value) / 65535.0;
	}
	case 5125: return static_cast<double>(ReadTyped<uint32_t>(source, byteOffset));
	case 5126: return static_cast<double>(ReadTyped<float>(source, byteOffset));
	default:
		throw std::runtime_error("Unsupported accessor component type");
	}
}

// Buffer reading

std::vector<uint8_t> ReadFromSource(const BufferSource& source, uint64_t offset, uint64_t size) {
	if (source.backing == BufferBacking::Fallback) {
		throw std::runtime_error(
			"Attempted to read from a fallback buffer (no URI). "
			"This buffer is only used by EXT_meshopt_compression and should not be read directly.");
	}

	if (source.backing == BufferBacking::DataUri) {
		const auto bytes = DecodeDataUri(source.dataUri);
		if (offset > bytes.size() || size > bytes.size() - offset) {
			throw std::runtime_error("Data URI range out of bounds");
		}

		std::vector<uint8_t> slice(static_cast<size_t>(size));
		if (size > 0) {
			std::memcpy(slice.data(), bytes.data() + offset, static_cast<size_t>(size));
		}
		return slice;
	}

	if (offset > source.fileLength || size > source.fileLength - offset) {
		throw std::runtime_error("Buffer source range out of bounds: " + source.filePath.string());
	}

	return ReadFileRange(source.filePath, source.fileOffset + offset, size);
}

// EXT_meshopt_compression: check if a bufferView is meshopt-compressed.
bool IsMeshoptCompressedView(const json& gltf, size_t bufferViewIndex) {
	const auto& bvArray = gltf.at("bufferViews");
	if (bufferViewIndex >= bvArray.size()) return false;
	const auto& bv = bvArray[bufferViewIndex];
	return bv.contains("extensions") &&
	       bv["extensions"].contains("EXT_meshopt_compression");
}

// EXT_meshopt_compression: decompress a bufferView and cache the result.
// Returns a pointer to the decompressed data vector (stable because it's in an unordered_map).
const std::vector<uint8_t>& GetDecompressedBufferView(ParsedDocument& doc, size_t bufferViewIndex) {
	// Fast path: check cache under lock.
	{
		std::lock_guard<std::mutex> lock(*doc.decompressedViewsMutex);
		auto it = doc.decompressedBufferViews.find(bufferViewIndex);
		if (it != doc.decompressedBufferViews.end()) {
			return it->second;
		}
	}

	const auto& bv = doc.gltf["bufferViews"][bufferViewIndex];
	const auto& ext = bv["extensions"]["EXT_meshopt_compression"];

	const size_t compBufferIndex = ext.at("buffer").get<size_t>();
	const size_t compByteOffset = ext.value<size_t>("byteOffset", static_cast<size_t>(0));
	const size_t compByteLength = ext.at("byteLength").get<size_t>();
	const size_t count = ext.at("count").get<size_t>();
	const size_t stride = ext.at("byteStride").get<size_t>();
	const std::string mode = ext.at("mode").get<std::string>();
	const std::string filter = ext.value<std::string>("filter", "NONE");

	if (compBufferIndex >= doc.buffers.size()) {
		throw std::runtime_error("EXT_meshopt_compression: buffer index " +
			std::to_string(compBufferIndex) + " out of range");
	}

	// Read compressed data from the source buffer.
	auto compressedData = ReadFromSource(doc.buffers[compBufferIndex], compByteOffset, compByteLength);

	// Decompress.
	std::vector<uint8_t> decompressed(count * stride);
	int rc = -1;
	if (mode == "ATTRIBUTES") {
		rc = meshopt_decodeVertexBuffer(decompressed.data(), count, stride,
			compressedData.data(), compressedData.size());
	} else if (mode == "TRIANGLES") {
		rc = meshopt_decodeIndexBuffer(decompressed.data(), count, stride,
			compressedData.data(), compressedData.size());
	} else if (mode == "INDICES") {
		rc = meshopt_decodeIndexSequence(decompressed.data(), count, stride,
			compressedData.data(), compressedData.size());
	} else {
		throw std::runtime_error("EXT_meshopt_compression: unsupported mode '" + mode + "'");
	}

	if (rc != 0) {
		throw std::runtime_error("EXT_meshopt_compression: meshopt decode failed for bufferView " +
			std::to_string(bufferViewIndex) + " (mode=" + mode + ")");
	}

	// Apply optional decode filter.
	if (filter == "OCTAHEDRAL") {
		meshopt_decodeFilterOct(decompressed.data(), count, stride);
	} else if (filter == "QUATERNION") {
		meshopt_decodeFilterQuat(decompressed.data(), count, stride);
	} else if (filter == "EXPONENTIAL") {
		meshopt_decodeFilterExp(decompressed.data(), count, stride);
	} else if (filter != "NONE") {
		throw std::runtime_error("EXT_meshopt_compression: unsupported filter '" + filter + "'");
	}

	spdlog::debug("EXT_meshopt_compression: decompressed bufferView {} (mode={}, filter={}, count={}, stride={})",
		bufferViewIndex, mode, filter, count, stride);

	// Cache under lock.
	std::lock_guard<std::mutex> lock(*doc.decompressedViewsMutex);
	auto [it, inserted] = doc.decompressedBufferViews.emplace(bufferViewIndex, std::move(decompressed));
	return it->second;
}

std::vector<uint8_t> ReadAccessorRawWindow(ParsedDocument& doc, size_t accessorIndex, size_t firstElement, size_t elementCount, size_t* outStride, size_t* outComponentCount, int* outComponentType) {
	const AccessorInfo accessor = GetAccessorInfo(doc.gltf, accessorIndex);
	const BufferViewInfo view = GetBufferViewInfo(doc.gltf, accessor.bufferViewIndex);

	const size_t componentCount = NumComponentsForType(accessor.type);
	const size_t componentBytes = BytesPerComponent(accessor.componentType);
	const size_t packedElementSize = componentCount * componentBytes;

	// For meshopt-compressed views, the stride comes from the extension, not the bufferView.
	const bool isMeshoptView = doc.hasMeshoptCompression &&
		IsMeshoptCompressedView(doc.gltf, accessor.bufferViewIndex);
	const size_t stride = view.byteStride == 0 ? packedElementSize : view.byteStride;

	if (elementCount == 0) {
		*outStride = stride;
		*outComponentCount = componentCount;
		*outComponentType = accessor.componentType;
		return {};
	}

	if (firstElement > accessor.count || elementCount > accessor.count - firstElement) {
		throw std::runtime_error("Accessor window out of bounds");
	}

	*outStride = stride;
	*outComponentCount = componentCount;
	*outComponentType = accessor.componentType;

	if (isMeshoptView) {
		// Read from the decompressed bufferView cache. The decompressed data
		// represents the full bufferView contents starting at byte 0, so we
		// offset from the accessor's byteOffset only (bufferView.byteOffset
		// was already accounted for during decompression).
		const auto& decompressed = GetDecompressedBufferView(doc, accessor.bufferViewIndex);
		const size_t relativeStart = accessor.byteOffset + firstElement * stride;
		const size_t bytesNeeded = (elementCount - 1) * stride + packedElementSize;

		if (relativeStart + bytesNeeded > decompressed.size()) {
			throw std::runtime_error("Accessor range exceeds decompressed bufferView bounds");
		}

		return std::vector<uint8_t>(decompressed.begin() + relativeStart,
			decompressed.begin() + relativeStart + bytesNeeded);
	}

	if (view.bufferIndex >= doc.buffers.size()) {
		throw std::runtime_error("Buffer index out of range");
	}

	const uint64_t relativeStart = static_cast<uint64_t>(view.byteOffset + accessor.byteOffset + firstElement * stride);
	const uint64_t relativeEnd = relativeStart + static_cast<uint64_t>((elementCount - 1) * stride + packedElementSize);
	const uint64_t viewEnd = static_cast<uint64_t>(view.byteOffset + view.byteLength);

	if (relativeEnd > viewEnd) {
		throw std::runtime_error("Accessor range exceeds bufferView bounds");
	}

	return ReadFromSource(doc.buffers[view.bufferIndex], relativeStart, relativeEnd - relativeStart);
}

// Extension logging

void LogDeclaredExtensions(const json& gltf, const std::string& filePath) {
	auto readStringArray = [](const json& object, const char* key) {
		std::vector<std::string> values;
		if (!object.contains(key) || !object[key].is_array()) {
			return values;
		}

		const auto& array = object[key];
		values.reserve(array.size());
		for (const auto& entry : array) {
			if (entry.is_string()) {
				values.push_back(entry.get<std::string>());
			}
		}
		return values;
	};

	auto joinNames = [](const std::vector<std::string>& names) {
		std::string joined;
		for (size_t i = 0; i < names.size(); ++i) {
			if (i > 0) {
				joined += ", ";
			}
			joined += names[i];
		}
		return joined;
	};

	const auto extensionsUsed = readStringArray(gltf, "extensionsUsed");
	const auto extensionsRequired = readStringArray(gltf, "extensionsRequired");

	if (extensionsUsed.empty() && extensionsRequired.empty()) {
		spdlog::info("glTF extensions for {}: none declared", filePath);
		return;
	}

	if (!extensionsUsed.empty()) {
		spdlog::info("glTF extensionsUsed for {}: {}", filePath, joinNames(extensionsUsed));
	}
	else {
		spdlog::info("glTF extensionsUsed for {}: none", filePath);
	}

	if (!extensionsRequired.empty()) {
		spdlog::info("glTF extensionsRequired for {}: {}", filePath, joinNames(extensionsRequired));
	}
	else {
		spdlog::info("glTF extensionsRequired for {}: none", filePath);
	}
}

// Document parsing

ParsedDocument ParseDocument(const std::string& filePath) {
	const std::filesystem::path path(filePath);
	if (!std::filesystem::is_regular_file(path)) {
		throw std::runtime_error("glTF file not found: " + filePath);
	}

	ParsedDocument doc;
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	std::optional<GLBChunkSpan> glbJsonChunk;
	std::optional<GLBChunkSpan> glbBinChunk;

	if (extension == ".glb") {
		const uint64_t fileSize = GetFileSize(path);
		const GLBHeader header = ReadGLBHeader(path);
		if (header.length != fileSize) {
			spdlog::warn("GLB length header ({}) differs from file size ({}) for {}",
				header.length, fileSize, filePath);
		}

		const auto chunks = ReadGLBChunkSpans(path, fileSize);
		for (const auto& chunk : chunks) {
			if (chunk.type == kJsonChunkType && !glbJsonChunk.has_value()) {
				glbJsonChunk = chunk;
			}
			else if (chunk.type == kBinChunkType && !glbBinChunk.has_value()) {
				glbBinChunk = chunk;
			}
		}

		if (!glbJsonChunk.has_value()) {
			throw std::runtime_error("GLB JSON chunk not found");
		}

		auto jsonBytes = ReadFileRange(path, glbJsonChunk->dataOffset, glbJsonChunk->length);
		doc.gltf = json::parse(jsonBytes.begin(), jsonBytes.end());
	}
	else {
		std::ifstream stream(path);
		if (!stream) {
			throw std::runtime_error("Failed to open glTF JSON: " + filePath);
		}
		stream >> doc.gltf;
	}

	if (!doc.gltf.contains("buffers") || doc.gltf["buffers"].empty()) {
		throw std::runtime_error("glTF contains no buffers");
	}

	// Detect EXT_meshopt_compression usage.
	if (doc.gltf.contains("extensionsUsed") && doc.gltf["extensionsUsed"].is_array()) {
		for (const auto& ext : doc.gltf["extensionsUsed"]) {
			if (ext.is_string() && ext.get<std::string>() == "EXT_meshopt_compression") {
				doc.hasMeshoptCompression = true;
				spdlog::info("glTF: EXT_meshopt_compression detected in {}", filePath);
				break;
			}
		}
	}

	const std::filesystem::path parent = path.parent_path();

	for (size_t bufferIndex = 0; bufferIndex < doc.gltf["buffers"].size(); ++bufferIndex) {
		const auto& buffer = doc.gltf["buffers"][bufferIndex];
		BufferSource source;

		if (buffer.contains("uri")) {
			const std::string uri = buffer["uri"].get<std::string>();
			if (uri.rfind("data:", 0) == 0) {
				source.backing = BufferBacking::DataUri;
				source.dataUri = uri;
				source.fileLength = buffer.value<uint64_t>("byteLength", static_cast<uint64_t>(0));
			}
			else {
				source.backing = BufferBacking::FileSpan;
				source.filePath = parent / std::filesystem::path(uri);
				source.fileOffset = 0;
				const uint64_t actualLength = GetFileSize(source.filePath);
				const uint64_t declaredLength = buffer.value<uint64_t>("byteLength", actualLength);
				if (declaredLength != actualLength) {
					spdlog::warn(
						"glTF buffer byteLength mismatch for {} (declared={}, actual={}); using actual size",
						source.filePath.string(), declaredLength, actualLength);
				}
				source.fileLength = actualLength;
			}
		}
		else {
			if (extension != ".glb") {
				if (doc.hasMeshoptCompression) {
					// EXT_meshopt_compression: URI-less buffers are fallback placeholders.
					// The actual data is stored on bufferViews via the extension and will
					// be decompressed on demand, but we still need this buffer entry for
					// the extension's compressed data references.
					source.backing = BufferBacking::Fallback;
					source.fileLength = buffer.value<uint64_t>("byteLength", static_cast<uint64_t>(0));
					spdlog::debug("glTF: buffer[{}] is a meshopt fallback (no URI, byteLength={})",
						bufferIndex, source.fileLength);
					doc.buffers.push_back(std::move(source));
					continue;
				}
				// Dump buffer object and top-level extensions for diagnostics.
				std::string bufferDump = buffer.dump(2);
				std::string extensionsUsed;
				if (doc.gltf.contains("extensionsUsed")) {
					extensionsUsed = doc.gltf["extensionsUsed"].dump();
				}
				std::string extensionsRequired;
				if (doc.gltf.contains("extensionsRequired")) {
					extensionsRequired = doc.gltf["extensionsRequired"].dump();
				}
				std::string bufferExtensions;
				if (buffer.contains("extensions")) {
					bufferExtensions = buffer["extensions"].dump(2);
				}
				spdlog::error("Buffer [{}] has no 'uri' field in non-GLB file: {}", bufferIndex, filePath);
				spdlog::error("  Buffer object: {}", bufferDump);
				if (!bufferExtensions.empty())
					spdlog::error("  Buffer extensions: {}", bufferExtensions);
				if (!extensionsUsed.empty())
					spdlog::error("  extensionsUsed: {}", extensionsUsed);
				if (!extensionsRequired.empty())
					spdlog::error("  extensionsRequired: {}", extensionsRequired);
				spdlog::error("  Total buffers in file: {}", doc.gltf["buffers"].size());
				throw std::runtime_error(
					"Non-GLB buffer[" + std::to_string(bufferIndex) + "] missing URI in " + filePath
					+ (extensionsRequired.empty() ? "" : " (extensionsRequired: " + extensionsRequired + ")"));
			}
			if (!glbBinChunk.has_value()) {
				throw std::runtime_error("GLB binary chunk not found");
			}

			source.backing = BufferBacking::FileSpan;
			source.filePath = path;
			source.fileOffset = glbBinChunk->dataOffset;
			const uint64_t actualLength = static_cast<uint64_t>(glbBinChunk->length);
			const uint64_t declaredLength = buffer.value<uint64_t>("byteLength", actualLength);
			if (declaredLength != actualLength) {
				spdlog::warn(
					"GLB buffer byteLength mismatch in {} (declared={}, binChunk={}); using bin chunk size",
					filePath, declaredLength, actualLength);
			}
			source.fileLength = actualLength;
		}

		doc.buffers.push_back(std::move(source));
	}

	return doc;
}

// Index reading

std::vector<uint32_t> ReadAllIndices(ParsedDocument& doc, const json& primitive, size_t vertexCount) {
	std::vector<uint32_t> indices;
	if (primitive.contains("indices")) {
		const size_t accessorIndex = primitive["indices"].get<size_t>();
		const AccessorInfo indexAccessor = GetAccessorInfo(doc.gltf, accessorIndex);
		if (NumComponentsForType(indexAccessor.type) != 1) {
			throw std::runtime_error("Index accessor must be SCALAR");
		}

		indices.reserve(indexAccessor.count);
		constexpr size_t kIndexChunkSize = 131072;
		for (size_t firstIndex = 0; firstIndex < indexAccessor.count; firstIndex += kIndexChunkSize) {
			const size_t chunkIndexCount = std::min(kIndexChunkSize, indexAccessor.count - firstIndex);
			size_t indexStride = 0;
			size_t indexComponents = 0;
			int indexComponentType = 0;
			const auto indexBytes = ReadAccessorRawWindow(doc, accessorIndex, firstIndex, chunkIndexCount, &indexStride, &indexComponents, &indexComponentType);
			if (indexComponents != 1) {
				throw std::runtime_error("Index accessor component mismatch");
			}

			for (size_t i = 0; i < chunkIndexCount; ++i) {
				const size_t indexBase = i * indexStride;
				indices.push_back(static_cast<uint32_t>(ReadComponentAsDouble(indexBytes, indexComponentType, indexBase)));
			}
		}
	}
	else {
		indices.reserve(vertexCount);
		for (size_t i = 0; i < vertexCount; ++i) {
			indices.push_back(static_cast<uint32_t>(i));
		}
	}
	return indices;
}

// Normal generation

std::vector<XMFLOAT3> ComputeSmoothNormals(
	ParsedDocument& doc,
	size_t positionAccessorIndex,
	size_t vertexCount,
	const std::vector<uint32_t>& indices)
{
	// Read all positions
	const AccessorInfo positionAccessor = GetAccessorInfo(doc.gltf, positionAccessorIndex);
	std::vector<XMFLOAT3> positions(vertexCount);
	constexpr size_t kChunkSize = 32768;
	for (size_t first = 0; first < vertexCount; first += kChunkSize) {
		const size_t count = std::min(kChunkSize, vertexCount - first);
		size_t stride = 0, components = 0;
		int componentType = 0;
		const auto bytes = ReadAccessorRawWindow(doc, positionAccessorIndex, first, count, &stride, &components, &componentType);
		const size_t componentBytes = BytesPerComponent(componentType);
		for (size_t i = 0; i < count; ++i) {
			const size_t base = i * stride;
			positions[first + i] = XMFLOAT3(
				static_cast<float>(ReadComponentAsDouble(bytes, componentType, base + componentBytes * 0, positionAccessor.normalized)),
				static_cast<float>(ReadComponentAsDouble(bytes, componentType, base + componentBytes * 1, positionAccessor.normalized)),
				static_cast<float>(ReadComponentAsDouble(bytes, componentType, base + componentBytes * 2, positionAccessor.normalized)));
		}
	}

	// Accumulate area-weighted face normals per vertex
	std::vector<XMFLOAT3> normals(vertexCount, XMFLOAT3(0.0f, 0.0f, 0.0f));
	for (size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
		const uint32_t i0 = indices[tri + 0];
		const uint32_t i1 = indices[tri + 1];
		const uint32_t i2 = indices[tri + 2];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
			continue;
		}

		const XMFLOAT3& p0 = positions[i0];
		const XMFLOAT3& p1 = positions[i1];
		const XMFLOAT3& p2 = positions[i2];
		const float e1x = p1.x - p0.x, e1y = p1.y - p0.y, e1z = p1.z - p0.z;
		const float e2x = p2.x - p0.x, e2y = p2.y - p0.y, e2z = p2.z - p0.z;

		const XMFLOAT3 faceNormal(
			e1y * e2z - e1z * e2y,
			e1z * e2x - e1x * e2z,
			e1x * e2y - e1y * e2x);

		const float lenSq = faceNormal.x * faceNormal.x + faceNormal.y * faceNormal.y + faceNormal.z * faceNormal.z;
		if (lenSq <= 1e-20f) {
			continue;
		}

		normals[i0].x += faceNormal.x; normals[i0].y += faceNormal.y; normals[i0].z += faceNormal.z;
		normals[i1].x += faceNormal.x; normals[i1].y += faceNormal.y; normals[i1].z += faceNormal.z;
		normals[i2].x += faceNormal.x; normals[i2].y += faceNormal.y; normals[i2].z += faceNormal.z;
	}

	// Normalize
	for (size_t i = 0; i < vertexCount; ++i) {
		const float lenSq = normals[i].x * normals[i].x + normals[i].y * normals[i].y + normals[i].z * normals[i].z;
		if (lenSq > 1e-20f) {
			const float invLen = 1.0f / std::sqrt(lenSq);
			normals[i].x *= invLen;
			normals[i].y *= invLen;
			normals[i].z *= invLen;
		}
	}

	return normals;
}

// Per-primitive Preprocessing

MeshPreprocessResult BuildPrimitivePreprocessData(
	ParsedDocument& doc,
	const json& primitive,
	const std::string& sourceFilePath,
	size_t meshIndex,
	size_t primitiveIndex)
{
	const int primitiveMode = primitive.value("mode", kTrianglesMode);
	if (primitiveMode != kTrianglesMode) {
		throw std::runtime_error("Only TRIANGLES primitive mode is supported in v1");
	}

	if (!primitive.contains("attributes") || !primitive["attributes"].contains("POSITION")) {
		throw std::runtime_error("Primitive missing POSITION accessor");
	}

	const auto& attributes = primitive["attributes"];

	const size_t positionAccessorIndex = attributes["POSITION"].get<size_t>();
	const AccessorInfo positionAccessor = GetAccessorInfo(doc.gltf, positionAccessorIndex);
	if (NumComponentsForType(positionAccessor.type) != 3) {
		throw std::runtime_error("POSITION accessor must be VEC3");
	}

	const size_t vertexCount = positionAccessor.count;

	bool hasNormals = false;
	size_t normalAccessorIndex = 0;
	AccessorInfo normalAccessor;
	if (attributes.contains("NORMAL")) {
		normalAccessorIndex = attributes["NORMAL"].get<size_t>();
		normalAccessor = GetAccessorInfo(doc.gltf, normalAccessorIndex);
		if (normalAccessor.count != vertexCount || NumComponentsForType(normalAccessor.type) != 3) {
			throw std::runtime_error("NORMAL accessor size/type mismatch");
		}
		hasNormals = true;
	}

	bool hasTexcoords = false;
	size_t texcoordAccessorIndex = 0;
	AccessorInfo texcoordAccessor;
	if (attributes.contains("TEXCOORD_0")) {
		texcoordAccessorIndex = attributes["TEXCOORD_0"].get<size_t>();
		texcoordAccessor = GetAccessorInfo(doc.gltf, texcoordAccessorIndex);
		if (texcoordAccessor.count != vertexCount || NumComponentsForType(texcoordAccessor.type) != 2) {
			throw std::runtime_error("TEXCOORD_0 accessor size/type mismatch");
		}
		hasTexcoords = true;
	}

	bool hasColors = false;
	size_t colorAccessorIndex = 0;
	AccessorInfo colorAccessor;
	if (attributes.contains("COLOR_0")) {
		colorAccessorIndex = attributes["COLOR_0"].get<size_t>();
		colorAccessor = GetAccessorInfo(doc.gltf, colorAccessorIndex);
		const size_t colorComponents = NumComponentsForType(colorAccessor.type);
		if (colorAccessor.count != vertexCount || (colorComponents != 3 && colorComponents != 4)) {
			throw std::runtime_error("COLOR_0 accessor size/type mismatch");
		}
		hasColors = true;
	}

	const bool hasJointIndices = attributes.contains("JOINTS_0");
	const bool hasJointWeights = attributes.contains("WEIGHTS_0");
	if (hasJointIndices != hasJointWeights) {
		throw std::runtime_error("glTF primitive skinning requires both JOINTS_0 and WEIGHTS_0");
	}
	const bool hasJointIndices1 = attributes.contains("JOINTS_1");
	const bool hasJointWeights1 = attributes.contains("WEIGHTS_1");
	if (hasJointIndices1 != hasJointWeights1) {
		throw std::runtime_error("glTF primitive skinning requires both JOINTS_1 and WEIGHTS_1 when using secondary influences");
	}

	bool hasSkinning = false;
	size_t jointAccessorIndex = 0;
	size_t weightAccessorIndex = 0;
	size_t jointAccessorIndex1 = 0;
	size_t weightAccessorIndex1 = 0;
	AccessorInfo jointAccessor;
	AccessorInfo weightAccessor;
	AccessorInfo jointAccessor1;
	AccessorInfo weightAccessor1;
	if (hasJointIndices && hasJointWeights) {
		jointAccessorIndex = attributes["JOINTS_0"].get<size_t>();
		weightAccessorIndex = attributes["WEIGHTS_0"].get<size_t>();
		jointAccessor = GetAccessorInfo(doc.gltf, jointAccessorIndex);
		weightAccessor = GetAccessorInfo(doc.gltf, weightAccessorIndex);
		if (jointAccessor.count != vertexCount || weightAccessor.count != vertexCount) {
			throw std::runtime_error("glTF skinning accessor count mismatch");
		}
		if (NumComponentsForType(jointAccessor.type) != 4 || NumComponentsForType(weightAccessor.type) != 4) {
			throw std::runtime_error("glTF JOINTS_0 and WEIGHTS_0 accessors must be VEC4");
		}
		if (jointAccessor.componentType != 5121 && jointAccessor.componentType != 5123) {
			throw std::runtime_error("glTF JOINTS_0 must use UNSIGNED_BYTE or UNSIGNED_SHORT");
		}
		hasSkinning = true;
	}

	if (hasJointIndices1 && hasJointWeights1) {
		jointAccessorIndex1 = attributes["JOINTS_1"].get<size_t>();
		weightAccessorIndex1 = attributes["WEIGHTS_1"].get<size_t>();
		jointAccessor1 = GetAccessorInfo(doc.gltf, jointAccessorIndex1);
		weightAccessor1 = GetAccessorInfo(doc.gltf, weightAccessorIndex1);
		if (jointAccessor1.count != vertexCount || weightAccessor1.count != vertexCount) {
			throw std::runtime_error("glTF secondary skinning accessor count mismatch");
		}
		if (NumComponentsForType(jointAccessor1.type) != 4 || NumComponentsForType(weightAccessor1.type) != 4) {
			throw std::runtime_error("glTF JOINTS_1 and WEIGHTS_1 accessors must be VEC4");
		}
		if (jointAccessor1.componentType != 5121 && jointAccessor1.componentType != 5123) {
			throw std::runtime_error("glTF JOINTS_1 must use UNSIGNED_BYTE or UNSIGNED_SHORT");
		}
	}

    std::map<uint32_t, size_t> texcoordAccessorIndices;
    uint32_t maxTexcoordSetIndex = 0;
    bool hasAnyTexcoordSet = false;
    for (auto it = attributes.begin(); it != attributes.end(); ++it) {
        const std::string& attributeName = it.key();
        if (attributeName.rfind("TEXCOORD_", 0) != 0) {
            continue;
        }

        const uint32_t setIndex = static_cast<uint32_t>(std::stoul(attributeName.substr(strlen("TEXCOORD_"))));
        texcoordAccessorIndices.emplace(setIndex, it.value().get<size_t>());
        maxTexcoordSetIndex = std::max(maxTexcoordSetIndex, setIndex);
        hasAnyTexcoordSet = true;
    }

	unsigned int meshFlags = VertexFlags::VERTEX_NORMALS; // Always present: authored or generated
	if (hasTexcoords) {
		meshFlags |= VertexFlags::VERTEX_TEXCOORDS;
	}
	if (hasColors) {
		meshFlags |= VertexFlags::VERTEX_COLORS;
	}
	if (hasSkinning) {
		meshFlags |= VertexFlags::VERTEX_SKINNED;
	}

	const uint8_t vertexSize = static_cast<uint8_t>(MeshVertexLayout::VertexSize(meshFlags));
	const unsigned int skinningVertexSize = hasSkinning
		? static_cast<unsigned int>(sizeof(XMFLOAT3) + sizeof(XMFLOAT3) + sizeof(PackedSkinningInfluences))
		: 0u;

	CLodCacheLoader::MeshCacheIdentity cacheIdentity{};
	cacheIdentity.sourceIdentifier = NormalizeCacheSourcePath(sourceFilePath);
	cacheIdentity.primPath = "/glTF/Mesh/" + std::to_string(meshIndex) + "/Primitive/" + std::to_string(primitiveIndex);
	cacheIdentity.subsetName = "";
	bool doubleSidedVoxelSourceNormals = false;
	if (primitive.contains("material")) {
		const size_t materialIndex = primitive["material"].get<size_t>();
		if (doc.gltf.contains("materials") && materialIndex < doc.gltf["materials"].size()) {
			doubleSidedVoxelSourceNormals = doc.gltf["materials"][materialIndex].value("doubleSided", false);
		}
	}
	cacheIdentity.doubleSidedVoxelSourceNormals = doubleSidedVoxelSourceNormals;

	auto prebuiltData = CLodCacheLoader::TryLoadPrebuilt(cacheIdentity);

	ClusterLODBuilderSettings builderSettings = GetDefaultBuilderSettings(cacheIdentity.sourceIdentifier);
	builderSettings.doubleSidedVoxelSourceNormals = doubleSidedVoxelSourceNormals;
	MeshIngestBuilder ingest(vertexSize, skinningVertexSize, meshFlags, builderSettings);
    std::vector<MeshUvSetData> uvSets;
    if (hasAnyTexcoordSet) {
        uvSets.resize(static_cast<size_t>(maxTexcoordSetIndex) + 1u);
        for (uint32_t setIndex = 0; setIndex <= maxTexcoordSetIndex; ++setIndex) {
            uvSets[setIndex].name = "TEXCOORD_" + std::to_string(setIndex);
            uvSets[setIndex].values.assign(vertexCount, XMFLOAT2(0.0f, 0.0f));
        }

        for (const auto& [setIndex, accessorIndex] : texcoordAccessorIndices) {
            const AccessorInfo accessor = GetAccessorInfo(doc.gltf, accessorIndex);
            if (accessor.count != vertexCount || NumComponentsForType(accessor.type) != 2) {
                throw std::runtime_error("TEXCOORD accessor size/type mismatch");
            }

            constexpr size_t kUvChunkSize = 32768;
            for (size_t firstVertex = 0; firstVertex < vertexCount; firstVertex += kUvChunkSize) {
                const size_t chunkVertexCount = std::min(kUvChunkSize, vertexCount - firstVertex);
                size_t stride = 0;
                size_t components = 0;
                int componentType = 0;
                const auto uvBytes = ReadAccessorRawWindow(doc, accessorIndex, firstVertex, chunkVertexCount, &stride, &components, &componentType);
                const size_t componentBytes = BytesPerComponent(componentType);

                for (size_t i = 0; i < chunkVertexCount; ++i) {
                    const size_t uvBase = i * stride;
                    uvSets[setIndex].values[firstVertex + i] = XMFLOAT2(
                        static_cast<float>(ReadComponentAsDouble(uvBytes, componentType, uvBase + componentBytes * 0, accessor.normalized)),
                        static_cast<float>(ReadComponentAsDouble(uvBytes, componentType, uvBase + componentBytes * 1, accessor.normalized)));
                }
            }
        }
    }
    ingest.SetUvSets(std::move(uvSets));
	if (prebuiltData.has_value()) {
		return MeshPreprocessResult(std::move(ingest), std::move(cacheIdentity), std::move(prebuiltData));
	}

	// Read indices early (needed for smooth normal generation and ingest)
	std::vector<uint32_t> allIndices = ReadAllIndices(doc, primitive, vertexCount);

	// Generate smooth normals when the primitive lacks a NORMAL attribute
	std::vector<XMFLOAT3> generatedNormals;
	if (!hasNormals) {
		generatedNormals = ComputeSmoothNormals(doc, positionAccessorIndex, vertexCount, allIndices);
		spdlog::info("glTF: Generated smooth normals for mesh {} primitive {} ({} vertices)",
			meshIndex, primitiveIndex, vertexCount);
	}

	ingest.ReserveVertices(vertexCount);

	constexpr size_t kVertexChunkSize = 32768;
	bool warnedZeroWeightVertex = false;
	for (size_t firstVertex = 0; firstVertex < vertexCount; firstVertex += kVertexChunkSize) {
		const size_t chunkVertexCount = std::min(kVertexChunkSize, vertexCount - firstVertex);

		size_t positionStride = 0;
		size_t positionComponents = 0;
		int positionComponentType = 0;
		const auto positionBytes = ReadAccessorRawWindow(doc, positionAccessorIndex, firstVertex, chunkVertexCount, &positionStride, &positionComponents, &positionComponentType);
		const size_t positionComponentBytes = BytesPerComponent(positionComponentType);

		size_t normalStride = 0;
		size_t normalComponents = 0;
		int normalComponentType = 0;
		std::vector<uint8_t> normalBytes;
		size_t normalComponentBytes = 0;
		if (hasNormals) {
			normalBytes = ReadAccessorRawWindow(doc, normalAccessorIndex, firstVertex, chunkVertexCount, &normalStride, &normalComponents, &normalComponentType);
			normalComponentBytes = BytesPerComponent(normalComponentType);
		}

		size_t texcoordStride = 0;
		size_t texcoordComponents = 0;
		int texcoordComponentType = 0;
		std::vector<uint8_t> texcoordBytes;
		size_t texcoordComponentBytes = 0;
		if (hasTexcoords) {
			texcoordBytes = ReadAccessorRawWindow(doc, texcoordAccessorIndex, firstVertex, chunkVertexCount, &texcoordStride, &texcoordComponents, &texcoordComponentType);
			texcoordComponentBytes = BytesPerComponent(texcoordComponentType);
		}

		size_t colorStride = 0;
		size_t colorComponents = 0;
		int colorComponentType = 0;
		std::vector<uint8_t> colorBytes;
		size_t colorComponentBytes = 0;
		if (hasColors) {
			colorBytes = ReadAccessorRawWindow(doc, colorAccessorIndex, firstVertex, chunkVertexCount, &colorStride, &colorComponents, &colorComponentType);
			colorComponentBytes = BytesPerComponent(colorComponentType);
		}

		size_t jointStride = 0;
		size_t jointComponents = 0;
		int jointComponentType = 0;
		std::vector<uint8_t> jointBytes;
		size_t jointComponentBytes = 0;
		size_t jointStride1 = 0;
		size_t jointComponents1 = 0;
		int jointComponentType1 = 0;
		std::vector<uint8_t> jointBytes1;
		size_t jointComponentBytes1 = 0;
		if (hasSkinning) {
			jointBytes = ReadAccessorRawWindow(doc, jointAccessorIndex, firstVertex, chunkVertexCount, &jointStride, &jointComponents, &jointComponentType);
			jointComponentBytes = BytesPerComponent(jointComponentType);
			if (hasJointIndices1 && hasJointWeights1) {
				jointBytes1 = ReadAccessorRawWindow(doc, jointAccessorIndex1, firstVertex, chunkVertexCount, &jointStride1, &jointComponents1, &jointComponentType1);
				jointComponentBytes1 = BytesPerComponent(jointComponentType1);
			}
		}

		size_t weightStride = 0;
		size_t weightComponents = 0;
		int weightComponentType = 0;
		std::vector<uint8_t> weightBytes;
		size_t weightComponentBytes = 0;
		size_t weightStride1 = 0;
		size_t weightComponents1 = 0;
		int weightComponentType1 = 0;
		std::vector<uint8_t> weightBytes1;
		size_t weightComponentBytes1 = 0;
		if (hasSkinning) {
			weightBytes = ReadAccessorRawWindow(doc, weightAccessorIndex, firstVertex, chunkVertexCount, &weightStride, &weightComponents, &weightComponentType);
			weightComponentBytes = BytesPerComponent(weightComponentType);
			if (hasJointIndices1 && hasJointWeights1) {
				weightBytes1 = ReadAccessorRawWindow(doc, weightAccessorIndex1, firstVertex, chunkVertexCount, &weightStride1, &weightComponents1, &weightComponentType1);
				weightComponentBytes1 = BytesPerComponent(weightComponentType1);
			}
		}

		for (size_t i = 0; i < chunkVertexCount; ++i) {
			const size_t positionBase = i * positionStride;
			const XMFLOAT3 pos(
				static_cast<float>(ReadComponentAsDouble(positionBytes, positionComponentType, positionBase + positionComponentBytes * 0, positionAccessor.normalized)),
				static_cast<float>(ReadComponentAsDouble(positionBytes, positionComponentType, positionBase + positionComponentBytes * 1, positionAccessor.normalized)),
				static_cast<float>(ReadComponentAsDouble(positionBytes, positionComponentType, positionBase + positionComponentBytes * 2, positionAccessor.normalized)));

			XMFLOAT3 normal;
			if (hasNormals) {
				const size_t normalBase = i * normalStride;
				normal = XMFLOAT3(
					static_cast<float>(ReadComponentAsDouble(normalBytes, normalComponentType, normalBase + normalComponentBytes * 0, normalAccessor.normalized)),
					static_cast<float>(ReadComponentAsDouble(normalBytes, normalComponentType, normalBase + normalComponentBytes * 1, normalAccessor.normalized)),
					static_cast<float>(ReadComponentAsDouble(normalBytes, normalComponentType, normalBase + normalComponentBytes * 2, normalAccessor.normalized))
				);
			}
			else {
				normal = generatedNormals[firstVertex + i];
			}

			std::array<std::byte, MeshVertexLayout::MaxVertexSize> packedVertex{};
			std::memcpy(packedVertex.data(), &pos, sizeof(XMFLOAT3));
			std::memcpy(packedVertex.data() + MeshVertexLayout::NormalOffset, &normal, sizeof(XMFLOAT3));

			if (hasTexcoords) {
				const size_t texcoordBase = i * texcoordStride;
				const XMFLOAT2 uv(
					static_cast<float>(ReadComponentAsDouble(texcoordBytes, texcoordComponentType, texcoordBase + texcoordComponentBytes * 0, texcoordAccessor.normalized)),
					static_cast<float>(ReadComponentAsDouble(texcoordBytes, texcoordComponentType, texcoordBase + texcoordComponentBytes * 1, texcoordAccessor.normalized))
				);
				std::memcpy(packedVertex.data() + MeshVertexLayout::TexcoordOffset(meshFlags), &uv, sizeof(XMFLOAT2));
			}

			if (hasColors) {
				const size_t colorBase = i * colorStride;
				const XMFLOAT3 color(
					static_cast<float>(ReadComponentAsDouble(colorBytes, colorComponentType, colorBase + colorComponentBytes * 0, colorAccessor.normalized)),
					static_cast<float>(ReadComponentAsDouble(colorBytes, colorComponentType, colorBase + colorComponentBytes * 1, colorAccessor.normalized)),
					static_cast<float>(ReadComponentAsDouble(colorBytes, colorComponentType, colorBase + colorComponentBytes * 2, colorAccessor.normalized))
				);
				std::memcpy(packedVertex.data() + MeshVertexLayout::ColorOffset(meshFlags), &color, sizeof(XMFLOAT3));
			}

			ingest.AppendVertexBytes(packedVertex.data(), vertexSize);

			if (hasSkinning) {
				const size_t jointBase = i * jointStride;
				const size_t weightBase = i * weightStride;
				const size_t jointBase1 = i * jointStride1;
				const size_t weightBase1 = i * weightStride1;

				uint32_t joints[8] = {};
				float weights[8] = {};
				float weightSum = 0.0f;
				for (size_t componentIndex = 0; componentIndex < 4; ++componentIndex) {
					joints[componentIndex] = static_cast<uint32_t>(ReadComponentAsDouble(
						jointBytes,
						jointComponentType,
						jointBase + jointComponentBytes * componentIndex,
						jointAccessor.normalized));
					weights[componentIndex] = static_cast<float>(ReadComponentAsDouble(
						weightBytes,
						weightComponentType,
						weightBase + weightComponentBytes * componentIndex,
						weightAccessor.normalized));
					weightSum += weights[componentIndex];
				}
				if (hasJointIndices1 && hasJointWeights1) {
					for (size_t componentIndex = 0; componentIndex < 4; ++componentIndex) {
						joints[componentIndex + 4] = static_cast<uint32_t>(ReadComponentAsDouble(
							jointBytes1,
							jointComponentType1,
							jointBase1 + jointComponentBytes1 * componentIndex,
							jointAccessor1.normalized));
						weights[componentIndex + 4] = static_cast<float>(ReadComponentAsDouble(
							weightBytes1,
							weightComponentType1,
							weightBase1 + weightComponentBytes1 * componentIndex,
							weightAccessor1.normalized));
						weightSum += weights[componentIndex + 4];
					}
				}

				if (weightSum > 0.0f) {
					const float invWeightSum = 1.0f / weightSum;
					for (float& weight : weights) {
						weight *= invWeightSum;
					}
				}
				else {
					if (!warnedZeroWeightVertex) {
						spdlog::warn("glTF mesh {} primitive {} has vertices with zero total skin weight; defaulting the first weight to 1", meshIndex, primitiveIndex);
						warnedZeroWeightVertex = true;
					}
					weights[0] = 1.0f;
				}

				PackedSkinningInfluences packedInfluences;
				packedInfluences.joints0 = XMUINT4(joints[0], joints[1], joints[2], joints[3]);
				packedInfluences.joints1 = XMUINT4(joints[4], joints[5], joints[6], joints[7]);
				packedInfluences.weights0 = XMFLOAT4(weights[0], weights[1], weights[2], weights[3]);
				packedInfluences.weights1 = XMFLOAT4(weights[4], weights[5], weights[6], weights[7]);

				std::array<std::byte, sizeof(XMFLOAT3) + sizeof(XMFLOAT3) + sizeof(PackedSkinningInfluences)> packedSkinningVertex{};
				std::memcpy(packedSkinningVertex.data(), &pos, sizeof(XMFLOAT3));
				size_t skinningOffset = sizeof(XMFLOAT3);
				std::memcpy(packedSkinningVertex.data() + skinningOffset, &normal, sizeof(XMFLOAT3));
				skinningOffset += sizeof(XMFLOAT3);
				std::memcpy(packedSkinningVertex.data() + skinningOffset, &packedInfluences, sizeof(PackedSkinningInfluences));
				ingest.AppendSkinningVertexBytes(packedSkinningVertex.data(), skinningVertexSize);
			}
		}
	}

	ingest.ReserveIndices(allIndices.size());
	ingest.AppendIndices(allIndices.data(), allIndices.size());

	if (!prebuiltData.has_value()) {
		ClusterLODPrebuildArtifacts artifacts = ingest.BuildClusterLODArtifacts();
		ClusterLODPrebuiltData savedPrebuiltData;

		const bool cacheSaved = CLodCacheLoader::SavePrebuiltLocked(cacheIdentity, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload(), &savedPrebuiltData);
		if (!cacheSaved) {
			spdlog::warn("Failed to save CLOD cache for {} (mesh {}, primitive {})", sourceFilePath, meshIndex, primitiveIndex);
			prebuiltData = std::move(artifacts.prebuiltData);
		}
		else {
			auto diskBackedPrebuilt = CLodCacheLoader::TryLoadPrebuilt(cacheIdentity);
			if (diskBackedPrebuilt.has_value()) {
				prebuiltData = std::move(diskBackedPrebuilt);
			}
			else {
				spdlog::warn("Immediate CLOD cache reload missed after save for {} (mesh {}, primitive {}); using saved disk metadata directly.", sourceFilePath, meshIndex, primitiveIndex);
				prebuiltData = std::move(savedPrebuiltData);
			}
		}
	}

	return MeshPreprocessResult(std::move(ingest), std::move(cacheIdentity), std::move(prebuiltData));
}

}

// Public API

namespace GlTFGeometryExtractor {

ExtractionResult ExtractAll(const std::string& filePath) {
	ParsedDocument doc = ParseDocument(filePath);
	LogDeclaredExtensions(doc.gltf, filePath);

	ExtractionResult result;
	result.gltf = std::move(doc.gltf);

	// Re-attach a const reference so helpers still work with the doc
	doc.gltf = result.gltf; // shallow copy of the json; buffers stay in doc

	if (!result.gltf.contains("meshes")) {
		return result;
	}

	struct PrimitiveWorkItem {
		size_t meshIndex = 0;
		size_t primitiveIndex = 0;
		const json* primitive = nullptr;
	};

	const auto& meshArray = result.gltf["meshes"];
	std::vector<PrimitiveWorkItem> workItems;
	std::vector<std::vector<std::optional<MeshPreprocessResult>>> preprocessed;
	preprocessed.resize(meshArray.size());

	for (size_t meshIndex = 0; meshIndex < meshArray.size(); ++meshIndex) {
		const auto& mesh = meshArray[meshIndex];
		if (!mesh.contains("primitives")) {
			continue;
		}

		const auto& primitiveArray = mesh["primitives"];
		preprocessed[meshIndex].resize(primitiveArray.size());

		for (size_t primitiveIndex = 0; primitiveIndex < primitiveArray.size(); ++primitiveIndex) {
			workItems.push_back(PrimitiveWorkItem{
				.meshIndex = meshIndex,
				.primitiveIndex = primitiveIndex,
				.primitive = &primitiveArray[primitiveIndex]
				});
		}
	}

	TaskSchedulerManager::GetInstance().ParallelFor("GlTFGeometryExtractor::PreprocessPrimitives", workItems.size(), [&](size_t workIndex) {
		const PrimitiveWorkItem& workItem = workItems[workIndex];
		preprocessed[workItem.meshIndex][workItem.primitiveIndex] = BuildPrimitivePreprocessData(
			doc,
			*workItem.primitive,
			filePath,
			workItem.meshIndex,
			workItem.primitiveIndex);
		});

	for (const PrimitiveWorkItem& workItem : workItems) {
		auto& preprocessSlot = preprocessed[workItem.meshIndex][workItem.primitiveIndex];
		if (!preprocessSlot.has_value()) {
			throw std::runtime_error("Missing preprocessed primitive data");
		}

		result.primitives.emplace_back(
			workItem.meshIndex,
			workItem.primitiveIndex,
			std::move(preprocessSlot.value()));
	}

	return result;
}

} // namespace GlTFGeometryExtractor
