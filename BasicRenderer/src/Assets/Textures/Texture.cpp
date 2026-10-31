#include "Assets/Textures/Pipeline/TexturePipelineStages.h"
#include <BasicRenderer/Assets/TexturePipeline.h>
#include "BasicRenderer/Assets/Texture.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Assets/Textures/TextureFactory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <DirectXTex.h>
#include <tracy/Tracy.hpp>
#include <windows.h>

#include <spdlog/spdlog.h>

#include <rhi_dx12.h>
#include <rhi_helpers.h>

#include "Runtime/Device/DeviceManager.h"
#include "Runtime/IO/DirectStorageManager.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "Assets/Textures/Processing/TextureProcessingManager.h"
#include "Assets/Textures/Cache/ProcessedTextureCache.h"

namespace {
std::atomic<uint64_t> g_directStorageTextureBeginUploadCount{ 0 };

const char* ToString(TextureSemantic semantic) {
	switch (semantic) {
	case TextureSemantic::BaseColor:
		return "BaseColor";
	case TextureSemantic::Emissive:
		return "Emissive";
	case TextureSemantic::Normal:
		return "Normal";
	case TextureSemantic::Height:
		return "Height";
	case TextureSemantic::AO:
		return "AO";
	case TextureSemantic::Opacity:
		return "Opacity";
	case TextureSemantic::Metallic:
		return "Metallic";
	case TextureSemantic::Roughness:
		return "Roughness";
	case TextureSemantic::MetallicRoughness:
		return "MetallicRoughness";
	case TextureSemantic::OpenPBRColor:
		return "OpenPBRColor";
	case TextureSemantic::OpenPBRScalar:
		return "OpenPBRScalar";
	default:
		return "Unknown";
	}
}

const char* ToString(TextureLoadPathTelemetry path) {
	switch (path) {
	case TextureLoadPathTelemetry::DirectStorageGpuDirect:
		return "directstorage_gpu_direct";
	case TextureLoadPathTelemetry::DirectStorageSystemMemoryRead:
		return "directstorage_system_memory_read";
	case TextureLoadPathTelemetry::CpuFileRead:
		return "cpu_file_read";
	case TextureLoadPathTelemetry::MemoryMappedFileRead:
		return "memory_mapped_file_read";
	case TextureLoadPathTelemetry::InMemoryContainer:
		return "in_memory_container";
	case TextureLoadPathTelemetry::DeferredFileReference:
		return "deferred_file_reference";
	default:
		return "unknown";
	}
}

bool IsTruthyEnvironmentFlag(const char* name) {
	char* value = nullptr;
	size_t len = 0;
	if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
		return false;
	}

	const bool enabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
	free(value);
	return enabled;
}

bool IsDirectStorageGpuTextureUploadDisabled() {
	static const bool disabled = IsTruthyEnvironmentFlag("BASICRENDERER_DISABLE_DIRECTSTORAGE_TEXTURE_UPLOAD");
	return disabled;
}

void WarnOnce(std::string key, std::string message);

enum class DirectStorageTexturePreflightResult : uint8_t {
	Eligible = 0,
	Disabled,
	QueueUnavailable,
	UnsupportedFileType,
	UnsupportedFormat,
	InvalidMetadata,
	InvalidLayout,
	InvalidRequest,
	ResourceCreateFailed,
	EnqueueFailed,
};

const char* ToString(DirectStorageTexturePreflightResult result) {
	switch (result) {
	case DirectStorageTexturePreflightResult::Eligible:
		return "eligible";
	case DirectStorageTexturePreflightResult::Disabled:
		return "disabled";
	case DirectStorageTexturePreflightResult::QueueUnavailable:
		return "queue_unavailable";
	case DirectStorageTexturePreflightResult::UnsupportedFileType:
		return "unsupported_file_type";
	case DirectStorageTexturePreflightResult::UnsupportedFormat:
		return "unsupported_format";
	case DirectStorageTexturePreflightResult::InvalidMetadata:
		return "invalid_metadata";
	case DirectStorageTexturePreflightResult::InvalidLayout:
		return "invalid_layout";
	case DirectStorageTexturePreflightResult::InvalidRequest:
		return "invalid_request";
	case DirectStorageTexturePreflightResult::ResourceCreateFailed:
		return "resource_create_failed";
	case DirectStorageTexturePreflightResult::EnqueueFailed:
		return "enqueue_failed";
	default:
		return "unknown";
	}
}

void RecordDirectStorageTexturePreflight(
	DirectStorageTexturePreflightResult result,
	const std::string& path,
	const std::string& detail = {})
{
	static std::atomic_uint64_t eligibleCount{ 0 };
	static std::atomic_uint64_t skipCount{ 0 };
	static std::atomic_uint64_t failureCount{ 0 };

	if (result == DirectStorageTexturePreflightResult::Eligible) {
		TracyPlot("SARP.Texture.DirectStorage.Preflight.Eligible", static_cast<int64_t>(eligibleCount.fetch_add(1, std::memory_order_relaxed) + 1u));
		return;
	}

	const bool failure =
		result == DirectStorageTexturePreflightResult::ResourceCreateFailed ||
		result == DirectStorageTexturePreflightResult::EnqueueFailed;
	const uint64_t count = failure
		? failureCount.fetch_add(1, std::memory_order_relaxed) + 1u
		: skipCount.fetch_add(1, std::memory_order_relaxed) + 1u;
	if (failure) {
		TracyPlot("SARP.Texture.DirectStorage.Preflight.Failures", static_cast<int64_t>(count));
	}
	else {
		TracyPlot("SARP.Texture.DirectStorage.Preflight.Skips", static_cast<int64_t>(count));
	}

	if (failure) {
		WarnOnce(
			"texture-ds-preflight-failure|" + path + "|" + ToString(result) + "|" + detail,
			"TextureAsset: DirectStorage texture preflight failed for '" + path + "' result=" + ToString(result) +
				(detail.empty() ? std::string{} : " detail='" + detail + "'"));
		return;
	}

	const std::string detailText = detail.empty() ? std::string{} : " detail='" + detail + "'";
	spdlog::debug(
		"TextureAsset: DirectStorage texture preflight skipped '{}' result={}{}",
		path,
		ToString(result),
		detailText);
}

const char* ToString(TextureUploadPathTelemetry path) {
	switch (path) {
	case TextureUploadPathTelemetry::DirectStorageGpuDirect:
		return "directstorage_gpu_direct";
	case TextureUploadPathTelemetry::CpuImmediateUpload:
		return "cpu_immediate_upload";
	case TextureUploadPathTelemetry::AsyncProcessingPlaceholder:
		return "async_processing_placeholder";
	case TextureUploadPathTelemetry::AsyncProcessingReadyUpload:
		return "async_processing_ready_upload";
	case TextureUploadPathTelemetry::ProcessingCacheUpload:
		return "processing_cache_upload";
	case TextureUploadPathTelemetry::ProcessingFailedFallback:
		return "processing_failed_fallback";
	case TextureUploadPathTelemetry::DeferredPlaceholder:
		return "deferred_placeholder";
	default:
		return "unknown";
	}
}

std::string FormatWin32Error(DWORD error)
{
	LPSTR message = nullptr;
	const DWORD length = FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		error,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		reinterpret_cast<LPSTR>(&message),
		0,
		nullptr);
	std::string result = length != 0 && message != nullptr
		? std::string(message, length)
		: "unknown Win32 error";
	if (message) {
		LocalFree(message);
	}
	while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == '.')) {
		result.pop_back();
	}
	return result + " (GetLastError=" + std::to_string(error) + ")";
}

void WarnOnce(std::string key, std::string message)
{
	static std::mutex mutex;
	static std::unordered_set<std::string> seen;
	std::scoped_lock lock(mutex);
	if (seen.insert(std::move(key)).second) {
		spdlog::warn("{}", message);
	}
}

void InfoOnce(std::string key, std::string message)
{
	static std::mutex mutex;
	static std::unordered_set<std::string> seen;
	std::scoped_lock lock(mutex);
	if (seen.insert(std::move(key)).second) {
		spdlog::info("{}", message);
	}
}

void LogSourceDataBuildAttribution(
	const char* builder,
	const std::string& path,
	const std::string& reason)
{
	const std::string effectiveReason = reason.empty() ? "unspecified" : reason;
	InfoOnce(
		std::string("texture-source-build|") + builder + "|" + path + "|" + effectiveReason,
		std::string("TextureAsset: source-data build builder=") + builder +
			" path='" + path + "' reason='" + effectiveReason + "'");
}

class MappedFileView {
public:
	MappedFileView() = default;
	~MappedFileView()
	{
		Reset();
	}

	MappedFileView(const MappedFileView&) = delete;
	MappedFileView& operator=(const MappedFileView&) = delete;

	MappedFileView(MappedFileView&& other) noexcept
	{
		MoveFrom(std::move(other));
	}

	MappedFileView& operator=(MappedFileView&& other) noexcept
	{
		if (this != &other) {
			Reset();
			MoveFrom(std::move(other));
		}
		return *this;
	}

	static std::optional<MappedFileView> Open(const std::wstring& path, std::string* outError = nullptr)
	{
		ZoneScopedN("TextureAsset::MappedFileView::Open");
		if (outError) {
			outError->clear();
		}

		HANDLE file = CreateFileW(
			path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
			nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			if (outError) {
				*outError = "CreateFileW failed: " + FormatWin32Error(GetLastError());
			}
			return std::nullopt;
		}

		LARGE_INTEGER size{};
		if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
			if (outError) {
				*outError = "GetFileSizeEx failed or file is empty: " + FormatWin32Error(GetLastError());
			}
			CloseHandle(file);
			return std::nullopt;
		}
		if (static_cast<unsigned long long>(size.QuadPart) > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)())) {
			if (outError) {
				*outError = "file is too large to map into address space";
			}
			CloseHandle(file);
			return std::nullopt;
		}

		HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if (mapping == nullptr) {
			if (outError) {
				*outError = "CreateFileMappingW failed: " + FormatWin32Error(GetLastError());
			}
			CloseHandle(file);
			return std::nullopt;
		}

		void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
		if (view == nullptr) {
			if (outError) {
				*outError = "MapViewOfFile failed: " + FormatWin32Error(GetLastError());
			}
			CloseHandle(mapping);
			CloseHandle(file);
			return std::nullopt;
		}

		MappedFileView result;
		result.m_file = file;
		result.m_mapping = mapping;
		result.m_view = view;
		result.m_size = static_cast<size_t>(size.QuadPart);
		TracyPlot("SARP.Texture.MMap.Bytes", static_cast<int64_t>(result.m_size));
		return result;
	}

	const void* Data() const noexcept { return m_view; }
	size_t Size() const noexcept { return m_size; }

private:
	void Reset()
	{
		if (m_view) {
			UnmapViewOfFile(m_view);
			m_view = nullptr;
		}
		if (m_mapping) {
			CloseHandle(m_mapping);
			m_mapping = nullptr;
		}
		if (m_file != INVALID_HANDLE_VALUE) {
			CloseHandle(m_file);
			m_file = INVALID_HANDLE_VALUE;
		}
		m_size = 0;
	}

	void MoveFrom(MappedFileView&& other) noexcept
	{
		m_file = other.m_file;
		m_mapping = other.m_mapping;
		m_view = other.m_view;
		m_size = other.m_size;
		other.m_file = INVALID_HANDLE_VALUE;
		other.m_mapping = nullptr;
		other.m_view = nullptr;
		other.m_size = 0;
	}

	HANDLE m_file = INVALID_HANDLE_VALUE;
	HANDLE m_mapping = nullptr;
	void* m_view = nullptr;
	size_t m_size = 0;
};

const char* ToString(TextureProcessingJobState state) {
	switch (state) {
	case TextureProcessingJobState::Queued:
		return "Queued";
	case TextureProcessingJobState::CpuPreparing:
		return "CpuPreparing";
	case TextureProcessingJobState::GpuReadyToSubmit:
		return "GpuReadyToSubmit";
	case TextureProcessingJobState::GpuSubmitted:
		return "GpuSubmitted";
	case TextureProcessingJobState::ReadbackPending:
		return "ReadbackPending";
	case TextureProcessingJobState::Ready:
		return "Ready";
	case TextureProcessingJobState::Failed:
		return "Failed";
	default:
		return "Unknown";
	}
}

const char* ToString(TextureDirectStorageReloadJobState state) {
	switch (state) {
	case TextureDirectStorageReloadJobState::Queued:
		return "Queued";
	case TextureDirectStorageReloadJobState::CreatingResource:
		return "CreatingResource";
	case TextureDirectStorageReloadJobState::Uploading:
		return "Uploading";
	case TextureDirectStorageReloadJobState::Ready:
		return "Ready";
	case TextureDirectStorageReloadJobState::Failed:
		return "Failed";
	default:
		return "Unknown";
	}
}

std::string TextureTelemetryLabel(const TextureAsset& texture) {
	if (!texture.Meta().filePath.empty()) {
		return texture.Meta().filePath;
	}
	if (!texture.Meta().processing.sourceIdentity.empty()) {
		return texture.Meta().processing.sourceIdentity + "|semantic:" + ToString(texture.Meta().processing.semantic);
	}
	return texture.GetWidth() && texture.GetHeight()
		? std::to_string(texture.GetWidth()) + "x" + std::to_string(texture.GetHeight())
		: std::string("unnamed_texture");
}

uint32_t CalcFullMipCount(uint32_t width, uint32_t height) {
	uint32_t levels = 1;
	while (width > 1 || height > 1) {
		width = (std::max)(1u, width >> 1);
		height = (std::max)(1u, height >> 1);
		++levels;
	}
	return levels;
}

uint32_t CalcMipCountFromDescription(const org::TextureDescription& desc) {
	if (desc.imageDimensions.empty()) {
		return 1u;
	}

	const uint32_t faces = desc.isCubemap ? 6u : 1u;
	const uint32_t slices = faces * (std::max)(1u, desc.arraySize);
	if (slices == 0u) {
		return 1u;
	}

	return static_cast<uint32_t>((std::max)(size_t(1), desc.imageDimensions.size() / slices));
}

bool IsDDSFilePath(const std::string& path) {
	if (path.empty()) {
		return false;
	}

	std::wstring extension = std::filesystem::path(path).extension().wstring();
	std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
	return extension == L".dds";
}

bool IsConditionedCacheFilePath(const std::string& path) {
	if (path.empty()) {
		return false;
	}

	std::wstring extension = std::filesystem::path(path).extension().wstring();
	std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
	return br::processed_texture_cache::IsConditionedCacheExtension(extension);
}

enum class ConditionedCacheResidencyClass : uint8_t {
	EligibleGpuDirectFullOrWindow,
	EligibleGpuDirectFullOnly,
	GpuQueueUnavailable,
	InvalidCache,
	UnsupportedStreamingWindow,
	ResourceCreateOrEnqueueFailed,
};

const char* ToString(ConditionedCacheResidencyClass value) {
	switch (value) {
	case ConditionedCacheResidencyClass::EligibleGpuDirectFullOrWindow:
		return "EligibleGpuDirectFullOrWindow";
	case ConditionedCacheResidencyClass::EligibleGpuDirectFullOnly:
		return "EligibleGpuDirectFullOnly";
	case ConditionedCacheResidencyClass::GpuQueueUnavailable:
		return "GpuQueueUnavailable";
	case ConditionedCacheResidencyClass::InvalidCache:
		return "InvalidCache";
	case ConditionedCacheResidencyClass::UnsupportedStreamingWindow:
		return "UnsupportedStreamingWindow";
	case ConditionedCacheResidencyClass::ResourceCreateOrEnqueueFailed:
		return "ResourceCreateOrEnqueueFailed";
	default:
		return "Unknown";
	}
}

bool ReadProcessedTextureCacheHeader(const std::wstring& filePath, br::processed_texture_cache::FileHeader& header, std::string* outError = nullptr) {
	if (outError) {
		outError->clear();
	}

	std::ifstream file(filePath, std::ios::binary);
	if (!file) {
		if (outError) {
			*outError = "failed to open conditioned texture cache file";
		}
		return false;
	}

	file.read(reinterpret_cast<char*>(&header), sizeof(header));
	if (!file || static_cast<size_t>(file.gcount()) != sizeof(header)) {
		if (outError) {
			*outError = "failed to read conditioned texture cache header";
		}
		return false;
	}

	if (header.magic != br::processed_texture_cache::kMagic ||
		header.version != br::processed_texture_cache::kVersion ||
		header.headerSize != sizeof(header) ||
		header.dataOffset < sizeof(header) ||
		header.dataSizeBytes == 0) {
		if (outError) {
			*outError = "conditioned texture cache header is invalid";
		}
		return false;
	}

	return true;
}

bool BuildProcessedTextureCacheLayouts(
	const br::processed_texture_cache::FileHeader& header,
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& layouts,
	std::vector<UINT>& numRows,
	UINT64& totalBytes,
	std::string* outError = nullptr)
{
	if (outError) {
		outError->clear();
	}

	if (header.baseWidth == 0 || header.baseHeight == 0 || header.mipLevels == 0 ||
		header.totalArraySlices == 0 || header.subresourceCount == 0 ||
		header.subresourceCount != header.totalArraySlices * header.mipLevels) {
		if (outError) {
			*outError = "conditioned texture cache header has inconsistent dimensions";
		}
		return false;
	}

	// The conditioned cache format intentionally uses D3D12 copyable
	// footprints. In Vulkan-primary multi-RHI mode the canonical D3D12 device is
	// the peer, not GetDevice(), so resolve it explicitly by API.
	auto* nativeDevice = rhi::dx12::get_device(
		DeviceManager::GetInstance().GetDevice(rhi::Backend::D3D12));
	if (nativeDevice == nullptr) {
		// The on-disk cache uses D3D12's documented copyable-footprint
		// alignment, but parsing it must not require a live D3D12 device when
		// Vulkan is the sole backend. Reconstruct the deterministic 2D footprint
		// layout (512-byte subresource offsets, 256-byte row pitches).
		const auto format = static_cast<rhi::Format>(header.format);
		const auto block = rhi::GetBlockInfo(format);
		if (block.bytesPerBlock == 0) {
			if (outError) *outError = "conditioned texture cache format has no portable block layout";
			return false;
		}
		auto alignUp = [](UINT64 value, UINT64 alignment) {
			return (value + alignment - 1u) & ~(alignment - 1u);
		};
		layouts.resize(header.subresourceCount);
		numRows.resize(header.subresourceCount);
		totalBytes = 0;
		for (uint32_t subresource = 0; subresource < header.subresourceCount; ++subresource) {
			const uint32_t mip = subresource % header.mipLevels;
			const uint32_t width = (std::max)(1u, header.baseWidth >> mip);
			const uint32_t height = (std::max)(1u, header.baseHeight >> mip);
			const uint32_t blocksWide = (std::max)(1u, (width + block.blockWidth - 1u) / block.blockWidth);
			const uint32_t rows = (std::max)(1u, (height + block.blockHeight - 1u) / block.blockHeight);
			const UINT64 rowBytes = static_cast<UINT64>(blocksWide) * block.bytesPerBlock;
			const UINT rowPitch = static_cast<UINT>(alignUp(rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
			const UINT64 offset = alignUp(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
			auto& layout = layouts[subresource];
			layout.Offset = offset;
			layout.Footprint.Format = rhi::ToDxgi(format);
			layout.Footprint.Width = width;
			layout.Footprint.Height = height;
			layout.Footprint.Depth = 1;
			layout.Footprint.RowPitch = rowPitch;
			numRows[subresource] = rows;
			totalBytes = offset + static_cast<UINT64>(rowPitch) * (rows - 1u) + rowBytes;
		}
		if (totalBytes == 0 || totalBytes > header.dataSizeBytes) {
			if (outError) *outError = "portable conditioned texture cache footprints exceed payload";
			return false;
		}
		return true;
	}

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Width = header.baseWidth;
	resourceDesc.Height = header.baseHeight;
	resourceDesc.DepthOrArraySize = static_cast<uint16_t>(header.totalArraySlices);
	resourceDesc.MipLevels = static_cast<uint16_t>(header.mipLevels);
	resourceDesc.Format = rhi::ToDxgi(static_cast<rhi::Format>(header.format));
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	layouts.resize(header.subresourceCount);
	numRows.resize(header.subresourceCount);
	std::vector<UINT64> rowSizes(header.subresourceCount);
	totalBytes = 0;
	nativeDevice->GetCopyableFootprints(
		&resourceDesc,
		0,
		header.subresourceCount,
		0,
		layouts.data(),
		numRows.data(),
		rowSizes.data(),
		&totalBytes);

	if (totalBytes == 0 || totalBytes > header.dataSizeBytes) {
		if (outError) {
			*outError = "conditioned texture cache payload size did not match computed D3D12 copyable footprints";
		}
		return false;
	}

	return true;
}

struct ConditionedCacheResidentUploadMetadata {
	std::wstring widePath;
	br::processed_texture_cache::FileHeader header{};
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
	std::vector<UINT> numRows;
	UINT64 totalBytes = 0;
};

struct ConditionedCacheResidentUploadMetadataCache {
	std::mutex mutex;
	std::unordered_map<std::string, std::shared_ptr<const ConditionedCacheResidentUploadMetadata>> byPath;
};

ConditionedCacheResidentUploadMetadataCache& GetConditionedCacheResidentUploadMetadataCache()
{
	static ConditionedCacheResidentUploadMetadataCache cache;
	return cache;
}

std::shared_ptr<const ConditionedCacheResidentUploadMetadata> GetConditionedCacheResidentUploadMetadata(
	const std::string& path,
	std::string& outError)
{
	ZoneScopedN("TextureAsset::GetConditionedCacheResidentUploadMetadata");
	outError.clear();
	if (path.empty()) {
		outError = "empty conditioned cache path";
		return {};
	}

	{
		ZoneScopedN("TextureAsset::GetConditionedCacheResidentUploadMetadata::Lookup");
		auto& cache = GetConditionedCacheResidentUploadMetadataCache();
		std::lock_guard lock(cache.mutex);
		if (auto it = cache.byPath.find(path); it != cache.byPath.end() && it->second) {
			return it->second;
		}
	}

	auto metadata = std::make_shared<ConditionedCacheResidentUploadMetadata>();
	metadata->widePath = std::filesystem::path(path).wstring();
	{
		ZoneScopedN("TextureAsset::GetConditionedCacheResidentUploadMetadata::ReadHeader");
		if (!ReadProcessedTextureCacheHeader(metadata->widePath, metadata->header, &outError)) {
			return {};
		}
	}
	{
		ZoneScopedN("TextureAsset::GetConditionedCacheResidentUploadMetadata::BuildLayouts");
		if (!BuildProcessedTextureCacheLayouts(metadata->header, metadata->layouts, metadata->numRows, metadata->totalBytes, &outError)) {
			return {};
		}
	}

	{
		ZoneScopedN("TextureAsset::GetConditionedCacheResidentUploadMetadata::Insert");
		auto& cache = GetConditionedCacheResidentUploadMetadataCache();
		std::lock_guard lock(cache.mutex);
		auto [it, inserted] = cache.byPath.emplace(path, metadata);
		if (!inserted && it->second) {
			return it->second;
		}
	}
	return metadata;
}

std::shared_ptr<TextureSourceData> BuildSourceDataFromConditionedCacheFilePath(const std::string& path, const std::string& reason) {
	ZoneScopedN("TextureAsset::BuildSourceDataFromConditionedCacheFilePath");
	ZoneText(path.data(), path.size());
	if (!reason.empty()) {
		ZoneText(reason.data(), reason.size());
	}
	LogSourceDataBuildAttribution("conditioned_cache", path, reason);
	const std::wstring widePath = std::filesystem::path(path).wstring();
	br::processed_texture_cache::FileHeader header{};
	std::string error;
	if (!ReadProcessedTextureCacheHeader(widePath, header, &error)) {
		throw std::runtime_error(error.empty() ? "failed to read conditioned texture cache header" : error);
	}

	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
	std::vector<UINT> numRows;
	UINT64 totalBytes = 0;
	if (!BuildProcessedTextureCacheLayouts(header, layouts, numRows, totalBytes, &error)) {
		throw std::runtime_error(error.empty() ? "failed to compute conditioned texture cache layout" : error);
	}

	std::string mapError;
	auto mapped = MappedFileView::Open(widePath, &mapError);
	if (!mapped) {
		WarnOnce(
			"texture-conditioned-mmap|" + path + "|" + mapError,
			"TextureAsset: memory-mapped conditioned cache read failed for '" + path + "' because " + mapError + "; falling back to std::ifstream");
	}
	std::vector<uint8_t> payload;
	const uint8_t* payloadBase = nullptr;
	size_t payloadSize = 0;
	if (mapped) {
		if (header.dataOffset > mapped->Size() || header.dataSizeBytes > mapped->Size() - static_cast<size_t>(header.dataOffset)) {
			throw std::runtime_error("conditioned texture cache mapped file ended before payload");
		}
		payloadBase = static_cast<const uint8_t*>(mapped->Data()) + static_cast<size_t>(header.dataOffset);
		payloadSize = static_cast<size_t>(header.dataSizeBytes);
	}
	else {
		std::ifstream file(widePath, std::ios::binary);
		if (!file) {
			throw std::runtime_error("failed to open conditioned texture cache payload");
		}
		file.seekg(static_cast<std::streamoff>(header.dataOffset), std::ios::beg);
		payload.resize(static_cast<size_t>(header.dataSizeBytes), 0u);
		file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
		if (!file || static_cast<size_t>(file.gcount()) != payload.size()) {
			throw std::runtime_error("failed to read conditioned texture cache payload");
		}
		payloadBase = payload.data();
		payloadSize = payload.size();
	}

	auto result = std::make_shared<TextureSourceData>();
	result->desc.format = static_cast<rhi::Format>(header.format);
	result->desc.channels = static_cast<unsigned short>(header.channels);
	result->desc.isCubemap = br::processed_texture_cache::HasFlag(header, br::processed_texture_cache::FlagIsCubemap);
	result->desc.isArray = br::processed_texture_cache::HasFlag(header, br::processed_texture_cache::FlagIsArray);
	result->desc.arraySize = (std::max)(1u, header.arraySize);
	result->desc.generateMipMaps = false;
	result->desc.imageDimensions.reserve(header.subresourceCount);
	result->subresources.reserve(header.subresourceCount);
	const DXGI_FORMAT dxgiFormat = rhi::ToDxgi(result->desc.format);

	for (uint32_t subresourceIndex = 0; subresourceIndex < header.subresourceCount; ++subresourceIndex) {
		const auto& layout = layouts[subresourceIndex];
		const uint32_t mipIndex = subresourceIndex % header.mipLevels;
		const size_t mipWidth = (std::max)(size_t(1), static_cast<size_t>(header.baseWidth) >> mipIndex);
		const size_t mipHeight = (std::max)(size_t(1), static_cast<size_t>(header.baseHeight) >> mipIndex);
		size_t rowPitch = 0;
		size_t slicePitch = 0;
		if (FAILED(DirectX::ComputePitch(dxgiFormat, mipWidth, mipHeight, rowPitch, slicePitch))) {
			throw std::runtime_error("failed to compute conditioned texture cache subresource pitch");
		}

		const size_t offset = static_cast<size_t>(layout.Offset);
		const size_t sourceRowPitch = static_cast<size_t>(layout.Footprint.RowPitch);
		const size_t rowCount = static_cast<size_t>(numRows[subresourceIndex]);
		const size_t sourceSpan = rowCount == 0u
			? 0u
			: sourceRowPitch * (rowCount - 1u) + rowPitch;
		if (offset > payloadSize || sourceSpan > payloadSize - offset) {
			throw std::runtime_error("conditioned texture cache payload ended before expected subresource data");
		}

		org::ImageDimensions dims{};
		dims.width = static_cast<uint32_t>(mipWidth);
		dims.height = static_cast<uint32_t>(mipHeight);
		dims.rowPitch = rowPitch;
		dims.slicePitch = slicePitch;
		result->desc.imageDimensions.push_back(dims);

		auto bytes = std::make_shared<std::vector<uint8_t>>(slicePitch, 0u);
		const uint8_t* srcBase = payloadBase + offset;
		uint8_t* dstBase = bytes->data();
		for (size_t row = 0; row < rowCount; ++row) {
			std::memcpy(
				dstBase + row * rowPitch,
				srcBase + row * sourceRowPitch,
				rowPitch);
		}
		result->subresources.push_back(std::move(bytes));
	}

	result->hasFullMipChain = br::processed_texture_cache::HasFlag(header, br::processed_texture_cache::FlagHasFullMipChain);
	result->isBlockCompressed = br::processed_texture_cache::HasFlag(header, br::processed_texture_cache::FlagIsBlockCompressed);
	return result;
}

bool TryBuildConditionedCacheResidentUpload(
	const std::string& path,
	uint32_t topMip,
	bool allowRTV,
	bool allowUAV,
	org::TextureDescription& outDesc,
	DirectStorageTextureSubresourceRangeCopy& outRange,
	uint32_t& outClampedTopMip,
	std::string& outError)
{
	ZoneScopedN("TextureAsset::TryBuildConditionedCacheResidentUpload");
	outError.clear();
	const auto metadata = GetConditionedCacheResidentUploadMetadata(path, outError);
	if (!metadata) {
		return false;
	}
	const br::processed_texture_cache::FileHeader& header = metadata->header;
	const auto& layouts = metadata->layouts;
	TracyPlot("SARP.Texture.ConditionedCache.Preflight.Width", static_cast<int64_t>(header.baseWidth));
	TracyPlot("SARP.Texture.ConditionedCache.Preflight.Height", static_cast<int64_t>(header.baseHeight));
	TracyPlot("SARP.Texture.ConditionedCache.Preflight.MipLevels", static_cast<int64_t>(header.mipLevels));

	const bool fullChainOnly = header.totalArraySlices > 1u;
	const ConditionedCacheResidencyClass residencyClass = fullChainOnly
		? ConditionedCacheResidencyClass::EligibleGpuDirectFullOnly
		: ConditionedCacheResidencyClass::EligibleGpuDirectFullOrWindow;
	TracyPlot("SARP.Texture.ConditionedCache.Preflight.FullChainOnly", static_cast<int64_t>(fullChainOnly ? 1 : 0));
	ZoneText(ToString(residencyClass), std::strlen(ToString(residencyClass)));

	outClampedTopMip = fullChainOnly
		? 0u
		: (std::min)(topMip, header.mipLevels - 1u);
	const uint32_t residentMipCount = header.mipLevels - outClampedTopMip;
	const uint32_t residentSubresourceCount = fullChainOnly
		? header.subresourceCount
		: residentMipCount;
	const auto& firstLayout = layouts[outClampedTopMip];
	if (firstLayout.Offset > header.dataSizeBytes || header.dataSizeBytes - firstLayout.Offset > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
		outError = "conditioned texture cache resident range exceeded DirectStorage upload limits";
		return false;
	}

	outDesc = {};
	outDesc.format = static_cast<rhi::Format>(header.format);
	outDesc.channels = static_cast<unsigned short>(header.channels);
	outDesc.isCubemap = br::processed_texture_cache::HasFlag(header, br::processed_texture_cache::FlagIsCubemap);
	outDesc.isArray = br::processed_texture_cache::HasFlag(header, br::processed_texture_cache::FlagIsArray);
	outDesc.arraySize = outDesc.isCubemap
		? (std::max)(1u, header.totalArraySlices / 6u)
		: (std::max)(1u, header.arraySize);
	outDesc.hasRTV = allowRTV;
	outDesc.hasUAV = allowUAV;
	outDesc.generateMipMaps = false;
	outDesc.initialLayout = rhi::ResourceLayout::Common;
	outDesc.imageDimensions.reserve(residentSubresourceCount);
	{
		ZoneScopedN("TextureAsset::TryBuildConditionedCacheResidentUpload::BuildResidentDimensions");
		for (uint32_t subresourceIndex = 0u; subresourceIndex < header.subresourceCount; ++subresourceIndex) {
			const uint32_t mipIndex = subresourceIndex % header.mipLevels;
			if (mipIndex < outClampedTopMip) {
				continue;
			}

			const size_t mipWidth = (std::max)(size_t(1), static_cast<size_t>(header.baseWidth) >> mipIndex);
			const size_t mipHeight = (std::max)(size_t(1), static_cast<size_t>(header.baseHeight) >> mipIndex);
			size_t rowPitch = 0;
			size_t slicePitch = 0;
			if (FAILED(DirectX::ComputePitch(rhi::ToDxgi(static_cast<rhi::Format>(header.format)), mipWidth, mipHeight, rowPitch, slicePitch))) {
				outError = "failed to compute conditioned texture cache resident pitch";
				return false;
			}

			org::ImageDimensions dims{};
			dims.width = static_cast<uint32_t>(mipWidth);
			dims.height = static_cast<uint32_t>(mipHeight);
			dims.rowPitch = rowPitch;
			dims.slicePitch = slicePitch;
			outDesc.imageDimensions.push_back(dims);
		}
	}

	outRange = {};
	outRange.sourceOffset = header.dataOffset + firstLayout.Offset;
	outRange.sourceSizeBytes = static_cast<uint32_t>(header.dataSizeBytes - firstLayout.Offset);
	outRange.uncompressedSizeBytes = outRange.sourceSizeBytes;
	outRange.firstSubresource = 0u;
	outRange.subresourceCount = residentSubresourceCount;

	return true;
}

uint32_t ComputeDefaultStreamingBootstrapTopMip(const org::TextureDescription& desc, uint32_t totalMipCount) {
	if (desc.imageDimensions.empty() || totalMipCount <= 1u) {
		return 0u;
	}

	// Start with only the final mip. For ordinary 2D material textures this produces
	// the smallest resource the API can allocate (normally one 64 KiB allocation),
	// and all finer residency must be justified by GPU streaming feedback.
	return totalMipCount - 1u;
}

std::shared_ptr<TextureSourceData> ClipTextureSourceDataTopMip(
	const std::shared_ptr<TextureSourceData>& sourceData,
	uint32_t topMip)
{
	if (!sourceData) {
		return {};
	}

	const uint32_t fullMipCount = CalcMipCountFromDescription(sourceData->desc);
	if (topMip == 0u || fullMipCount <= 1u) {
		return sourceData;
	}

	const uint32_t clampedTopMip = (std::min)(topMip, fullMipCount - 1u);
	if (clampedTopMip == 0u) {
		return sourceData;
	}

	const uint32_t faces = sourceData->desc.isCubemap ? 6u : 1u;
	const uint32_t slices = faces * (std::max)(1u, sourceData->desc.arraySize);
	const uint32_t residentMipCount = fullMipCount - clampedTopMip;

	auto clipped = std::make_shared<TextureSourceData>();
	clipped->desc = sourceData->desc;
	clipped->desc.imageDimensions.clear();
	clipped->desc.imageDimensions.reserve(static_cast<size_t>(slices) * residentMipCount);
	clipped->subresources.reserve(static_cast<size_t>(slices) * residentMipCount);

	for (uint32_t slice = 0u; slice < slices; ++slice) {
		const size_t sliceBase = static_cast<size_t>(slice) * fullMipCount;
		for (uint32_t mip = clampedTopMip; mip < fullMipCount; ++mip) {
			const size_t index = sliceBase + mip;
			clipped->desc.imageDimensions.push_back(sourceData->desc.imageDimensions[index]);
			clipped->subresources.push_back(sourceData->subresources[index]);
		}
	}

	clipped->isBlockCompressed = sourceData->isBlockCompressed;
	clipped->hasFullMipChain = false;
	return clipped;
}

uint32_t InferTextureSourceTopMip(
	const org::TextureDescription& desc,
	uint32_t fullWidth,
	uint32_t fullHeight,
	uint32_t totalMipCount)
{
	if (desc.imageDimensions.empty() || fullWidth == 0u || fullHeight == 0u || totalMipCount == 0u) {
		return 0u;
	}

	const org::ImageDimensions& firstMip = desc.imageDimensions.front();
	for (uint32_t mip = 0u; mip < totalMipCount; ++mip) {
		const uint32_t expectedWidth = (std::max)(1u, fullWidth >> mip);
		const uint32_t expectedHeight = (std::max)(1u, fullHeight >> mip);
		if (firstMip.width == expectedWidth && firstMip.height == expectedHeight) {
			return mip;
		}
	}
	return 0u;
}

std::shared_ptr<TextureSourceData> ShapeTextureSourceDataForResidentTopMip(
	const std::shared_ptr<TextureSourceData>& sourceData,
	uint32_t fullWidth,
	uint32_t fullHeight,
	uint32_t totalMipCount,
	uint32_t desiredResidentTopMip)
{
	if (!sourceData || sourceData->desc.imageDimensions.empty() || desiredResidentTopMip == 0u) {
		return sourceData;
	}

	const uint32_t sourceTopMip = InferTextureSourceTopMip(
		sourceData->desc,
		fullWidth,
		fullHeight,
		totalMipCount);
	if (sourceTopMip >= desiredResidentTopMip) {
		return sourceData;
	}

	const uint32_t relativeTopMip = desiredResidentTopMip - sourceTopMip;
	const uint32_t sourceMipCount = CalcMipCountFromDescription(sourceData->desc);
	if (relativeTopMip >= sourceMipCount) {
		// The supplied source does not contain the requested coarse mip. Preserve
		// the usable source and report its actual residency so a later reload can
		// obtain a complete source instead of publishing false residency metadata.
		return sourceData;
	}
	return ClipTextureSourceDataTopMip(sourceData, relativeTopMip);
}

// Block-compressed 2D DDS payloads are stored exactly as the renderer consumes
// them: item-major, then mip, tightly packed, and DirectXTex applies no
// conversion to them. Copy each subresource straight from the mapped file
// instead of decoding into a zero-filled ScratchImage and copying it again.
// Returns null for anything DirectXTex might convert or that fails validation.
std::shared_ptr<TextureSourceData> TryBuildSourceDataFromBlockCompressedDDS(
	const uint8_t* bytes, size_t size, bool preferSRGB)
{
	constexpr size_t kHeaderBytes = 4u + 124u;
	constexpr size_t kDX10HeaderBytes = 20u;
	constexpr size_t kFourCCOffset = 4u + 72u + 8u;
	constexpr uint32_t kDX10FourCC = uint32_t('D') | (uint32_t('X') << 8) | (uint32_t('1') << 16) | (uint32_t('0') << 24);
	if (size < kHeaderBytes) return nullptr;
	DirectX::TexMetadata metadata{};
	if (FAILED(DirectX::GetMetadataFromDDSMemory(bytes, size, DirectX::DDS_FLAGS_NONE, metadata))) return nullptr;
	if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.depth != 1 ||
		!DirectX::IsCompressed(metadata.format) || metadata.mipLevels == 0 || metadata.arraySize == 0) {
		return nullptr;
	}
	uint32_t fourCC = 0;
	std::memcpy(&fourCC, bytes + kFourCCOffset, sizeof(fourCC));
	size_t offset = kHeaderBytes + (fourCC == kDX10FourCC ? kDX10HeaderBytes : 0u);

	auto result = std::make_shared<TextureSourceData>();
	const size_t imageCount = metadata.arraySize * metadata.mipLevels;
	result->desc.imageDimensions.reserve(imageCount);
	result->subresources.reserve(imageCount);
	for (size_t item = 0; item < metadata.arraySize; ++item) {
		size_t width = metadata.width;
		size_t height = metadata.height;
		for (size_t mip = 0; mip < metadata.mipLevels; ++mip) {
			size_t rowPitch = 0;
			size_t slicePitch = 0;
			if (FAILED(DirectX::ComputePitch(metadata.format, width, height, rowPitch, slicePitch)) ||
				slicePitch > size - offset) {
				return nullptr;
			}
			org::ImageDimensions dims{};
			dims.width = static_cast<uint32_t>(width);
			dims.height = static_cast<uint32_t>(height);
			dims.rowPitch = rowPitch;
			dims.slicePitch = slicePitch;
			result->desc.imageDimensions.push_back(dims);
			result->subresources.push_back(std::make_shared<std::vector<uint8_t>>(bytes + offset, bytes + offset + slicePitch));
			offset += slicePitch;
			width = (std::max)(size_t(1), width / 2u);
			height = (std::max)(size_t(1), height / 2u);
		}
	}

	result->desc.format = rhi::helpers::ToRHI(preferSRGB ? DirectX::MakeSRGB(metadata.format) : DirectX::MakeLinear(metadata.format));
	result->desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(result->desc.format));
	result->desc.isCubemap = metadata.IsCubemap();
	result->desc.isArray = metadata.arraySize > 1 && !result->desc.isCubemap;
	result->desc.arraySize = result->desc.isCubemap
		? static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize / size_t(6)))
		: static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));
	result->isBlockCompressed = rhi::helpers::IsBlockCompressed(result->desc.format);
	result->hasFullMipChain = metadata.mipLevels == CalcFullMipCount(
		result->desc.imageDimensions[0].width,
		result->desc.imageDimensions[0].height);
	return result;
}

std::shared_ptr<TextureSourceData> BuildSourceDataFromDDSFilePath(const std::string& path, bool preferSRGB, const std::string& reason) {
	ZoneScopedN("TextureAsset::BuildSourceDataFromDDSFilePath");
	ZoneText(path.data(), path.size());
	if (!reason.empty()) {
		ZoneText(reason.data(), reason.size());
	}
	LogSourceDataBuildAttribution("dds", path, reason);
	DirectX::ScratchImage image;
	DirectX::TexMetadata metadata{};
	const std::wstring widePath = std::filesystem::path(path).wstring();
	HRESULT hr = E_FAIL;
	std::string mapError;
	if (auto mapped = MappedFileView::Open(widePath, &mapError)) {
		if (auto direct = TryBuildSourceDataFromBlockCompressedDDS(
				static_cast<const uint8_t*>(mapped->Data()), mapped->Size(), preferSRGB)) {
			return direct;
		}
		ZoneScopedN("TextureAsset::BuildSourceDataFromDDSFilePath::LoadFromMappedMemory");
		hr = DirectX::LoadFromDDSMemory(
			static_cast<const uint8_t*>(mapped->Data()),
			mapped->Size(),
			DirectX::DDS_FLAGS_NONE,
			&metadata,
			image);
	}
	else {
		WarnOnce(
			"texture-dds-mmap|" + path + "|" + mapError,
			"TextureAsset: memory-mapped DDS read failed for '" + path + "' because " + mapError + "; falling back to DirectXTex file load");
		ZoneScopedN("TextureAsset::BuildSourceDataFromDDSFilePath::LoadFromDDSFile");
		hr = DirectX::LoadFromDDSFile(widePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
	}
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to load DDS from file path: " + path);
	}

	auto result = std::make_shared<TextureSourceData>();
	result->desc.format = rhi::helpers::ToRHI(preferSRGB ? DirectX::MakeSRGB(metadata.format) : DirectX::MakeLinear(metadata.format));
	result->desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(result->desc.format));
	result->desc.isCubemap = metadata.IsCubemap();
	result->desc.isArray = metadata.arraySize > 1 && !result->desc.isCubemap;
	result->desc.arraySize = result->desc.isCubemap
		? static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize / size_t(6)))
		: static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));

	result->desc.imageDimensions.reserve(image.GetImageCount());
	result->subresources.reserve(image.GetImageCount());

	const DirectX::Image* images = image.GetImages();
	if (!images || image.GetImageCount() == 0) {
		throw std::runtime_error("DDS file did not produce any images: " + path);
	}

	for (size_t imageIndex = 0; imageIndex < image.GetImageCount(); ++imageIndex) {
		const DirectX::Image& src = images[imageIndex];

		org::ImageDimensions dims{};
		dims.width = static_cast<uint32_t>(src.width);
		dims.height = static_cast<uint32_t>(src.height);
		dims.rowPitch = src.rowPitch;
		dims.slicePitch = src.slicePitch;
		result->desc.imageDimensions.push_back(dims);

		const auto* first = reinterpret_cast<const uint8_t*>(src.pixels);
		result->subresources.push_back(std::make_shared<std::vector<uint8_t>>(first, first + src.slicePitch));
	}

	result->isBlockCompressed = rhi::helpers::IsBlockCompressed(result->desc.format);
	result->hasFullMipChain = metadata.mipLevels == CalcFullMipCount(
		result->desc.imageDimensions[0].width,
		result->desc.imageDimensions[0].height);
	return result;
}

std::shared_ptr<TextureSourceData> BuildSourceDataFromWICFilePath(const std::string& path, bool preferSRGB, const std::string& reason) {
	ZoneScopedN("TextureAsset::BuildSourceDataFromWICFilePath");
	ZoneText(path.data(), path.size());
	if (!reason.empty()) {
		ZoneText(reason.data(), reason.size());
	}
	LogSourceDataBuildAttribution("wic", path, reason);

	DirectX::ScratchImage image;
	DirectX::TexMetadata metadata{};
	const std::wstring widePath = std::filesystem::path(path).wstring();
	const auto flags = DirectX::WIC_FLAGS_FORCE_RGB |
		(preferSRGB ? DirectX::WIC_FLAGS_FORCE_SRGB : DirectX::WIC_FLAGS_FORCE_LINEAR);
	HRESULT hr = E_FAIL;
	std::string mapError;
	if (auto mapped = MappedFileView::Open(widePath, &mapError)) {
		ZoneScopedN("TextureAsset::BuildSourceDataFromWICFilePath::LoadFromMappedMemory");
		hr = DirectX::LoadFromWICMemory(
			static_cast<const uint8_t*>(mapped->Data()),
			mapped->Size(),
			flags,
			&metadata,
			image);
	}
	else {
		WarnOnce(
			"texture-wic-mmap|" + path + "|" + mapError,
			"TextureAsset: memory-mapped WIC read failed for '" + path + "' because " + mapError + "; falling back to DirectXTex file load");
		ZoneScopedN("TextureAsset::BuildSourceDataFromWICFilePath::LoadFromWICFile");
		hr = DirectX::LoadFromWICFile(widePath.c_str(), flags, &metadata, image);
	}
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to load WIC image from file path: " + path);
	}

	auto result = std::make_shared<TextureSourceData>();
	result->desc.format = rhi::helpers::ToRHI(preferSRGB ? DirectX::MakeSRGB(metadata.format) : DirectX::MakeLinear(metadata.format));
	result->desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(result->desc.format));
	result->desc.isCubemap = metadata.IsCubemap();
	result->desc.isArray = metadata.arraySize > 1 && !result->desc.isCubemap;
	result->desc.arraySize = result->desc.isCubemap
		? static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize / size_t(6)))
		: static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));

	result->desc.imageDimensions.reserve(image.GetImageCount());
	result->subresources.reserve(image.GetImageCount());
	const DirectX::Image* images = image.GetImages();
	if (!images || image.GetImageCount() == 0) {
		throw std::runtime_error("WIC file did not produce any images: " + path);
	}

	for (size_t imageIndex = 0; imageIndex < image.GetImageCount(); ++imageIndex) {
		const DirectX::Image& src = images[imageIndex];
		org::ImageDimensions dims{};
		dims.width = static_cast<uint32_t>(src.width);
		dims.height = static_cast<uint32_t>(src.height);
		dims.rowPitch = src.rowPitch;
		dims.slicePitch = src.slicePitch;
		result->desc.imageDimensions.push_back(dims);

		const auto* first = reinterpret_cast<const uint8_t*>(src.pixels);
		result->subresources.push_back(std::make_shared<std::vector<uint8_t>>(first, first + src.slicePitch));
	}

	result->isBlockCompressed = rhi::helpers::IsBlockCompressed(result->desc.format);
	result->hasFullMipChain = metadata.mipLevels == CalcFullMipCount(
		result->desc.imageDimensions[0].width,
		result->desc.imageDimensions[0].height);
	return result;
}

std::shared_ptr<TextureSourceData> BuildSourceDataFromTextureFilePath(
	const std::string& path,
	bool preferSRGB,
	const std::string& reason)
{
	if (IsConditionedCacheFilePath(path)) {
		return BuildSourceDataFromConditionedCacheFilePath(path, reason);
	}
	if (IsDDSFilePath(path)) {
		return BuildSourceDataFromDDSFilePath(path, preferSRGB, reason);
	}
	return BuildSourceDataFromWICFilePath(path, preferSRGB, reason);
}

std::shared_ptr<TextureDirectStorageReloadJobHandle> BeginUploadConditionedCacheFilePathDirectToVRAMAsync(
	const std::string& path,
	uint32_t topMip,
	bool allowRTV,
	bool allowUAV)
{
	ZoneScopedN("TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync");
	if (path.empty()) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidRequest, path, "empty conditioned cache path");
		return {};
	}
	if (IsDirectStorageGpuTextureUploadDisabled()) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::Disabled, path, "texture upload disabled by environment");
		return {};
	}
	if (!DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::Gpu)) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::QueueUnavailable, path, "GPU queue unavailable");
		return {};
	}

	auto handle = std::make_shared<TextureDirectStorageReloadJobHandle>();
	handle->targetTopMip.store(topMip, std::memory_order_release);
	handle->state.store(TextureDirectStorageReloadJobState::Queued, std::memory_order_release);

	TaskSchedulerManager::GetInstance().Submit(TaskLane::Streaming, TaskDomain::TextureProcessing, "TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync", [handle, path, topMip, allowRTV, allowUAV]() mutable {
		ZoneScopedN("TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync::IoTask");
		if (handle->cancelRequested.load(std::memory_order_acquire)) {
			handle->state.store(TextureDirectStorageReloadJobState::Failed, std::memory_order_release);
			return;
		}
		handle->state.store(TextureDirectStorageReloadJobState::CreatingResource, std::memory_order_release);

		try {
			org::TextureDescription desc{};
			DirectStorageTextureSubresourceRangeCopy range{};
			uint32_t clampedTopMip = 0u;
			std::string preflightError;
			{
				ZoneScopedN("TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync::BuildResidentUpload");
				if (!TryBuildConditionedCacheResidentUpload(
						path,
						topMip,
						allowRTV,
						allowUAV,
						desc,
						range,
						clampedTopMip,
						preflightError)) {
					RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidLayout, path, preflightError);
					if (!preflightError.empty()) {
						spdlog::debug("TextureAsset: conditioned cache DirectStorage preflight fallback for '{}' because {}", path, preflightError);
					}
					throw std::runtime_error(preflightError.empty()
						? "conditioned texture cache DirectStorage preflight failed"
						: preflightError);
				}
			}

			if (handle->cancelRequested.load(std::memory_order_acquire)) {
				throw std::runtime_error("conditioned texture cache DirectStorage upload was canceled before resource creation");
			}

			std::shared_ptr<org::PixelBuffer> uploadedImage;
			{
				ZoneScopedN("TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync::CreatePixelBuffer");
				uploadedImage = org::PixelBuffer::CreateShared(desc);
			}
			if (!uploadedImage) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::ResourceCreateFailed, path, "failed to create destination PixelBuffer");
				throw std::runtime_error("failed to create resident PixelBuffer for conditioned texture cache DirectStorage upload");
			}
			org::memory::SetResourceUsageHint(*uploadedImage, "Streamed textures (DirectStorage pending)");
			org::memory::SetResourceMemoryIdentifier(*uploadedImage, path);
			if (handle->cancelRequested.load(std::memory_order_acquire)) {
				throw std::runtime_error("conditioned texture cache DirectStorage upload was canceled before enqueue");
			}

			std::string directStorageMessage;
			DirectStorageAsyncRequestHandle requestHandle;
			{
				ZoneScopedN("TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync::EnqueueDirectStorage");
				requestHandle = DirectStorageManager::GetInstance().EnqueueUploadTextureSubresourceRangeFromFile(
					std::filesystem::path(path).wstring(),
					uploadedImage->GetAPIResource(),
					range,
					&directStorageMessage);
			}
			if (!requestHandle.IsValid()) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::EnqueueFailed, path, directStorageMessage);
				throw std::runtime_error(directStorageMessage.empty()
					? "failed to enqueue conditioned texture cache DirectStorage upload"
					: directStorageMessage);
			}

			{
				std::scoped_lock lock(handle->mutex);
				handle->targetTopMip.store(clampedTopMip, std::memory_order_release);
				handle->uploadedImage = std::move(uploadedImage);
				handle->requestHandle = std::move(requestHandle);
				handle->error.clear();
			}

			handle->state.store(TextureDirectStorageReloadJobState::Uploading, std::memory_order_release);
			RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::Eligible, path);
		}
		catch (const std::exception& ex) {
			{
				std::scoped_lock lock(handle->mutex);
				handle->error = ex.what();
			}
			handle->state.store(TextureDirectStorageReloadJobState::Failed, std::memory_order_release);
		}
	});

	return handle;
}
}

std::shared_ptr<TextureSourceData> LoadTextureSourceDataFromConditionedCacheFilePath(
	const std::string& path,
	const std::string& reason)
{
	return BuildSourceDataFromConditionedCacheFilePath(path, reason);
}

std::shared_ptr<TextureSourceData> LoadTextureSourceDataFromDDSFilePath(
	const std::string& path,
	bool preferSRGB,
	const std::string& reason)
{
	return BuildSourceDataFromDDSFilePath(path, preferSRGB, reason);
}

std::shared_ptr<TextureSourceData> LoadTextureSourceDataFromFilePath(
	const std::string& path,
	bool preferSRGB,
	const std::string& reason)
{
	return BuildSourceDataFromTextureFilePath(path, preferSRGB, reason);
}

std::shared_ptr<TextureSourceData> LoadTextureSourceDataFromImageBytes(
	const std::uint8_t* bytes,
	std::size_t byteCount,
	bool preferSRGB,
	std::string_view formatHint,
	const std::string& label)
{
	ZoneScopedN("TextureAsset::LoadTextureSourceDataFromImageBytes");
	if (!bytes || byteCount == 0) throw std::runtime_error("empty image bytes: " + label);
	std::string hint(formatHint);
	std::ranges::transform(hint, hint.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const auto starts = [&](std::string_view magic) {
		return byteCount >= magic.size() && std::memcmp(bytes, magic.data(), magic.size()) == 0;
	};

	DirectX::ScratchImage image;
	DirectX::TexMetadata metadata{};
	HRESULT hr = E_FAIL;
	bool colorFormat = true;
	if (starts("DDS ")) {
		hr = DirectX::LoadFromDDSMemory(bytes, byteCount, DirectX::DDS_FLAGS_NONE, &metadata, image);
	}
	else if (starts("#?")) {
		hr = DirectX::LoadFromHDRMemory(bytes, byteCount, &metadata, image);
		colorFormat = false;  // HDR stays linear float
	}
	else if (hint.find("tga") != std::string::npos) {
		hr = DirectX::LoadFromTGAMemory(bytes, byteCount, DirectX::TGA_FLAGS_NONE, &metadata, image);
	}
	else {
		const auto flags = DirectX::WIC_FLAGS_FORCE_RGB |
			(preferSRGB ? DirectX::WIC_FLAGS_FORCE_SRGB : DirectX::WIC_FLAGS_FORCE_LINEAR);
		hr = DirectX::LoadFromWICMemory(bytes, byteCount, flags, &metadata, image);
	}
	if (FAILED(hr)) throw std::runtime_error("failed to decode image bytes: " + label);

	auto result = std::make_shared<TextureSourceData>();
	const DXGI_FORMAT format = !colorFormat ? metadata.format
		: preferSRGB ? DirectX::MakeSRGB(metadata.format) : DirectX::MakeLinear(metadata.format);
	result->desc.format = rhi::helpers::ToRHI(format);
	result->desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(result->desc.format));
	result->desc.isCubemap = metadata.IsCubemap();
	result->desc.isArray = metadata.arraySize > 1 && !result->desc.isCubemap;
	result->desc.arraySize = result->desc.isCubemap
		? static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize / size_t(6)))
		: static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));
	const DirectX::Image* images = image.GetImages();
	if (!images || image.GetImageCount() == 0) throw std::runtime_error("image bytes produced no images: " + label);
	result->desc.imageDimensions.reserve(image.GetImageCount());
	result->subresources.reserve(image.GetImageCount());
	for (size_t imageIndex = 0; imageIndex < image.GetImageCount(); ++imageIndex) {
		const DirectX::Image& src = images[imageIndex];
		org::ImageDimensions dims{};
		dims.width = static_cast<uint32_t>(src.width);
		dims.height = static_cast<uint32_t>(src.height);
		dims.rowPitch = src.rowPitch;
		dims.slicePitch = src.slicePitch;
		result->desc.imageDimensions.push_back(dims);
		const auto* first = reinterpret_cast<const uint8_t*>(src.pixels);
		result->subresources.push_back(std::make_shared<std::vector<uint8_t>>(first, first + src.slicePitch));
	}
	result->isBlockCompressed = rhi::helpers::IsBlockCompressed(result->desc.format);
	result->hasFullMipChain = metadata.mipLevels == CalcFullMipCount(
		result->desc.imageDimensions[0].width,
		result->desc.imageDimensions[0].height);
	return result;
}

std::shared_ptr<TextureDirectStorageReloadJobHandle> br::textures::BeginCacheWindowUpload(
	const std::wstring& path, std::uint32_t topMip, bool allowRTV, bool allowUAV)
{
	return BeginUploadConditionedCacheFilePathDirectToVRAMAsync(std::filesystem::path(path).string(), topMip, allowRTV, allowUAV);
}

std::shared_ptr<TextureSourceData> br::textures::ShapeSourceForTopMip(
	const std::shared_ptr<TextureSourceData>& data, std::uint32_t topMip)
{
	if (!data || data->desc.imageDimensions.empty()) return data;
	return ShapeTextureSourceDataForResidentTopMip(
		data,
		data->desc.imageDimensions[0].width,
		data->desc.imageDimensions[0].height,
		CalcMipCountFromDescription(data->desc),
		topMip);
}

void TextureAsset::MarkTerminalLoadFailure(std::string_view reason) {
	if (m_terminalLoadFailure.exchange(true, std::memory_order_acq_rel)) return;
	spdlog::warn("TextureAsset: '{}' failed to load ({}); renderables waiting on it proceed with the placeholder",
		TextureTelemetryLabel(*this), reason);
}

uint32_t TextureAsset::NextStreamingTextureID() {
	static std::atomic<uint32_t> nextID{1u};
	return nextID.fetch_add(1u, std::memory_order_relaxed);
}

void TextureAsset::UpdateSourceShapeFromDescription(const org::TextureDescription& desc, uint32_t totalMipCountHint) {
	if (desc.imageDimensions.empty()) {
		return;
	}

	const uint32_t descMipCount = CalcMipCountFromDescription(desc);
	const uint32_t totalMipCount = (std::max)(descMipCount, totalMipCountHint);
	ApplySourceShapeHint(desc.imageDimensions[0].width, desc.imageDimensions[0].height, totalMipCount);
}

void TextureAsset::ApplySourceShapeHint(uint32_t fullWidth, uint32_t fullHeight, uint32_t totalMipCount) {
	if (fullWidth == 0u || fullHeight == 0u || totalMipCount == 0u) {
		return;
	}

	if (m_sourceFullWidth == 0u ||
		m_sourceFullHeight == 0u ||
		totalMipCount >= m_sourceTotalMipCount) {
		m_sourceFullWidth = fullWidth;
		m_sourceFullHeight = fullHeight;
	}

	m_sourceTotalMipCount = (std::max)(m_sourceTotalMipCount, totalMipCount);
}

void TextureAsset::RefreshStreamingStateFromDescription() {
	if (m_image && m_meta.processing.isParticipatingMaterialTexture) {
		org::memory::SetResourceUsageHint(*m_image, "Material textures");
		if (!m_meta.filePath.empty()) {
			org::memory::SetResourceMemoryIdentifier(*m_image, m_meta.filePath);
		}
	}
	else if (m_image && !m_meta.filePath.empty()) {
		std::string normalizedPath = m_meta.filePath;
		std::ranges::transform(normalizedPath, normalizedPath.begin(), [](unsigned char ch) {
			return ch == '\\' ? '/' : static_cast<char>(std::tolower(ch));
		});
		org::memory::SetResourceUsageHint(*m_image,
			normalizedPath.find("object_reyes_atlas_height") != std::string::npos
				? "Object Reyes height atlases"
				: "Non-material texture assets");
		org::memory::SetResourceMemoryIdentifier(*m_image, m_meta.filePath);
	}
	const bool wasEligible = m_streamingState.eligible;
	UpdateSourceShapeFromDescription(m_desc);
	const uint32_t descMipCount = CalcMipCountFromDescription(m_desc);
	const uint32_t totalMipCount = (std::max)(1u, m_sourceTotalMipCount);

	m_streamingState.eligible = m_meta.processing.isParticipatingMaterialTexture && totalMipCount > 1u && HasStreamingSourceData();
	if (!wasEligible && m_streamingState.eligible) {
		m_streamingState.enabled = !m_suppressMipStreaming && IsMaterialTextureStreamingEnabledSetting();
	}
	else {
		m_streamingState.enabled =
			m_streamingState.enabled &&
			m_streamingState.eligible &&
			!m_suppressMipStreaming &&
			IsMaterialTextureStreamingEnabledSetting();
	}
	if (m_meta.isProcessingCacheArtifact && (m_desc.isCubemap || m_desc.isArray)) {
		m_streamingState.enabled = false;
		m_streamingState.requestedTopMip = 0u;
		m_streamingState.pendingTopMip = 0u;
	}
	m_streamingState.residency.totalMipCount = totalMipCount;
	m_streamingState.residency.residentTopMip = (std::min)(m_streamingState.residency.residentTopMip, totalMipCount - 1u);
	const uint32_t maxResidentMipCount = totalMipCount - m_streamingState.residency.residentTopMip;
	m_streamingState.residency.residentMipCount = (std::max)(1u, (std::min)(descMipCount, maxResidentMipCount));
	m_streamingState.requestedTopMip = (std::min)(m_streamingState.requestedTopMip, totalMipCount - 1u);
	m_streamingState.pendingTopMip = (std::min)(m_streamingState.pendingTopMip, totalMipCount - 1u);
	if (!wasEligible && m_streamingState.enabled) {
		ApplyStreamingBootstrapTopMip();
	}
}

void TextureAsset::ApplyStreamingBootstrapTopMip() {
	if (!m_streamingState.eligible ||
		m_streamingState.requestedTopMip != 0u ||
		m_streamingState.pendingTopMip != 0u ||
		m_streamingState.lastSeenFrame != 0u) {
		return;
	}

	const uint32_t bootstrapTopMip = ComputeDefaultStreamingBootstrapTopMip(
		m_desc,
		m_streamingState.residency.totalMipCount);
	if (bootstrapTopMip == 0u) {
		return;
	}

	m_streamingState.requestedTopMip = bootstrapTopMip;
	m_streamingState.pendingTopMip = bootstrapTopMip;
}

bool TextureAsset::HasStreamingSourceData() const {
	return m_pipelineRequest != nullptr;
}

std::shared_ptr<TextureAsset> TextureAsset::CreateFromImage(std::shared_ptr<org::PixelBuffer> image,
	std::shared_ptr<org::Sampler> sampler, std::string name)
{
	auto desc = image ? image->GetDescription() : org::TextureDescription{};
	auto texture = std::shared_ptr<TextureAsset>(new TextureAsset(
		std::move(desc), std::move(image), std::move(sampler), TextureFileMeta{}));
	if (!name.empty()) texture->SetName(name);
	return texture;
}

std::shared_ptr<TextureAsset> TextureAsset::CreateForPipeline(
	std::shared_ptr<const br::textures::TextureRequest> request, std::string cacheKey)
{
	// The shape is unknown until the pipeline reads the cache header or processes the
	// source; a 1x1 description stands in, so streaming stays ineligible until then.
	org::TextureDescription desc{};
	desc.channels = 4;
	desc.format = request->preferSRGB ? rhi::Format::R8G8B8A8_UNorm_sRGB : rhi::Format::R8G8B8A8_UNorm;
	desc.hasRTV = request->allowRTV;
	desc.hasUAV = request->allowUAV;
	org::ImageDimensions dims{};
	dims.width = 1;
	dims.height = 1;
	dims.rowPitch = 4;
	dims.slicePitch = 4;
	desc.imageDimensions.push_back(dims);

	TextureFileMeta meta{};
	meta.filePath = request->source.identity;
	meta.preferSRGB = request->preferSRGB;
	if (request->preparation == br::textures::TexturePreparation::Processed) meta.processing = request->processing;
	// Until the pipeline reports the real shape, describe the format processing will most
	// likely produce, so channel choices made from it (normal Z reconstruction) hold.
	if (const auto predicted = TextureProcessingManager::GetInstance().PredictedFormat(meta)) desc.format = *predicted;
	auto texture = std::shared_ptr<TextureAsset>(new TextureAsset(
		std::move(desc), nullptr, request->sampler, std::move(meta)));
	texture->m_name = request->debugName;
	texture->m_pipelineRequest = std::move(request);
	texture->m_pipelineCacheKey = std::move(cacheKey);
	texture->m_suppressMipStreaming = !texture->m_pipelineRequest->mipStreaming;
	texture->RecordLoadPath(TextureLoadPathTelemetry::DeferredFileReference, "texture pipeline request");
	return texture;
}

void TextureAsset::ApplyPipelineSourceShape(const org::TextureDescription& fullDesc) {
	std::scoped_lock lock(m_uploadAdvanceMutex);
	if (fullDesc.imageDimensions.empty()) return;
	if (!m_hasUploadedFinalImage) m_desc = fullDesc;
	m_desc.hasRTV = m_pipelineRequest ? m_pipelineRequest->allowRTV : m_desc.hasRTV;
	m_desc.hasUAV = m_pipelineRequest ? m_pipelineRequest->allowUAV : m_desc.hasUAV;
	UpdateSourceShapeFromDescription(fullDesc, CalcMipCountFromDescription(fullDesc));
	RefreshStreamingStateFromDescription();
	BumpStreamingStateRevision();
}

bool TextureAsset::NeedsPipelineResidency() const {
	std::scoped_lock lock(m_uploadAdvanceMutex);
	if (HasTerminalLoadFailure()) return false;
	if (!m_hasUploadedFinalImage || !m_image) return true;
	return m_streamingState.enabled
		? m_streamingState.pendingTopMip != m_streamingState.residency.residentTopMip
		: m_streamingState.residency.residentTopMip != 0u;
}

uint32_t TextureAsset::GetDesiredResidentTopMip() const {
	return m_streamingState.enabled
		? (std::min)(m_streamingState.pendingTopMip, m_streamingState.residency.totalMipCount - 1u)
		: 0u;
}

void TextureAsset::InvalidateResidentImageForStreamingRequest() {
	if (!m_hasUploadedFinalImage || !HasStreamingSourceData()) {
		return;
	}

	if (m_streamingState.residency.residentTopMip == GetDesiredResidentTopMip()) {
		return;
	}

	// Keep the last usable image published while the replacement is prepared.
	// Material descriptors continue to reference it until the streaming worker
	// produces a complete replacement and the main thread adopts that binding.
	m_hasUploadedFinalImage = false;
	m_hasUploadedPlaceholder = false;
	BumpBindingRevision();
}

void TextureAsset::BumpStreamingStateRevision() {
	++m_streamingState.stateRevision;
}

void TextureAsset::BumpBindingRevision() {
	++m_streamingState.bindingRevision;
	BumpStreamingStateRevision();
}

void TextureAsset::RecordLoadPath(TextureLoadPathTelemetry path, std::string detail) {
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	m_meta.loadPath = path;
	m_meta.loadPathDetail = std::move(detail);
	if (m_lastReportedLoadPath == path) {
		return;
	}
	m_lastReportedLoadPath = path;
	spdlog::debug(
		"TextureTelemetry: load_path={} texture='{}' detail='{}'",
		ToString(path),
		TextureTelemetryLabel(*this),
		m_meta.loadPathDetail);
}

void TextureAsset::RecordUploadPath(TextureUploadPathTelemetry path, std::string detail) {
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	m_meta.uploadPath = path;
	m_meta.uploadPathDetail = std::move(detail);
	if (m_lastReportedUploadPath == path) {
		return;
	}
	m_lastReportedUploadPath = path;
	spdlog::debug(
		"TextureTelemetry: upload_path={} texture='{}' detail='{}'",
		ToString(path),
		TextureTelemetryLabel(*this),
		m_meta.uploadPathDetail);
}

void TextureAsset::SetProcessingSettings(TextureProcessingSettings settings) {
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	const bool wasEligible = m_streamingState.eligible;
	m_meta.processing = std::move(settings);
	RefreshStreamingStateFromDescription();
	if (m_streamingState.eligible) {
		m_streamingState.enabled = !m_suppressMipStreaming && IsMaterialTextureStreamingEnabledSetting();
		if (!wasEligible) {
			ApplyStreamingBootstrapTopMip();
		}
	}
	BumpStreamingStateRevision();
	InvalidateResidentImageForStreamingRequest();
}

TexturePendingDebugInfo TextureAsset::GetPendingDebugInfo() const {
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	TexturePendingDebugInfo info{};
	info.label = TextureTelemetryLabel(*this);
	info.debugName = m_name;
	info.sourceIdentity = m_meta.processing.sourceIdentity;
	info.filePath = m_meta.filePath;
	info.hasUsableImage = HasUsableImage();
	info.hasFinalImage = m_hasUploadedFinalImage;
	info.hasPlaceholder = m_hasUploadedPlaceholder;
	info.needsStreamingReload =
		m_hasUploadedFinalImage &&
		HasStreamingSourceData() &&
		((m_streamingState.enabled &&
		  m_streamingState.pendingTopMip != m_streamingState.residency.residentTopMip) ||
		 (!m_streamingState.enabled && m_streamingState.residency.residentTopMip != 0u));
	info.isProcessingCacheArtifact = m_meta.isProcessingCacheArtifact;
	info.streamingTextureID = m_streamingState.streamingTextureID;
	info.requestedTopMip = m_streamingState.requestedTopMip;
	info.pendingTopMip = m_streamingState.pendingTopMip;
	info.residentTopMip = m_streamingState.residency.residentTopMip;
	info.residentMipCount = m_streamingState.residency.residentMipCount;
	info.totalMipCount = m_streamingState.residency.totalMipCount;
	info.stateRevision = m_streamingState.stateRevision;
	info.bindingRevision = m_streamingState.bindingRevision;
	info.loadPath = ToString(m_meta.loadPath);
	info.uploadPath = ToString(m_meta.uploadPath);
	return info;
}

bool TextureAsset::ApplyStreamingSystemRequest(uint32_t topMip, uint64_t frameIndex, bool forceResidencyChange) {
	constexpr uint64_t kUpgradeRequestSettleFrames = 8u;
	constexpr uint32_t kStreamingMipLevelStep = 4u;
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	const uint32_t rawClampedTopMip = (std::min)(topMip, m_streamingState.residency.totalMipCount - 1u);
	if (!forceResidencyChange) {
		m_streamingState.lastFeedbackTopMip = rawClampedTopMip;
	}
	// Every streamed image is a complete replacement, so changing residency by one
	// mip at a time is disproportionately expensive.  Quantize upgrades toward the
	// finer end of a four-mip bucket: this never supplies less detail than feedback
	// requested and caps a typical 12-mip texture at 11 -> 8 -> 4 -> 0.
	const uint32_t clampedTopMip =
		!forceResidencyChange && rawClampedTopMip < m_streamingState.residency.residentTopMip
			? (rawClampedTopMip / kStreamingMipLevelStep) * kStreamingMipLevelStep
			: rawClampedTopMip;
	const bool requestChanged = m_streamingState.requestedTopMip != clampedTopMip;
	const bool frameChanged = frameIndex != 0u && m_streamingState.lastSeenFrame != frameIndex;
	const bool requestIsResident = clampedTopMip >= m_streamingState.residency.residentTopMip;
	// A residency job reads the window streaming wants when it runs, so a change of the
	// wanted window never waits for a load in flight.
	const bool uploadInProgress = false;
	if (requestChanged) {
		m_streamingRequestChangedFrame = frameIndex;
	}
	// Feedback commonly walks through several successively finer mips while an
	// object approaches the camera.  Rebuilding a complete replacement for every
	// intermediate request multiplies texture copies.  Keep the current usable
	// image briefly and let the request settle; mip zero remains immediate.
	const bool upgradeRequestSettled =
		frameIndex == 0u ||
		clampedTopMip == 0u ||
		m_streamingRequestChangedFrame == 0u ||
		frameIndex >= m_streamingRequestChangedFrame + kUpgradeRequestSettleFrames;
	const bool shouldChangeResidency =
		(forceResidencyChange && !uploadInProgress && m_streamingState.pendingTopMip != clampedTopMip) ||
		(!requestIsResident && !uploadInProgress && m_streamingState.pendingTopMip != clampedTopMip &&
		 upgradeRequestSettled);

	if (!requestChanged && !shouldChangeResidency && !frameChanged) {
		return false;
	}

	if (frameIndex != 0u) {
		m_streamingState.lastSeenFrame = frameIndex;
	}
	if (requestChanged) {
		m_streamingState.requestedTopMip = clampedTopMip;
	}
	if (shouldChangeResidency) {
		m_streamingState.pendingTopMip = clampedTopMip;
	}

	if (requestChanged || shouldChangeResidency) {
		BumpStreamingStateRevision();
	}
	return shouldChangeResidency;
}

void TextureAsset::EnableMipStreaming(bool enabled) {
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	const bool newEnabled = enabled && !m_suppressMipStreaming && m_streamingState.eligible && IsMaterialTextureStreamingEnabledSetting();
	if (m_streamingState.enabled == newEnabled) {
		return;
	}
	m_streamingState.enabled = newEnabled;
	if (m_streamingState.enabled) {
		ApplyStreamingBootstrapTopMip();
	}
	BumpStreamingStateRevision();
	InvalidateResidentImageForStreamingRequest();
}

void TextureAsset::SetMipStreamingSuppressed(bool suppressed) {
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	if (m_suppressMipStreaming == suppressed) {
		return;
	}
	m_suppressMipStreaming = suppressed;
	if (m_suppressMipStreaming && m_streamingState.enabled) {
		m_streamingState.enabled = false;
		BumpStreamingStateRevision();
		InvalidateResidentImageForStreamingRequest();
	}
	else if (!m_suppressMipStreaming) {
		EnableMipStreaming(true);
	}
}

void TextureAsset::SetRequestedTopMip(uint32_t topMip, uint64_t frameIndex) {
	const uint32_t clampedTopMip = (std::min)(topMip, m_streamingState.residency.totalMipCount - 1u);
	if (m_streamingState.requestedTopMip == clampedTopMip &&
		(frameIndex == 0u || m_streamingState.lastSeenFrame == frameIndex)) {
		return;
	}
	m_streamingState.requestedTopMip = clampedTopMip;
	if (frameIndex != 0u) {
		m_streamingState.lastSeenFrame = frameIndex;
	}
	BumpStreamingStateRevision();
}

void TextureAsset::SetPendingTopMip(uint32_t topMip) {
	const uint32_t clampedTopMip = (std::min)(topMip, m_streamingState.residency.totalMipCount - 1u);
	if (m_streamingState.pendingTopMip == clampedTopMip) {
		return;
	}
	m_streamingState.pendingTopMip = clampedTopMip;
	BumpStreamingStateRevision();
}

void TextureAsset::SetResidentMipWindow(uint32_t residentTopMip, uint32_t residentMipCount) {
	const uint32_t totalMipCount = m_streamingState.residency.totalMipCount;
	const uint32_t clampedTopMip = (std::min)(residentTopMip, totalMipCount - 1u);
	const uint32_t clampedMipCount = (std::max)(1u, (std::min)(residentMipCount, totalMipCount - clampedTopMip));
	if (m_streamingState.residency.residentTopMip == clampedTopMip &&
		m_streamingState.residency.residentMipCount == clampedMipCount) {
		return;
	}
	m_streamingState.residency.residentTopMip = clampedTopMip;
	m_streamingState.residency.residentMipCount = clampedMipCount;
	BumpStreamingStateRevision();
}

void TextureAsset::NoteTextureSeen(uint64_t frameIndex) {
	if (frameIndex == 0u || m_streamingState.lastSeenFrame == frameIndex) {
		return;
	}
	m_streamingState.lastSeenFrame = frameIndex;
}

void TextureAsset::AdoptUploadedImage(std::shared_ptr<org::PixelBuffer> image) {
	const uint32_t desiredResidentTopMip = GetDesiredResidentTopMip();
	if (image && !image->HasValidBackingResource()) {
		image.reset();
	}
	SetPreparedImageLocked(std::move(image));
	if (m_image) {
		m_desc = m_image->GetDescription();
	}
	m_hasUploadedFinalImage = (m_image != nullptr);
	m_hasUploadedPlaceholder = false;
	RefreshStreamingStateFromDescription();
	const uint32_t actualResidentTopMip = InferTextureSourceTopMip(
		m_desc,
		GetFullMip0Width(),
		GetFullMip0Height(),
		m_streamingState.residency.totalMipCount);
	SetResidentMipWindow(actualResidentTopMip, CalcMipCountFromDescription(m_desc));
	SetPendingTopMip(desiredResidentTopMip);
	BumpBindingRevision();
	if (!m_name.empty() && HasUsableImage()) {
		m_image->SetName(m_name);
	}
}

bool TextureAsset::PublishPreparedImage(
	uint64_t bindingRevision,
	const std::shared_ptr<org::PixelBuffer>& image,
	std::shared_ptr<org::PixelBuffer>* replacedPublishedImage)
{
	std::scoped_lock lock(m_uploadAdvanceMutex);
	if (m_streamingState.bindingRevision != bindingRevision || m_image != image || !image ||
		!image->HasValidBackingResource()) {
		return false;
	}
	if (replacedPublishedImage) {
		// Publication and capture of the binding being replaced must be one atomic
		// operation.  The streaming worker's earlier view can be stale if the main
		// thread adopted an intermediate revision while the worker prepared this one.
		*replacedPublishedImage = m_publishedImage;
	}
	m_publishedImage = image;
	m_publishedBindingRevision = bindingRevision;
	m_publishedStreamingState = m_streamingState;
	return true;
}

bool TextureAsset::RejectPreparedImage(
	uint64_t bindingRevision,
	const std::shared_ptr<org::PixelBuffer>& image)
{
	std::scoped_lock lock(m_uploadAdvanceMutex);
	if (m_streamingState.bindingRevision != bindingRevision || m_image != image) {
		return false;
	}
	// The last published image remains valid.  Discard only the failed prepared
	// revision so EnsureUploaded can reopen the source and create a replacement.
	m_image.reset();
	m_hasUploadedFinalImage = false;
	m_hasUploadedPlaceholder = false;
	BumpBindingRevision();
	return true;
}

bool TextureAsset::DropResidentImageForStreaming() {
	std::lock_guard lock(m_uploadAdvanceMutex);
	if (!m_hasUploadedFinalImage || !HasStreamingSourceData()) {
		return false;
	}

	m_image.reset();
	if (!m_meta.processing.isParticipatingMaterialTexture) {
		m_publishedImage.reset();
		m_publishedBindingRevision = 0u;
	}
	// Participating material textures keep the last usable binding published until
	// a replacement reaches the manager's adoption boundary.  Dropping the working
	// image must not expose a null descriptor to existing generic texture-streaming
	// material owners.
	m_hasUploadedFinalImage = false;
	m_hasUploadedPlaceholder = false;
	BumpBindingRevision();
	RecordUploadPath(TextureUploadPathTelemetry::DeferredPlaceholder, "height atlas streaming evicted resident image");
	return true;
}
