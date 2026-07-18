#include "Resources/Texture.h"

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

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/DirectStorageManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Managers/Singletons/TextureProcessingManager.h"
#include "Utilities/ProcessedTextureCache.h"

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

const char* ToString(TextureReloadJobState state) {
	switch (state) {
	case TextureReloadJobState::Queued:
		return "Queued";
	case TextureReloadJobState::BuildingSourceData:
		return "BuildingSourceData";
	case TextureReloadJobState::Ready:
		return "Ready";
	case TextureReloadJobState::Failed:
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

uint32_t CalcMipCountFromDescription(const TextureDescription& desc) {
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

bool ShouldPreserveAlphaCoverage(const TextureFileMeta& meta, const TextureDescription& desc) {
	if (!meta.processing.isParticipatingMaterialTexture || meta.alphaIsAllOpaque) {
		return false;
	}
	if (desc.isArray || desc.isCubemap || desc.channels != 4 || desc.imageDimensions.empty()) {
		return false;
	}

	return rhi::helpers::stripSrgb(desc.format) == rhi::Format::R8G8B8A8_UNorm;
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

	auto* nativeDevice = rhi::dx12::get_device(DeviceManager::GetInstance().GetDevice());
	if (nativeDevice == nullptr) {
		if (outError) {
			*outError = "failed to get native D3D12 device for conditioned texture cache layout";
		}
		return false;
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

		ImageDimensions dims{};
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
	TextureDescription& outDesc,
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

			ImageDimensions dims{};
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

uint32_t ComputeDefaultStreamingBootstrapTopMip(const TextureDescription& desc, uint32_t totalMipCount) {
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

std::shared_ptr<PixelBuffer> CreatePlaceholderTexture(
	const TextureFactory& factory,
	const TextureProcessingSettings& settings)
{
	TextureDescription desc{};
	desc.channels = 4;
	desc.format = settings.preferSRGB
		? rhi::Format::R8G8B8A8_UNorm_sRGB
		: rhi::Format::R8G8B8A8_UNorm;
	desc.generateMipMaps = false;

	ImageDimensions dims{};
	dims.width = 1;
	dims.height = 1;
	dims.rowPitch = 4;
	dims.slicePitch = 4;
	desc.imageDimensions.push_back(dims);

	uint8_t rgba[4] = { 255u, 255u, 255u, 255u };
	switch (settings.semantic) {
	case TextureSemantic::Normal:
		rgba[0] = 128u;
		rgba[1] = 128u;
		rgba[2] = 255u;
		break;
	case TextureSemantic::Height:
		rgba[0] = 0u;
		rgba[1] = 0u;
		rgba[2] = 0u;
		break;
	case TextureSemantic::Metallic:
		rgba[0] = 0u;
		break;
	case TextureSemantic::Roughness:
		rgba[0] = 255u;
		break;
	case TextureSemantic::MetallicRoughness:
		rgba[0] = 0u;
		rgba[1] = 255u;
		rgba[2] = 0u;
		break;
	default:
		break;
	}

	auto bytes = std::make_shared<std::vector<uint8_t>>(std::begin(rgba), std::end(rgba));
	auto placeholder = factory.CreateAlwaysResidentPixelBuffer(
		desc,
		TextureFactory::TextureInitialData::FromBytes({ bytes }));
	if (placeholder) {
		rg::memory::SetResourceUsageHint(*placeholder, "Material textures");
	}
	return placeholder;
}

std::shared_ptr<PixelBuffer> GetSharedProcessingPlaceholderTexture(
	const TextureFactory& factory,
	const TextureProcessingSettings& settings)
{
	ZoneScopedN("TextureAsset::GetSharedProcessingPlaceholderTexture");
	constexpr size_t kTextureSemanticCount = static_cast<size_t>(TextureSemantic::OpenPBRScalar) + 1u;
	constexpr size_t kPlaceholderVariantCount = kTextureSemanticCount * 2u;
	const size_t semanticIndex = static_cast<size_t>(settings.semantic);
	const size_t cacheIndex = semanticIndex * 2u + (settings.preferSRGB ? 1u : 0u);

	if (cacheIndex >= kPlaceholderVariantCount) {
		ZoneScopedN("TextureAsset::GetSharedProcessingPlaceholderTexture::UncachedSemantic");
		TracyPlot("SARP.Texture.ProcessingPlaceholder.UncachedSemantic", static_cast<int64_t>(semanticIndex));
		return CreatePlaceholderTexture(factory, settings);
	}

	static std::mutex cacheMutex;
	static std::array<std::shared_ptr<PixelBuffer>, kPlaceholderVariantCount> placeholders{};

	std::lock_guard<std::mutex> lock(cacheMutex);
	auto& placeholder = placeholders[cacheIndex];
	if (placeholder && placeholder->HasValidBackingResource()) {
		ZoneScopedN("TextureAsset::GetSharedProcessingPlaceholderTexture::CacheHit");
		TracyPlot("SARP.Texture.ProcessingPlaceholder.CacheHit", static_cast<int64_t>(1));
		return placeholder;
	}

	{
		ZoneScopedN("TextureAsset::GetSharedProcessingPlaceholderTexture::CreatePlaceholderTexture");
		TracyPlot("SARP.Texture.ProcessingPlaceholder.CacheMiss", static_cast<int64_t>(1));
		placeholder = CreatePlaceholderTexture(factory, settings);
		if (placeholder) {
			std::ostringstream name;
			name << "Shared Processing Placeholder "
				<< ToString(settings.semantic)
				<< (settings.preferSRGB ? " sRGB" : " Linear");
			placeholder->SetName(name.str());
		}
	}
	return placeholder;
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

		ImageDimensions dims{};
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
		ImageDimensions dims{};
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

std::shared_ptr<TextureReloadJobHandle> RequestReloadSourceDataAsync(
	std::string filePath,
	bool preferSRGB,
	uint32_t targetTopMip,
	bool streamingEnabled,
	std::string reason)
{
	auto handle = std::make_shared<TextureReloadJobHandle>();
	handle->targetTopMip = targetTopMip;
	handle->state.store(TextureReloadJobState::Queued, std::memory_order_release);

	TaskSchedulerManager::GetInstance().RunBackgroundTask("TextureAsset::RequestReloadSourceDataAsync", [handle, filePath = std::move(filePath), preferSRGB, targetTopMip, streamingEnabled, reason = std::move(reason)]() mutable {
		ZoneScopedN("TextureAsset::RequestReloadSourceDataAsync::BuildSourceData");
		ZoneText(filePath.data(), filePath.size());
		if (!reason.empty()) {
			ZoneText(reason.data(), reason.size());
		}
		handle->state.store(TextureReloadJobState::BuildingSourceData, std::memory_order_release);
		try {
			auto sourceData = BuildSourceDataFromTextureFilePath(filePath, preferSRGB, reason);
			const uint32_t fullMipCount = CalcMipCountFromDescription(sourceData->desc);
			const uint32_t clippedTopMip = (streamingEnabled && fullMipCount > 1u)
				? (std::min)(targetTopMip, fullMipCount - 1u)
				: 0u;
			auto reloadSourceData = ClipTextureSourceDataTopMip(sourceData, clippedTopMip);

			{
				std::scoped_lock lock(handle->mutex);
				handle->sourceData = std::move(reloadSourceData);
				handle->sourceTotalMipCount = fullMipCount;
				handle->sourceFullWidth = sourceData->desc.imageDimensions.empty() ? 0u : sourceData->desc.imageDimensions[0].width;
				handle->sourceFullHeight = sourceData->desc.imageDimensions.empty() ? 0u : sourceData->desc.imageDimensions[0].height;
				handle->error.clear();
			}

			handle->state.store(TextureReloadJobState::Ready, std::memory_order_release);
		}
		catch (const std::exception& ex) {
			{
				std::scoped_lock lock(handle->mutex);
				handle->error = ex.what();
			}
			handle->state.store(TextureReloadJobState::Failed, std::memory_order_release);
		}
	});

	return handle;
}

std::shared_ptr<PixelBuffer> TryUploadDDSFilePathDirectToVRAM(
	const std::string& path,
	bool preferSRGB,
	uint32_t topMip,
	bool allowRTV,
	bool allowUAV)
{
	if (path.empty()) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidRequest, path, "empty texture path");
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
	RecordDirectStorageTexturePreflight(
		DirectStorageTexturePreflightResult::UnsupportedFormat,
		path,
		"raw DDS rows are tightly packed, but DirectStorage texture uploads require D3D12 copyable-footprint row layout");
	return {};

	const std::filesystem::path filePath(path);
	std::wstring extension = filePath.extension().wstring();
	std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
	if (extension != L".dds") {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::UnsupportedFileType, path, "only DDS files are supported");
		return {};
	}

	DirectX::ScratchImage image;
	DirectX::TexMetadata metadata{};
	const HRESULT loadHr = DirectX::LoadFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
	if (FAILED(loadHr) || metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.depth != 1) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidMetadata, path, "failed to load 2D DDS metadata");
		return {};
	}

	size_t headerSize = 0;
	const HRESULT headerHr = DirectX::EncodeDDSHeader(
		metadata,
		DirectX::DDS_FLAGS_NONE,
		nullptr,
		(std::numeric_limits<size_t>::max)(),
		headerSize);
	if (FAILED(headerHr) || headerSize == 0) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidMetadata, path, "failed to encode DDS header");
		return {};
	}

	TextureDescription desc{};
	desc.format = rhi::helpers::ToRHI(preferSRGB ? DirectX::MakeSRGB(metadata.format) : DirectX::MakeLinear(metadata.format));
	desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(desc.format));
	if (rhi::helpers::IsBlockCompressed(desc.format)) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::UnsupportedFormat, path, "block-compressed DDS uses packed block rows incompatible with this region-copy path");
		return {};
	}

	desc.isCubemap = metadata.IsCubemap();
	desc.isArray = metadata.arraySize > 1 && !desc.isCubemap;
	desc.arraySize = desc.isCubemap
		? static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize / size_t(6)))
		: static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));
	desc.hasRTV = allowRTV;
	desc.hasUAV = allowUAV;
	desc.generateMipMaps = false;
	desc.initialLayout = rhi::ResourceLayout::Common;

	const uint32_t fullMipCount = static_cast<uint32_t>((std::max)(size_t(1), metadata.mipLevels));
	const uint32_t clampedTopMip = (std::min)(topMip, fullMipCount - 1u);
	const uint32_t arraySlices = static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));
	const DirectX::Image* images = image.GetImages();
	const size_t imageCount = image.GetImageCount();
	if (images == nullptr || imageCount != static_cast<size_t>(arraySlices) * fullMipCount) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidLayout, path, "DDS image count did not match array slices and mips");
		return {};
	}

	const uint32_t residentMipCount = fullMipCount - clampedTopMip;
	desc.imageDimensions.reserve(static_cast<size_t>(arraySlices) * residentMipCount);
	std::vector<br::DirectStorageTextureRegionCopy> regions;
	regions.reserve(static_cast<size_t>(arraySlices) * residentMipCount);

	uint64_t currentOffset = static_cast<uint64_t>(headerSize);
	uint32_t destinationSubresourceIndex = 0u;
	for (size_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
		const DirectX::Image& srcImage = images[imageIndex];
		if (srcImage.width > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) ||
			srcImage.height > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) ||
			srcImage.slicePitch == 0 ||
			srcImage.slicePitch > static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
			RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidLayout, path, "DDS image dimensions exceeded upload limits");
			return {};
		}

		const uint32_t mipIndex = static_cast<uint32_t>(imageIndex % fullMipCount);
		if (mipIndex >= clampedTopMip) {
			ImageDimensions dims{};
			dims.width = static_cast<uint32_t>(srcImage.width);
			dims.height = static_cast<uint32_t>(srcImage.height);
			dims.rowPitch = srcImage.rowPitch;
			dims.slicePitch = srcImage.slicePitch;
			desc.imageDimensions.push_back(dims);

			br::DirectStorageTextureRegionCopy region{};
			region.sourceOffset = currentOffset;
			region.sourceSizeBytes = static_cast<uint32_t>(srcImage.slicePitch);
			region.uncompressedSizeBytes = static_cast<uint32_t>(srcImage.slicePitch);
			region.subresourceIndex = destinationSubresourceIndex++;
			region.width = dims.width;
			region.height = dims.height;
			region.depth = 1;
			regions.push_back(region);
		}

		currentOffset += srcImage.slicePitch;
	}

	if (regions.empty()) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidLayout, path, "no upload regions were produced");
		return {};
	}

	auto pixelBuffer = PixelBuffer::CreateShared(desc);
	if (!pixelBuffer) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::ResourceCreateFailed, path, "failed to create destination PixelBuffer");
		return {};
	}
	std::string directStorageMessage;
	if (!DirectStorageManager::GetInstance().UploadTextureRegionsFromFile(filePath.wstring(), pixelBuffer->GetAPIResource(), regions, &directStorageMessage)) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::EnqueueFailed, path, directStorageMessage);
		if (!directStorageMessage.empty()) {
			spdlog::debug("TextureAsset: DirectStorage fallback for '{}' because {}", path, directStorageMessage);
		}
		return {};
	}

	RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::Eligible, path);
	return pixelBuffer;
}

std::shared_ptr<PixelBuffer> TryUploadConditionedCacheFilePathDirectToVRAM(
	const std::string& path,
	uint32_t topMip,
	bool allowRTV,
	bool allowUAV)
{
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

	TextureDescription desc{};
	DirectStorageTextureSubresourceRangeCopy range{};
	uint32_t clampedTopMip = 0u;
	std::string error;
	if (!TryBuildConditionedCacheResidentUpload(path, topMip, allowRTV, allowUAV, desc, range, clampedTopMip, error)) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidLayout, path, error);
		if (!error.empty()) {
			spdlog::debug("TextureAsset: conditioned cache DirectStorage fallback for '{}' because {}", path, error);
		}
		return {};
	}

	auto pixelBuffer = PixelBuffer::CreateShared(desc);
	if (!pixelBuffer) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::ResourceCreateFailed, path, "failed to create destination PixelBuffer");
		return {};
	}

	std::string directStorageMessage;
	if (!DirectStorageManager::GetInstance().UploadTextureSubresourceRangeFromFile(
			std::filesystem::path(path).wstring(),
			pixelBuffer->GetAPIResource(),
			range,
			&directStorageMessage)) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::EnqueueFailed, path, directStorageMessage);
		if (!directStorageMessage.empty()) {
			spdlog::debug("TextureAsset: conditioned cache DirectStorage fallback for '{}' because {}", path, directStorageMessage);
		}
		return {};
	}

	RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::Eligible, path);
	return pixelBuffer;
}

std::shared_ptr<TextureDirectStorageReloadJobHandle> BeginUploadDDSFilePathDirectToVRAMAsync(
	const std::string& path,
	bool preferSRGB,
	uint32_t topMip,
	bool allowRTV,
	bool allowUAV)
{
	if (path.empty()) {
		RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidRequest, path, "empty texture path");
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

	TaskSchedulerManager::GetInstance().QueueIoTask("TextureAsset::BeginUploadDDSFilePathDirectToVRAMAsync", [handle, path, preferSRGB, topMip, allowRTV, allowUAV]() mutable {
		if (handle->cancelRequested.load(std::memory_order_acquire)) {
			handle->state.store(TextureDirectStorageReloadJobState::Failed, std::memory_order_release);
			return;
		}
		handle->state.store(TextureDirectStorageReloadJobState::CreatingResource, std::memory_order_release);

		try {
			RecordDirectStorageTexturePreflight(
				DirectStorageTexturePreflightResult::UnsupportedFormat,
				path,
				"raw DDS rows are tightly packed, but DirectStorage texture uploads require D3D12 copyable-footprint row layout");
			throw std::runtime_error("raw DDS textures do not use this DirectStorage GPU-direct path");

			const std::filesystem::path filePath(path);
			std::wstring extension = filePath.extension().wstring();
			std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
			if (extension != L".dds") {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::UnsupportedFileType, path, "only DDS files are supported");
				throw std::runtime_error("only DDS files support DirectStorage GPU-direct texture upload");
			}

			DirectX::ScratchImage image;
			DirectX::TexMetadata metadata{};
			const HRESULT loadHr = DirectX::LoadFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
			if (FAILED(loadHr) || metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.depth != 1) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidMetadata, path, "failed to load 2D DDS metadata");
				throw std::runtime_error("failed to load DDS metadata for DirectStorage GPU-direct texture upload");
			}

			size_t headerSize = 0;
			const HRESULT headerHr = DirectX::EncodeDDSHeader(
				metadata,
				DirectX::DDS_FLAGS_NONE,
				nullptr,
				(std::numeric_limits<size_t>::max)(),
				headerSize);
			if (FAILED(headerHr) || headerSize == 0) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidMetadata, path, "failed to encode DDS header");
				throw std::runtime_error("failed to encode DDS header for DirectStorage GPU-direct texture upload");
			}

			TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(preferSRGB ? DirectX::MakeSRGB(metadata.format) : DirectX::MakeLinear(metadata.format));
			desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(desc.format));
			if (rhi::helpers::IsBlockCompressed(desc.format)) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::UnsupportedFormat, path, "block-compressed DDS uses packed block rows incompatible with this region-copy path");
				throw std::runtime_error("block-compressed DDS textures do not use this DirectStorage GPU-direct path");
			}

			desc.isCubemap = metadata.IsCubemap();
			desc.isArray = metadata.arraySize > 1 && !desc.isCubemap;
			desc.arraySize = desc.isCubemap
				? static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize / size_t(6)))
				: static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));
			desc.hasRTV = allowRTV;
			desc.hasUAV = allowUAV;
			desc.generateMipMaps = false;
			desc.initialLayout = rhi::ResourceLayout::Common;

			const uint32_t fullMipCount = static_cast<uint32_t>((std::max)(size_t(1), metadata.mipLevels));
			const uint32_t clampedTopMip = (std::min)(topMip, fullMipCount - 1u);
			const uint32_t arraySlices = static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));
			const DirectX::Image* images = image.GetImages();
			const size_t imageCount = image.GetImageCount();
			if (images == nullptr || imageCount != static_cast<size_t>(arraySlices) * fullMipCount) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidLayout, path, "DDS image count did not match array slices and mips");
				throw std::runtime_error("DDS image layout did not match expected subresource count for DirectStorage GPU-direct texture upload");
			}
			if (handle->cancelRequested.load(std::memory_order_acquire)) {
				throw std::runtime_error("DirectStorage texture upload was canceled before resource creation");
			}

			const uint32_t residentMipCount = fullMipCount - clampedTopMip;
			desc.imageDimensions.reserve(static_cast<size_t>(arraySlices) * residentMipCount);
			std::vector<br::DirectStorageTextureRegionCopy> regions;
			regions.reserve(static_cast<size_t>(arraySlices) * residentMipCount);

			uint64_t currentOffset = static_cast<uint64_t>(headerSize);
			uint32_t destinationSubresourceIndex = 0u;
			for (size_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
				const DirectX::Image& srcImage = images[imageIndex];
				if (srcImage.width > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) ||
					srcImage.height > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) ||
					srcImage.slicePitch == 0 ||
					srcImage.slicePitch > static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
					RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidLayout, path, "DDS image dimensions exceeded upload limits");
					throw std::runtime_error("DDS image dimensions exceeded DirectStorage GPU-direct texture upload limits");
				}

				const uint32_t mipIndex = static_cast<uint32_t>(imageIndex % fullMipCount);
				if (mipIndex >= clampedTopMip) {
					ImageDimensions dims{};
					dims.width = static_cast<uint32_t>(srcImage.width);
					dims.height = static_cast<uint32_t>(srcImage.height);
					dims.rowPitch = srcImage.rowPitch;
					dims.slicePitch = srcImage.slicePitch;
					desc.imageDimensions.push_back(dims);

					br::DirectStorageTextureRegionCopy region{};
					region.sourceOffset = currentOffset;
					region.sourceSizeBytes = static_cast<uint32_t>(srcImage.slicePitch);
					region.uncompressedSizeBytes = static_cast<uint32_t>(srcImage.slicePitch);
					region.subresourceIndex = destinationSubresourceIndex++;
					region.width = dims.width;
					region.height = dims.height;
					region.depth = 1;
					regions.push_back(region);
				}

				currentOffset += srcImage.slicePitch;
			}

			if (regions.empty()) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::InvalidLayout, path, "no upload regions were produced");
				throw std::runtime_error("no texture regions were produced for DirectStorage GPU-direct texture upload");
			}

			auto uploadedImage = PixelBuffer::CreateShared(desc);
			if (!uploadedImage) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::ResourceCreateFailed, path, "failed to create destination PixelBuffer");
				throw std::runtime_error("failed to create resident PixelBuffer for DirectStorage GPU-direct texture upload");
			}
			if (handle->cancelRequested.load(std::memory_order_acquire)) {
				throw std::runtime_error("DirectStorage texture upload was canceled before enqueue");
			}

			std::string directStorageMessage;
			DirectStorageAsyncRequestHandle requestHandle = DirectStorageManager::GetInstance().EnqueueUploadTextureRegionsFromFile(
				filePath.wstring(),
				uploadedImage->GetAPIResource(),
				regions,
				&directStorageMessage);
			if (!requestHandle.IsValid()) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::EnqueueFailed, path, directStorageMessage);
				throw std::runtime_error(directStorageMessage.empty()
					? "failed to enqueue DirectStorage GPU-direct texture upload"
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

	TaskSchedulerManager::GetInstance().QueueIoTask("TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync", [handle, path, topMip, allowRTV, allowUAV]() mutable {
		ZoneScopedN("TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync::IoTask");
		if (handle->cancelRequested.load(std::memory_order_acquire)) {
			handle->state.store(TextureDirectStorageReloadJobState::Failed, std::memory_order_release);
			return;
		}
		handle->state.store(TextureDirectStorageReloadJobState::CreatingResource, std::memory_order_release);

		try {
			TextureDescription desc{};
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

			std::shared_ptr<PixelBuffer> uploadedImage;
			{
				ZoneScopedN("TextureAsset::BeginUploadConditionedCacheFilePathDirectToVRAMAsync::CreatePixelBuffer");
				uploadedImage = PixelBuffer::CreateShared(desc);
			}
			if (!uploadedImage) {
				RecordDirectStorageTexturePreflight(DirectStorageTexturePreflightResult::ResourceCreateFailed, path, "failed to create destination PixelBuffer");
				throw std::runtime_error("failed to create resident PixelBuffer for conditioned texture cache DirectStorage upload");
			}
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

uint32_t TextureAsset::NextStreamingTextureID() {
	static std::atomic<uint32_t> nextID{1u};
	return nextID.fetch_add(1u, std::memory_order_relaxed);
}

void TextureAsset::UpdateSourceShapeFromDescription(const TextureDescription& desc, uint32_t totalMipCountHint) {
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
		rg::memory::SetResourceUsageHint(*m_image, "Material textures");
		if (!m_meta.filePath.empty()) {
			rg::memory::SetResourceMemoryIdentifier(*m_image, m_meta.filePath);
		}
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
	return !m_initialDataString.empty() || !m_originalSourceBytes.empty();
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

std::shared_ptr<TextureSourceData> TextureAsset::BuildSourceData(const char* reason) {
	if (std::holds_alternative<std::string>(m_initialStorage)) {
		const auto& path = std::get<std::string>(m_initialStorage);
		const std::string buildReason = reason != nullptr ? reason : "TextureAsset::BuildSourceData";
		auto sourceData = BuildSourceDataFromTextureFilePath(path, m_meta.preferSRGB, buildReason);
		UpdateSourceShapeFromDescription(sourceData->desc, CalcMipCountFromDescription(sourceData->desc));
		if (!m_streamingState.enabled) {
			return sourceData;
		}

		const uint32_t fullMipCount = CalcMipCountFromDescription(sourceData->desc);
		if (fullMipCount <= 1u) {
			return sourceData;
		}

		const uint32_t targetTopMip = (std::min)(GetDesiredResidentTopMip(), fullMipCount - 1u);
		return ClipTextureSourceDataTopMip(sourceData, targetTopMip);
	}

	if (!m_originalSourceBytes.empty()) {
		auto sourceData = std::make_shared<TextureSourceData>();
		sourceData->desc = m_originalSourceDesc;
		sourceData->subresources = m_originalSourceBytes;
		sourceData->isBlockCompressed = rhi::helpers::IsBlockCompressed(m_originalSourceDesc.format);
		UpdateSourceShapeFromDescription(sourceData->desc);

		if (!sourceData->desc.imageDimensions.empty()) {
			const uint32_t faces = sourceData->desc.isCubemap ? 6u : 1u;
			const uint32_t slices = faces * (std::max)(1u, sourceData->desc.arraySize);
			const uint32_t fullMipCount = CalcFullMipCount(
				sourceData->desc.imageDimensions[0].width,
				sourceData->desc.imageDimensions[0].height);
			const size_t expectedSubresources = static_cast<size_t>(slices) * fullMipCount;

			sourceData->hasFullMipChain =
				!sourceData->subresources.empty() &&
				sourceData->subresources.size() == expectedSubresources &&
				sourceData->desc.imageDimensions.size() == expectedSubresources;

			if (m_streamingState.enabled && fullMipCount > 1u) {
				const uint32_t targetTopMip = (std::min)(GetDesiredResidentTopMip(), fullMipCount - 1u);
				return ClipTextureSourceDataTopMip(sourceData, targetTopMip);
			}
		}

		return sourceData;
	}

	auto sourceData = std::make_shared<TextureSourceData>();
	sourceData->desc = m_desc;
	sourceData->subresources = ResolveToBytes();
	sourceData->isBlockCompressed = rhi::helpers::IsBlockCompressed(m_desc.format);
	UpdateSourceShapeFromDescription(sourceData->desc);

	if (m_desc.imageDimensions.empty()) {
		return sourceData;
	}

	const uint32_t faces = m_desc.isCubemap ? 6u : 1u;
	const uint32_t slices = faces * (std::max)(1u, m_desc.arraySize);
	const uint32_t fullMipCount = CalcFullMipCount(
		m_desc.imageDimensions[0].width,
		m_desc.imageDimensions[0].height);
	const size_t expectedSubresources = static_cast<size_t>(slices) * fullMipCount;

	sourceData->hasFullMipChain =
		!sourceData->subresources.empty() &&
		sourceData->subresources.size() == expectedSubresources &&
		m_desc.imageDimensions.size() == expectedSubresources;

	return sourceData;
}

std::shared_ptr<TextureSourceData> TextureAsset::BuildProcessingSourceData(const char* reason) {
	if (!m_initialDataString.empty()) {
		const std::string buildReason = reason != nullptr ? reason : "TextureAsset::BuildProcessingSourceData";
		auto sourceData = BuildSourceDataFromTextureFilePath(m_initialDataString, m_meta.preferSRGB, buildReason);
		UpdateSourceShapeFromDescription(sourceData->desc, CalcMipCountFromDescription(sourceData->desc));
		return sourceData;
	}

	if (!m_originalSourceBytes.empty()) {
		auto sourceData = std::make_shared<TextureSourceData>();
		sourceData->desc = m_originalSourceDesc;
		sourceData->subresources = m_originalSourceBytes;
		sourceData->isBlockCompressed = rhi::helpers::IsBlockCompressed(m_originalSourceDesc.format);
		UpdateSourceShapeFromDescription(sourceData->desc);

		if (!sourceData->desc.imageDimensions.empty()) {
			const uint32_t faces = sourceData->desc.isCubemap ? 6u : 1u;
			const uint32_t slices = faces * (std::max)(1u, sourceData->desc.arraySize);
			const uint32_t fullMipCount = CalcFullMipCount(
				sourceData->desc.imageDimensions[0].width,
				sourceData->desc.imageDimensions[0].height);
			const size_t expectedSubresources = static_cast<size_t>(slices) * fullMipCount;

			sourceData->hasFullMipChain =
				!sourceData->subresources.empty() &&
				sourceData->subresources.size() == expectedSubresources &&
				sourceData->desc.imageDimensions.size() == expectedSubresources;
		}

		return sourceData;
	}

	return BuildSourceData(reason);
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
	m_processingFallbackRequested = false;
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

void TextureAsset::PrimeConditionedCacheResidentUploadMetadata() const {
	if (m_initialDataString.empty() || !IsConditionedCacheFilePath(m_initialDataString)) {
		return;
	}

	std::string error;
	if (!GetConditionedCacheResidentUploadMetadata(m_initialDataString, error) && !error.empty()) {
		spdlog::debug(
			"TextureAsset: failed to prime conditioned cache upload metadata for '{}': {}",
			m_initialDataString,
			error);
	}
}

TexturePendingDebugInfo TextureAsset::GetPendingDebugInfo() const {
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	TexturePendingDebugInfo info{};
	info.label = TextureTelemetryLabel(*this);
	info.debugName = m_name;
	info.sourceIdentity = m_meta.processing.sourceIdentity;
	info.filePath = m_meta.filePath;
	info.initialData = m_initialDataString;
	info.hasUsableImage = HasUsableImage();
	info.hasFinalImage = m_hasUploadedFinalImage;
	info.hasPlaceholder = m_hasUploadedPlaceholder;
	info.needsStreamingReload =
		m_hasUploadedFinalImage &&
		HasStreamingSourceData() &&
		((m_streamingState.enabled &&
		  m_streamingState.pendingTopMip != m_streamingState.residency.residentTopMip) ||
		 (!m_streamingState.enabled && m_streamingState.residency.residentTopMip != 0u));
	info.hasProcessingHandle = m_processingHandle != nullptr;
	info.hasReloadHandle = m_reloadHandle != nullptr;
	info.hasDirectStorageHandle = m_directStorageReloadHandle != nullptr;
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
	if (m_processingHandle) {
		info.processingState = ToString(m_processingHandle->state.load(std::memory_order_acquire));
	}
	if (m_reloadHandle) {
		info.reloadState = ToString(m_reloadHandle->state.load(std::memory_order_acquire));
	}
	if (m_directStorageReloadHandle) {
		info.directStorageState = ToString(m_directStorageReloadHandle->state.load(std::memory_order_acquire));
		info.directStorageTargetTopMip = m_directStorageReloadHandle->targetTopMip.load(std::memory_order_acquire);
	}
	return info;
}

bool TextureAsset::ApplyStreamingSystemRequest(uint32_t topMip, uint64_t frameIndex, bool forceResidencyChange) {
	constexpr uint64_t kUpgradeRequestSettleFrames = 8u;
	constexpr uint32_t kStreamingMipLevelStep = 4u;
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	const uint32_t rawClampedTopMip = (std::min)(topMip, m_streamingState.residency.totalMipCount - 1u);
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
	bool uploadInProgress =
		m_processingHandle != nullptr ||
		m_reloadHandle != nullptr ||
		m_directStorageReloadHandle != nullptr;
	if (!requestIsResident && m_directStorageReloadHandle) {
		const TextureDirectStorageReloadJobState state = m_directStorageReloadHandle->state.load(std::memory_order_acquire);
		const uint32_t targetTopMip = m_directStorageReloadHandle->targetTopMip.load(std::memory_order_acquire);
		if (targetTopMip > clampedTopMip &&
			(state == TextureDirectStorageReloadJobState::Queued ||
			 state == TextureDirectStorageReloadJobState::CreatingResource)) {
			m_directStorageReloadHandle->cancelRequested.store(true, std::memory_order_release);
			spdlog::info(
				"TextureAsset: canceled stale DirectStorage residency upload texture='{}' requestedTopMip={} staleTargetTopMip={} residentTopMip={} pendingTopMip={} state={}",
				TextureTelemetryLabel(*this),
				clampedTopMip,
				targetTopMip,
				m_streamingState.residency.residentTopMip,
				m_streamingState.pendingTopMip,
				ToString(state));
			m_directStorageReloadHandle.reset();
			uploadInProgress =
				m_processingHandle != nullptr ||
				m_reloadHandle != nullptr ||
				m_directStorageReloadHandle != nullptr;
		}
	}
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

void TextureAsset::AdoptUploadedImage(std::shared_ptr<PixelBuffer> image) {
	const uint32_t residentTopMip = GetDesiredResidentTopMip();
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
	SetResidentMipWindow(residentTopMip, CalcMipCountFromDescription(m_desc));
	SetPendingTopMip(residentTopMip);
	BumpBindingRevision();
	if (!m_name.empty() && HasUsableImage()) {
		m_image->SetName(m_name);
	}
}

bool TextureAsset::PublishPreparedImage(
	uint64_t bindingRevision,
	const std::shared_ptr<PixelBuffer>& image,
	std::shared_ptr<PixelBuffer>* replacedPublishedImage)
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
	return true;
}

DirectStorageAsyncRequestHandle TextureAsset::QueueInitialDirectStorageUploadIfNeeded() {
	if (m_hasUploadedFinalImage) {
		return {};
	}

	const uint32_t desiredResidentTopMip = GetDesiredResidentTopMip();
	if (m_directStorageReloadHandle) {
		const TextureDirectStorageReloadJobState state = m_directStorageReloadHandle->state.load(std::memory_order_acquire);
		const uint32_t targetTopMip = m_directStorageReloadHandle->targetTopMip.load(std::memory_order_acquire);
		if (targetTopMip == desiredResidentTopMip &&
			(state == TextureDirectStorageReloadJobState::Queued ||
			 state == TextureDirectStorageReloadJobState::CreatingResource ||
			 state == TextureDirectStorageReloadJobState::Uploading ||
			 state == TextureDirectStorageReloadJobState::Ready)) {
			return {};
		}

		if (state == TextureDirectStorageReloadJobState::Queued ||
			state == TextureDirectStorageReloadJobState::CreatingResource ||
			state == TextureDirectStorageReloadJobState::Uploading) {
			return {};
		}

		m_directStorageReloadHandle->cancelRequested.store(true, std::memory_order_release);
		m_directStorageReloadHandle.reset();
	}

	auto* filePath = std::get_if<std::string>(&m_initialStorage);
	if (filePath == nullptr || filePath->empty()) {
		return {};
	}

	m_directStorageReloadHandle = IsConditionedCacheFilePath(*filePath)
		? BeginUploadConditionedCacheFilePathDirectToVRAMAsync(
			*filePath,
			desiredResidentTopMip,
			m_desc.hasRTV,
			m_desc.hasUAV)
		: BeginUploadDDSFilePathDirectToVRAMAsync(
			*filePath,
			m_meta.preferSRGB,
			desiredResidentTopMip,
			m_desc.hasRTV,
			m_desc.hasUAV);
	return {};
}

void TextureAsset::EnsureUploaded(const TextureFactory& factory) {
	EnsureUploaded(factory, TextureUploadAdvanceMode::AllowBlockingFallback);
}

TextureUploadAdvanceResult TextureAsset::RequestAllOrNothingUpload(const TextureFactory& factory, TextureUploadAdvanceMode mode) {
	return EnsureUploaded(factory, mode);
}

bool TextureAsset::DropResidentImageForStreaming() {
	std::lock_guard lock(m_uploadAdvanceMutex);
	if (!m_hasUploadedFinalImage || !HasStreamingSourceData()) {
		return false;
	}

	m_image.reset();
	// Keep the last usable binding published until a replacement reaches the
	// manager's adoption boundary.  Dropping the working image must not expose a
	// null descriptor to existing material owners.
	m_hasUploadedFinalImage = false;
	m_hasUploadedPlaceholder = false;
	m_directStorageReloadHandle.reset();
	m_reloadHandle.reset();
	BumpBindingRevision();
	RecordUploadPath(TextureUploadPathTelemetry::DeferredPlaceholder, "height atlas streaming evicted resident image");
	return true;
}

TextureUploadAdvanceResult TextureAsset::EnsureUploaded(const TextureFactory& factory, TextureUploadAdvanceMode mode) {
	ZoneScopedN("TextureAsset::EnsureUploaded");
	std::scoped_lock uploadAdvanceLock(m_uploadAdvanceMutex);
	const uint64_t initialBindingRevision = GetBindingRevision();
	bool didMainThreadUpload = false;
	auto makeResult = [&]() {
		return TextureUploadAdvanceResult{
			.hasUsableImage = HasUsableImage(),
			.hasPendingWork = HasPendingUploadWork(),
			.bindingChanged = GetBindingRevision() != initialBindingRevision,
			.didMainThreadUpload = didMainThreadUpload,
		};
	};
	const bool allowBlockingFallback = mode == TextureUploadAdvanceMode::AllowBlockingFallback;
	{
		ZoneScopedN("TextureAsset::EnsureUploaded::RefreshStreamingState");
		RefreshStreamingStateFromDescription();
	}
	const bool needsStreamingReload =
		m_hasUploadedFinalImage &&
		HasStreamingSourceData() &&
		((m_streamingState.enabled &&
		  m_streamingState.pendingTopMip != m_streamingState.residency.residentTopMip) ||
		 (!m_streamingState.enabled && m_streamingState.residency.residentTopMip != 0u));
	if (m_hasUploadedFinalImage && !needsStreamingReload && HasUsableImage()) {
		return makeResult();
	}

	const uint32_t desiredResidentTopMip = GetDesiredResidentTopMip();
	const bool isParticipatingMaterialTexture = m_meta.processing.isParticipatingMaterialTexture;
	const auto* initialFilePath = std::get_if<std::string>(&m_initialStorage);
	const bool isConditionedCacheSource =
		(initialFilePath != nullptr && IsConditionedCacheFilePath(*initialFilePath)) ||
		m_meta.isProcessingCacheArtifact;
	const bool canUseDirectStorageGpu = DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::Gpu);
	const bool useConditionedCacheResidency =
		isConditionedCacheSource &&
		isParticipatingMaterialTexture &&
		canUseDirectStorageGpu;
	const bool shouldProcessTexture =
		TextureProcessingManager::GetInstance().ShouldProcess(m_meta) &&
		!isConditionedCacheSource &&
		!m_processingFallbackRequested;
	auto ensureProcessingPlaceholder = [&](const std::string& detail) {
		ZoneScopedN("TextureAsset::EnsureUploaded::EnsureProcessingPlaceholder");
		if (HasUsableImage() || !m_meta.processing.allowAsyncPlaceholder) {
			return false;
		}

		{
			ZoneScopedN("TextureAsset::EnsureUploaded::EnsureProcessingPlaceholder::GetSharedPlaceholderTexture");
			m_image = GetSharedProcessingPlaceholderTexture(factory, m_meta.processing);
			if (!m_meta.processing.isParticipatingMaterialTexture) m_publishedImage = m_image;
		}
		{
			ZoneScopedN("TextureAsset::EnsureUploaded::EnsureProcessingPlaceholder::RecordUploadPath");
			RecordUploadPath(TextureUploadPathTelemetry::AsyncProcessingPlaceholder, detail);
		}
		m_hasUploadedPlaceholder = true;
		BumpBindingRevision();
		return true;
	};

	auto tryAdvanceAsyncDirectStorageReload = [&](const std::string& detail) -> bool {
		ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload");
		auto* filePath = std::get_if<std::string>(&m_initialStorage);
		if (filePath == nullptr || filePath->empty()) {
			return false;
		}

		if (m_directStorageReloadHandle) {
			TextureDirectStorageReloadJobState state = m_directStorageReloadHandle->state.load(std::memory_order_acquire);
			const uint32_t handleTargetTopMip = m_directStorageReloadHandle->targetTopMip.load(std::memory_order_acquire);
			if (handleTargetTopMip != desiredResidentTopMip &&
				(state == TextureDirectStorageReloadJobState::Queued ||
				 state == TextureDirectStorageReloadJobState::CreatingResource)) {
				m_directStorageReloadHandle->cancelRequested.store(true, std::memory_order_release);
				spdlog::info(
					"TextureAsset: canceled obsolete DirectStorage upload before submit texture='{}' desiredTopMip={} staleTargetTopMip={} residentTopMip={} pendingTopMip={} state={} detail='{}'",
					TextureTelemetryLabel(*this),
					desiredResidentTopMip,
					handleTargetTopMip,
					m_streamingState.residency.residentTopMip,
					m_streamingState.pendingTopMip,
					ToString(state),
					detail);
				m_directStorageReloadHandle.reset();
			}
		}

		if (m_directStorageReloadHandle) {
			ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload::PollExistingHandle");
			TextureDirectStorageReloadJobState state = m_directStorageReloadHandle->state.load(std::memory_order_acquire);
			if (state == TextureDirectStorageReloadJobState::Uploading) {
				DirectStorageAsyncRequestStatus requestStatus;
				{
					ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload::PollRequest");
					requestStatus = DirectStorageManager::GetInstance().PollRequest(m_directStorageReloadHandle->requestHandle);
				}
				if (requestStatus.state == DirectStorageAsyncRequestState::Ready) {
					m_directStorageReloadHandle->state.store(TextureDirectStorageReloadJobState::Ready, std::memory_order_release);
					state = TextureDirectStorageReloadJobState::Ready;
				}
				else if (requestStatus.state == DirectStorageAsyncRequestState::Failed || requestStatus.state == DirectStorageAsyncRequestState::Invalid) {
					{
						std::scoped_lock lock(m_directStorageReloadHandle->mutex);
						m_directStorageReloadHandle->error = requestStatus.message;
					}
					m_directStorageReloadHandle->state.store(TextureDirectStorageReloadJobState::Failed, std::memory_order_release);
					state = TextureDirectStorageReloadJobState::Failed;
				}
			}
			else if (state == TextureDirectStorageReloadJobState::Queued || state == TextureDirectStorageReloadJobState::CreatingResource) {
				return true;
			}

			const uint32_t handleTargetTopMip = m_directStorageReloadHandle->targetTopMip.load(std::memory_order_acquire);
			if (handleTargetTopMip != desiredResidentTopMip) {
				if (state == TextureDirectStorageReloadJobState::Ready || state == TextureDirectStorageReloadJobState::Failed) {
					m_directStorageReloadHandle.reset();
				}
				else {
					return true;
				}
			}
			else if (state == TextureDirectStorageReloadJobState::Ready) {
				ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload::AdoptReadyImage");
				std::shared_ptr<PixelBuffer> uploadedImage;
				{
					ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload::CopyReadyImage");
					std::scoped_lock lock(m_directStorageReloadHandle->mutex);
					uploadedImage = m_directStorageReloadHandle->uploadedImage;
				}
				m_directStorageReloadHandle.reset();
				if (!uploadedImage || !uploadedImage->HasValidBackingResource()) {
					return false;
				}

				{
					ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload::AdoptUploadedImage");
					AdoptUploadedImage(std::move(uploadedImage));
				}
				if (m_processingHandle) {
					const TextureProcessingJobState processingState = m_processingHandle->state.load(std::memory_order_acquire);
					if (processingState == TextureProcessingJobState::Ready || processingState == TextureProcessingJobState::Failed) {
						m_processingHandle.reset();
					}
				}
				{
					ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload::RecordUploadPath");
					RecordUploadPath(TextureUploadPathTelemetry::DirectStorageGpuDirect, detail);
				}
				if (!m_initialDataString.empty()) {
					m_initialStorage = m_initialDataString;
				}
				return true;
			}
			else if (state == TextureDirectStorageReloadJobState::Failed) {
				ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload::HandleFailure");
				std::string directStorageError;
				{
					std::scoped_lock lock(m_directStorageReloadHandle->mutex);
					directStorageError = m_directStorageReloadHandle->error;
				}
				if (!directStorageError.empty()) {
					if (directStorageError.find("block-compressed DDS textures do not use this DirectStorage GPU-direct path") != std::string::npos) {
						spdlog::debug(
							"TextureAsset: DirectStorage texture upload skipped for '{}' because {}",
							TextureTelemetryLabel(*this),
							directStorageError);
					}
					else {
						WarnOnce(
							"texture-directstorage-failed|" + TextureTelemetryLabel(*this) + "|" + directStorageError,
							"TextureAsset: DirectStorage texture upload failed for '" + TextureTelemetryLabel(*this) + "': " + directStorageError);
					}
				}
				m_directStorageReloadHandle.reset();
				return false;
			}
			else {
				return true;
			}
		}

		{
			ZoneScopedN("TextureAsset::EnsureUploaded::TryAdvanceAsyncDirectStorageReload::BeginUpload");
			ZoneValue(m_streamingState.streamingTextureID);
			if (!filePath->empty()) {
				ZoneText(filePath->c_str(), filePath->size());
			}
			const uint64_t beginUploadCount = g_directStorageTextureBeginUploadCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
			TracyPlot("SARP.Texture.DirectStorage.BeginUpload.Count", static_cast<int64_t>(beginUploadCount));
			TracyPlot("SARP.Texture.DirectStorage.BeginUpload.StreamingTextureID", static_cast<int64_t>(m_streamingState.streamingTextureID));
			TracyPlot("SARP.Texture.DirectStorage.BeginUpload.DesiredTopMip", static_cast<int64_t>(desiredResidentTopMip));
			TracyPlot("SARP.Texture.DirectStorage.BeginUpload.ResidentTopMip", static_cast<int64_t>(m_streamingState.residency.residentTopMip));
			TracyPlot("SARP.Texture.DirectStorage.BeginUpload.TotalMipCount", static_cast<int64_t>(m_streamingState.residency.totalMipCount));
			TracyPlot("SARP.Texture.DirectStorage.BeginUpload.FullWidth", static_cast<int64_t>(GetFullMip0Width()));
			TracyPlot("SARP.Texture.DirectStorage.BeginUpload.FullHeight", static_cast<int64_t>(GetFullMip0Height()));
			TracyPlot("SARP.Texture.DirectStorage.BeginUpload.ConditionedCache", static_cast<int64_t>(IsConditionedCacheFilePath(*filePath) ? 1 : 0));
			m_directStorageReloadHandle = IsConditionedCacheFilePath(*filePath)
				? BeginUploadConditionedCacheFilePathDirectToVRAMAsync(
					*filePath,
					desiredResidentTopMip,
					m_desc.hasRTV,
					m_desc.hasUAV)
				: BeginUploadDDSFilePathDirectToVRAMAsync(
					*filePath,
					m_meta.preferSRGB,
					desiredResidentTopMip,
					m_desc.hasRTV,
					m_desc.hasUAV);
		}
		return m_directStorageReloadHandle != nullptr;
	};

	auto promoteStreamingSourceToProcessedCachePath = [&](const std::string& cachePath) {
		ZoneScopedN("TextureAsset::EnsureUploaded::PromoteStreamingSourceToProcessedCachePath");
		if (cachePath.empty()) {
			return false;
		}

		m_initialDataString = cachePath;
		m_initialStorage = m_initialDataString;
		m_meta.isProcessingCacheArtifact = true;
		PrimeConditionedCacheResidentUploadMetadata();
		return true;
	};

	auto promoteStreamingSourceToProcessedCache = [&]() {
		ZoneScopedN("TextureAsset::EnsureUploaded::PromoteStreamingSourceToProcessedCache");
		const std::wstring cachePath = TextureProcessingManager::GetInstance().GetExistingCachePathForFile(m_meta);
		if (cachePath.empty()) {
			return false;
		}

		return promoteStreamingSourceToProcessedCachePath(std::filesystem::path(cachePath).string());
	};

	auto uploadSourceDataThroughFactory = [&](
		const std::shared_ptr<TextureSourceData>& sourceDataToUpload,
		TextureUploadPathTelemetry uploadPath,
		const std::string& detail) {
		ZoneScopedN("TextureAsset::EnsureUploaded::UploadSourceDataThroughFactory");
		if (!sourceDataToUpload) {
			return false;
		}

		const uint32_t residentMipCount = CalcMipCountFromDescription(sourceDataToUpload->desc);
		m_desc = sourceDataToUpload->desc;
		{
			ZoneScopedN("TextureAsset::EnsureUploaded::UploadSourceDataThroughFactory::RefreshStreamingState");
			RefreshStreamingStateFromDescription();
		}
		{
			ZoneScopedN("TextureAsset::EnsureUploaded::UploadSourceDataThroughFactory::CreateAlwaysResidentPixelBuffer");
			TracyPlot("SARP.Texture.MainThreadUpload.Subresources", static_cast<int64_t>(sourceDataToUpload->subresources.size()));
			m_image = factory.CreateAlwaysResidentPixelBuffer(
				sourceDataToUpload->desc,
				TextureFactory::TextureInitialData::FromBytes(sourceDataToUpload->subresources),
				m_name,
				ShouldPreserveAlphaCoverage(m_meta, sourceDataToUpload->desc),
				false,
				m_meta.processing.maxMipLevels);
			if (!m_meta.processing.isParticipatingMaterialTexture) m_publishedImage = m_image;
		}
		if (!m_image || !m_image->HasValidBackingResource()) {
			return false;
		}

		{
			ZoneScopedN("TextureAsset::EnsureUploaded::UploadSourceDataThroughFactory::UpdateResidencyState");
			SetResidentMipWindow(desiredResidentTopMip, residentMipCount);
			SetPendingTopMip(desiredResidentTopMip);
		}
		{
			ZoneScopedN("TextureAsset::EnsureUploaded::UploadSourceDataThroughFactory::RecordUploadPath");
			RecordUploadPath(uploadPath, detail);
		}
		m_hasUploadedFinalImage = true;
		m_hasUploadedPlaceholder = false;
		didMainThreadUpload = true;
		BumpBindingRevision();
		if (!m_initialDataString.empty()) {
			m_initialStorage = m_initialDataString;
		}
		else {
			m_initialStorage = std::monostate{};
		}
		return true;
	};

	auto requestAsyncSourceDataIfNeeded = [&](uint32_t targetTopMip, bool streamingEnabled, const char* reason) -> bool {
		ZoneScopedN("TextureAsset::EnsureUploaded::RequestAsyncSourceDataIfNeeded");
		if (reason != nullptr) {
			ZoneText(reason, std::strlen(reason));
		}
		if (m_reloadHandle) {
			return true;
		}

		auto* filePath = std::get_if<std::string>(&m_initialStorage);
		if (filePath == nullptr || filePath->empty()) {
			return false;
		}
		if (IsConditionedCacheFilePath(*filePath)) {
			return false;
		}

		m_reloadHandle = RequestReloadSourceDataAsync(
			*filePath,
			m_meta.preferSRGB,
			targetTopMip,
			streamingEnabled,
			reason != nullptr ? reason : "EnsureUploaded::RequestAsyncSourceDataIfNeeded");
		return m_reloadHandle != nullptr;
	};

	auto requestCpuSourceDataFallbackIfNeeded = [&](uint32_t targetTopMip, bool streamingEnabled, const char* reason) -> bool {
		ZoneScopedN("TextureAsset::EnsureUploaded::RequestCpuSourceDataFallbackIfNeeded");
		if (reason != nullptr) {
			ZoneText(reason, std::strlen(reason));
		}
		if (m_reloadHandle) {
			return true;
		}

		auto* filePath = std::get_if<std::string>(&m_initialStorage);
		if (filePath == nullptr || filePath->empty()) {
			return false;
		}

		m_reloadHandle = RequestReloadSourceDataAsync(
			*filePath,
			m_meta.preferSRGB,
			targetTopMip,
			streamingEnabled,
			reason != nullptr ? reason : "EnsureUploaded::RequestCpuSourceDataFallbackIfNeeded");
		return m_reloadHandle != nullptr;
	};

	const bool preferDirectStorageStreamingReload = [&]() {
		if (!needsStreamingReload) {
			return false;
		}

		auto* filePath = std::get_if<std::string>(&m_initialStorage);
		if (filePath == nullptr || (!IsDDSFilePath(*filePath) && !IsConditionedCacheFilePath(*filePath))) {
			return false;
		}

		return m_meta.isProcessingCacheArtifact ||
			m_lastReportedUploadPath == TextureUploadPathTelemetry::DirectStorageGpuDirect;
	}();

	if (preferDirectStorageStreamingReload &&
		tryAdvanceAsyncDirectStorageReload("texture residency reloaded asynchronously from DDS-backed source through DirectStorage GPU queue")) {
		return makeResult();
	}
	if (useConditionedCacheResidency && m_meta.isProcessingCacheArtifact && !m_processingHandle) {
		ZoneScopedN("TextureAsset::EnsureUploaded::ConditionedCacheResidency");
		if (tryAdvanceAsyncDirectStorageReload("texture residency uploaded asynchronously from conditioned cache through DirectStorage GPU queue")) {
			ensureProcessingPlaceholder("conditioned cache DirectStorage upload pending; placeholder texture uploaded");
			return makeResult();
		}

		if (!allowBlockingFallback) {
			requestCpuSourceDataFallbackIfNeeded(
				desiredResidentTopMip,
				m_streamingState.enabled,
				"ConditionedCacheResidency::DirectStorageUnavailable::AsyncCpuFallback");
			ensureProcessingPlaceholder("conditioned cache DirectStorage upload unavailable; async CPU fallback queued");
			return makeResult();
		}

		std::shared_ptr<TextureSourceData> fallbackSourceData;
		try {
			ZoneScopedN("TextureAsset::EnsureUploaded::ConditionedCacheResidency::BlockingBuildSourceData");
			fallbackSourceData = BuildSourceData("ConditionedCacheResidency::BlockingCpuFallback");
		}
		catch (const std::exception& ex) {
			spdlog::warn(
				"TextureAsset: failed to load conditioned cache CPU fallback for '{}': {}",
				TextureTelemetryLabel(*this),
				ex.what());
		}
		if (uploadSourceDataThroughFactory(
				fallbackSourceData,
				TextureUploadPathTelemetry::ProcessingCacheUpload,
				"conditioned cache DirectStorage residency unavailable, uploaded cache through TextureFactory")) {
			if (m_processingHandle) {
				const TextureProcessingJobState processingState = m_processingHandle->state.load(std::memory_order_acquire);
				if (processingState == TextureProcessingJobState::Ready || processingState == TextureProcessingJobState::Failed) {
					m_processingHandle.reset();
				}
			}
			return makeResult();
		}

		ensureProcessingPlaceholder("conditioned cache DirectStorage upload unavailable; placeholder texture kept resident");
		return makeResult();
	}

	std::shared_ptr<TextureSourceData> sourceData;
	bool reloadFailedThisFrame = false;
	// Processing needs the complete source mip chain regardless of the current streaming
	// residency target. Comparing its mip-0 job with desiredResidentTopMip discarded the
	// handle on every advance and queued unbounded duplicate background decodes.
	const uint32_t expectedReloadTopMip = shouldProcessTexture ? 0u : desiredResidentTopMip;
	if (m_reloadHandle && m_reloadHandle->targetTopMip != expectedReloadTopMip) {
		ZoneScopedN("TextureAsset::EnsureUploaded::ResetStaleReloadHandle");
		m_reloadHandle.reset();
	}
	if (!m_reloadHandle && shouldProcessTexture && !isConditionedCacheSource && std::holds_alternative<std::string>(m_initialStorage)) {
		ZoneScopedN("TextureAsset::EnsureUploaded::QueueProcessingSourceReload");
		const auto& filePath = std::get<std::string>(m_initialStorage);
		if (!filePath.empty()) {
			m_reloadHandle = RequestReloadSourceDataAsync(
				filePath,
				m_meta.preferSRGB,
				0u,
				false,
				"QueueProcessingSourceReload::OriginalFileForProcessing");
		}
	}
	if (!m_reloadHandle && !shouldProcessTexture && !useConditionedCacheResidency && std::holds_alternative<std::string>(m_initialStorage)) {
		ZoneScopedN("TextureAsset::EnsureUploaded::QueueNonProcessingSourceReload");
		const auto& filePath = std::get<std::string>(m_initialStorage);
		if (!filePath.empty() && !IsConditionedCacheFilePath(filePath)) {
			m_reloadHandle = RequestReloadSourceDataAsync(
				filePath,
				m_meta.preferSRGB,
				desiredResidentTopMip,
				m_streamingState.enabled,
				"QueueNonProcessingSourceReload::FileCpuReload");
		}
	}
	if (m_reloadHandle) {
		ZoneScopedN("TextureAsset::EnsureUploaded::PollReloadHandle");
		const TextureReloadJobState reloadState = m_reloadHandle->state.load(std::memory_order_acquire);
		if (reloadState == TextureReloadJobState::Ready) {
			ZoneScopedN("TextureAsset::EnsureUploaded::PollReloadHandle::Ready");
			uint32_t sourceTotalMipCount = 0u;
			uint32_t sourceFullWidth = 0u;
			uint32_t sourceFullHeight = 0u;
			{
				ZoneScopedN("TextureAsset::EnsureUploaded::PollReloadHandle::CopySourceData");
				std::scoped_lock lock(m_reloadHandle->mutex);
				sourceData = m_reloadHandle->sourceData;
				sourceTotalMipCount = m_reloadHandle->sourceTotalMipCount;
				sourceFullWidth = m_reloadHandle->sourceFullWidth;
				sourceFullHeight = m_reloadHandle->sourceFullHeight;
			}
			ApplySourceShapeHint(sourceFullWidth, sourceFullHeight, sourceTotalMipCount);
			m_reloadHandle.reset();
		}
		else if (reloadState == TextureReloadJobState::Failed) {
			ZoneScopedN("TextureAsset::EnsureUploaded::PollReloadHandle::Failed");
			std::string reloadError;
			{
				std::scoped_lock lock(m_reloadHandle->mutex);
				reloadError = m_reloadHandle->error;
			}
			if (!reloadError.empty()) {
				WarnOnce(
					"texture-reload-failed|" + TextureTelemetryLabel(*this) + "|" + reloadError,
					"TextureAsset: async source-data build failed for '" + TextureTelemetryLabel(*this) + "': " + reloadError);
			}
			m_reloadHandle.reset();
			reloadFailedThisFrame = true;
		}
	}

	if (m_processingFallbackRequested && sourceData) {
		ZoneScopedN("TextureAsset::EnsureUploaded::ProcessingFallbackRequested");
		m_meta.isProcessingCacheArtifact = false;
		if (uploadSourceDataThroughFactory(
				sourceData,
				TextureUploadPathTelemetry::ProcessingFailedFallback,
				"async processing failed earlier; uploaded asynchronously rebuilt source data through TextureFactory")) {
			m_processingFallbackRequested = false;
			return makeResult();
		}
		ensureProcessingPlaceholder("async processing failed earlier; rebuilt source data could not be uploaded yet");
		return makeResult();
	}

	if (shouldProcessTexture) {
		ZoneScopedN("TextureAsset::EnsureUploaded::ShouldProcessTextureInitial");
		if (!sourceData && (m_reloadHandle || reloadFailedThisFrame)) {
			if (allowBlockingFallback && m_meta.processing.allowCpuBootstrapBeforeAsyncProcessing && m_reloadHandle) {
				m_reloadHandle.reset();
			}
			else {
				ensureProcessingPlaceholder("async source-data build pending; placeholder texture uploaded");
				return makeResult();
			}
		}
		if (!sourceData && !allowBlockingFallback) {
			if (m_meta.processing.allowCpuBootstrapBeforeAsyncProcessing && !m_originalSourceBytes.empty()) {
				ZoneScopedN("TextureAsset::EnsureUploaded::ShouldProcessTextureInitial::InMemoryCpuBootstrap");
				sourceData = BuildSourceData("ShouldProcessTextureInitial::InMemoryCpuBootstrap");
			}
			else {
				requestAsyncSourceDataIfNeeded(
					0u,
					false,
					"ShouldProcessTextureInitial::AsyncProcessingSourceBuild");
				ensureProcessingPlaceholder("async source-data build queued; placeholder texture uploaded");
				return makeResult();
			}
		}
		if (!sourceData) {
			ZoneScopedN("TextureAsset::EnsureUploaded::ShouldProcessTextureInitial::BlockingBuildSourceData");
			sourceData = BuildSourceData("ShouldProcessTextureInitial::BlockingSourceBuild");
		}
		if (!m_processingHandle && !TextureProcessingManager::GetInstance().NeedsProcessing(*sourceData, m_meta)) {
			ZoneScopedN("TextureAsset::EnsureUploaded::ShouldProcessTextureInitial::NoProcessingNeeded");
			const uint32_t residentMipCount = CalcMipCountFromDescription(sourceData->desc);
			if (m_meta.processing.allowCpuBootstrapBeforeAsyncProcessing && !HasUsableImage()) {
				if (uploadSourceDataThroughFactory(
						sourceData,
						TextureUploadPathTelemetry::CpuImmediateUpload,
						"texture data uploaded through TextureFactory as bootstrap before asynchronous residency")) {
					return makeResult();
				}
			}
			if (needsStreamingReload) {
				if (tryAdvanceAsyncDirectStorageReload("texture residency reloaded asynchronously from file-backed DDS through DirectStorage GPU queue")) {
					return makeResult();
				}
			}
			else if (tryAdvanceAsyncDirectStorageReload("texture residency uploaded asynchronously from file-backed DDS through DirectStorage GPU queue")) {
				ensureProcessingPlaceholder("DirectStorage texture upload pending; fallback texture uploaded");
				return makeResult();
			}
			if (useConditionedCacheResidency && m_meta.isProcessingCacheArtifact) {
				if (uploadSourceDataThroughFactory(
						sourceData,
						TextureUploadPathTelemetry::ProcessingCacheUpload,
						"conditioned cache DirectStorage residency unavailable, uploaded cache through TextureFactory")) {
					return makeResult();
				}

				ensureProcessingPlaceholder("conditioned cache DirectStorage upload unavailable; placeholder texture kept resident");
				return makeResult();
			}
			if (useConditionedCacheResidency) {
				if (promoteStreamingSourceToProcessedCache()) {
					m_meta.isProcessingCacheArtifact = true;
					if (tryAdvanceAsyncDirectStorageReload("texture residency uploaded asynchronously from existing conditioned cache through DirectStorage GPU queue")) {
						ensureProcessingPlaceholder("conditioned cache DirectStorage upload pending; placeholder texture uploaded");
						return makeResult();
					}
					if (!allowBlockingFallback) {
						requestCpuSourceDataFallbackIfNeeded(
							desiredResidentTopMip,
							m_streamingState.enabled,
							"ExistingConditionedCache::DirectStorageUnavailable::AsyncCpuFallback");
						ensureProcessingPlaceholder("conditioned cache DirectStorage upload unavailable; async cache fallback queued");
						return makeResult();
					}
					std::shared_ptr<TextureSourceData> fallbackSourceData;
					try {
						ZoneScopedN("TextureAsset::EnsureUploaded::ExistingConditionedCache::BlockingBuildSourceData");
						fallbackSourceData = BuildSourceData("ExistingConditionedCache::BlockingCpuFallback");
					}
					catch (const std::exception& ex) {
						spdlog::warn(
							"TextureAsset: failed to load existing conditioned cache CPU fallback for '{}': {}",
							TextureTelemetryLabel(*this),
							ex.what());
					}
					if (uploadSourceDataThroughFactory(
							fallbackSourceData,
							TextureUploadPathTelemetry::ProcessingCacheUpload,
							"existing conditioned cache DirectStorage residency unavailable, uploaded cache through TextureFactory")) {
						return makeResult();
					}
					ensureProcessingPlaceholder("conditioned cache DirectStorage upload unavailable; placeholder texture kept resident");
					return makeResult();
				}

				// The texture is already in a renderable format, but material streaming still needs a
				// conditioned cache artifact so DirectStorage can populate residency windows later.
				// Fall through to RequestProcessing(), which will write/adopt the cache without
				// forcing this frame down the CPU immediate upload path.
			}
			else {
				ZoneScopedN("TextureAsset::EnsureUploaded::NoProcessingNeeded::ImmediateCpuUpload");
				m_desc = sourceData->desc;
				RefreshStreamingStateFromDescription();
				{
					ZoneScopedN("TextureAsset::EnsureUploaded::NoProcessingNeeded::CreateAlwaysResidentPixelBuffer");
					m_image = factory.CreateAlwaysResidentPixelBuffer(
						sourceData->desc,
						TextureFactory::TextureInitialData::FromBytes(sourceData->subresources),
						m_name,
						ShouldPreserveAlphaCoverage(m_meta, sourceData->desc),
						false,
						m_meta.processing.maxMipLevels);
					if (!m_meta.processing.isParticipatingMaterialTexture) m_publishedImage = m_image;
				}
				m_hasUploadedFinalImage = true;
				m_hasUploadedPlaceholder = false;
				SetResidentMipWindow(desiredResidentTopMip, residentMipCount);
				SetPendingTopMip(desiredResidentTopMip);
				RecordUploadPath(TextureUploadPathTelemetry::CpuImmediateUpload, "texture data uploaded through TextureFactory without async processing");
				didMainThreadUpload = true;
				BumpBindingRevision();
				if (!m_initialDataString.empty()) {
					m_initialStorage = m_initialDataString;
				}
				else {
					m_initialStorage = std::monostate{};
				}
				return makeResult();
			}
		}
	}

	if (shouldProcessTexture) {
		ZoneScopedN("TextureAsset::EnsureUploaded::ShouldProcessTexture");
		if (!sourceData && m_reloadHandle) {
			ensureProcessingPlaceholder("async reload source build pending; placeholder texture uploaded");

			return makeResult();
		}

		if (m_meta.processing.allowCpuBootstrapBeforeAsyncProcessing && !HasUsableImage() && sourceData) {
			ZoneScopedN("TextureAsset::EnsureUploaded::CpuBootstrapBeforeAsyncProcessing");
			if (!m_processingHandle) {
				try {
					ZoneScopedN("TextureAsset::EnsureUploaded::CpuBootstrapBeforeAsyncProcessing::RequestProcessing");
					m_processingHandle = TextureProcessingManager::GetInstance().RequestProcessing(sourceData, m_meta);
				}
				catch (const std::exception& ex) {
					spdlog::warn(
						"TextureAsset: failed to queue async processing while bootstrapping '{}': {}",
						TextureTelemetryLabel(*this),
						ex.what());
				}
			}
			if (uploadSourceDataThroughFactory(
					sourceData,
					TextureUploadPathTelemetry::CpuImmediateUpload,
					"texture data uploaded through TextureFactory as bootstrap while async processing/cache preparation continues")) {
				return makeResult();
			}
		}

		if (!m_processingHandle) {
			ZoneScopedN("TextureAsset::EnsureUploaded::RequestProcessing");
			if (!sourceData && !allowBlockingFallback) {
				requestAsyncSourceDataIfNeeded(
					0u,
					false,
					"RequestProcessing::AsyncProcessingSourceBuild");
				ensureProcessingPlaceholder("async processing source-data build queued; placeholder texture uploaded");
				return makeResult();
			}
			std::shared_ptr<TextureSourceData> processingSourceData;
			if (sourceData) {
				processingSourceData = sourceData;
			}
			else {
				ZoneScopedN("TextureAsset::EnsureUploaded::RequestProcessing::BlockingBuildProcessingSourceData");
				processingSourceData = BuildProcessingSourceData("RequestProcessing::BlockingProcessingSourceBuild");
			}
			{
				ZoneScopedN("TextureAsset::EnsureUploaded::RequestProcessing::EnqueueProcessing");
				m_processingHandle = TextureProcessingManager::GetInstance().RequestProcessing(processingSourceData, m_meta);
			}
		}

		if (m_processingHandle) {
			ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle");
			const TextureProcessingJobState state = m_processingHandle->state.load(std::memory_order_acquire);
			if (state == TextureProcessingJobState::GpuReadyToSubmit) {
				ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::SubmitBC7CompressionJob");
				if (factory.SubmitBC7CompressionJob(m_processingHandle, m_name)) {
					TextureProcessingManager::GetInstance().MarkGpuJobSubmitted(m_processingHandle);
				}
			}

			if (state == TextureProcessingJobState::Ready) {
				ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::Ready");
				std::shared_ptr<TextureSourceData> result;
				std::shared_ptr<PixelBuffer> uploadedImage;
				bool loadedFromCache = false;
				bool completedOnGpu = false;
				std::string conditionedCachePath;
				{
					ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::CopyReadyResult");
					std::scoped_lock lock(m_processingHandle->mutex);
					result = m_processingHandle->result;
					uploadedImage = m_processingHandle->uploadedImage;
					loadedFromCache = m_processingHandle->loadedFromCache;
					completedOnGpu = m_processingHandle->completedOnGpu;
					conditionedCachePath = m_processingHandle->conditionedCachePath;
				}
				const bool preferStreamedProcessingResult =
					m_streamingState.enabled &&
					isParticipatingMaterialTexture &&
					!conditionedCachePath.empty();

				if (!preferStreamedProcessingResult && uploadedImage && uploadedImage->HasValidBackingResource()) {
					ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::AdoptUploadedImageResult");
					m_desc = uploadedImage->GetDescription();
					m_meta.isProcessingCacheArtifact = loadedFromCache;
					if (!promoteStreamingSourceToProcessedCachePath(conditionedCachePath)) {
						promoteStreamingSourceToProcessedCache();
					}
					{
						ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::AdoptUploadedImage");
						AdoptUploadedImage(std::move(uploadedImage));
					}
					m_processingHandle.reset();
					RecordUploadPath(
						loadedFromCache ? TextureUploadPathTelemetry::ProcessingCacheUpload : TextureUploadPathTelemetry::AsyncProcessingReadyUpload,
						completedOnGpu
							? "async GPU processing completed and adopted resident PixelBuffer"
							: "async processing completed and adopted resident PixelBuffer");
					if (!m_initialDataString.empty()) {
						m_initialStorage = m_initialDataString;
					}
					else {
						m_initialStorage = std::monostate{};
					}
					return makeResult();
				}

				const bool canUseReadyConditionedCacheResidency =
					isParticipatingMaterialTexture &&
					canUseDirectStorageGpu &&
					!conditionedCachePath.empty();
				if (canUseReadyConditionedCacheResidency && promoteStreamingSourceToProcessedCachePath(conditionedCachePath)) {
					ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::ReadyConditionedCacheResidency");
					if (result) {
						m_desc = result->desc;
						RefreshStreamingStateFromDescription();
					}
					m_meta.isProcessingCacheArtifact = true;
					if (tryAdvanceAsyncDirectStorageReload(
							loadedFromCache
								? "processed texture cache hit; residency uploaded asynchronously from conditioned cache through DirectStorage GPU queue"
								: "async processing completed; residency uploaded asynchronously from conditioned cache through DirectStorage GPU queue")) {
						ensureProcessingPlaceholder("conditioned cache DirectStorage upload pending; placeholder texture uploaded");
						return makeResult();
					}

					std::shared_ptr<TextureSourceData> fallbackSourceData = result;
					if (fallbackSourceData && m_streamingState.enabled) {
						fallbackSourceData = ClipTextureSourceDataTopMip(fallbackSourceData, desiredResidentTopMip);
					}
					if (!fallbackSourceData && !allowBlockingFallback) {
						requestCpuSourceDataFallbackIfNeeded(
							desiredResidentTopMip,
							m_streamingState.enabled,
							"ReadyConditionedCacheResidency::DirectStorageUnavailable::AsyncCpuFallback");
						ensureProcessingPlaceholder("conditioned cache DirectStorage upload unavailable; async cache fallback queued");
						return makeResult();
					}
					if (!fallbackSourceData) {
						try {
							ZoneScopedN("TextureAsset::EnsureUploaded::ReadyConditionedCacheResidency::BlockingBuildSourceData");
							fallbackSourceData = BuildSourceData("ReadyConditionedCacheResidency::BlockingCpuFallback");
						}
						catch (const std::exception& ex) {
							spdlog::warn(
								"TextureAsset: failed to load conditioned cache CPU fallback for '{}': {}",
								TextureTelemetryLabel(*this),
								ex.what());
						}
					}
					if (uploadSourceDataThroughFactory(
							fallbackSourceData,
							loadedFromCache ? TextureUploadPathTelemetry::ProcessingCacheUpload : TextureUploadPathTelemetry::AsyncProcessingReadyUpload,
							loadedFromCache
								? "processed texture cache hit; DirectStorage residency unavailable, uploaded cache through TextureFactory"
								: "async processing completed; DirectStorage residency unavailable, uploaded processed result through TextureFactory")) {
						m_processingHandle.reset();
						return makeResult();
					}

					ensureProcessingPlaceholder("conditioned cache DirectStorage upload unavailable; placeholder texture kept resident");
					m_processingHandle.reset();
					return makeResult();
				}

				if (!conditionedCachePath.empty()) {
					ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::ReadyConditionedCacheFallback");
					m_meta.isProcessingCacheArtifact = loadedFromCache;
					if (promoteStreamingSourceToProcessedCachePath(conditionedCachePath)) {
						std::shared_ptr<TextureSourceData> fallbackSourceData = result;
						if (fallbackSourceData && m_streamingState.enabled) {
							fallbackSourceData = ClipTextureSourceDataTopMip(fallbackSourceData, desiredResidentTopMip);
						}
						if (!fallbackSourceData && !allowBlockingFallback) {
							requestCpuSourceDataFallbackIfNeeded(
								desiredResidentTopMip,
								m_streamingState.enabled,
								"ReadyConditionedCacheFallback::NoProcessingResult::AsyncCpuFallback");
							ensureProcessingPlaceholder("conditioned cache CPU upload fallback queued asynchronously");
							return makeResult();
						}
						if (!fallbackSourceData) {
							try {
								ZoneScopedN("TextureAsset::EnsureUploaded::ReadyConditionedCacheFallback::BlockingBuildSourceData");
								fallbackSourceData = BuildSourceData("ReadyConditionedCacheFallback::BlockingCpuFallback");
							}
							catch (const std::exception& ex) {
								spdlog::warn(
									"TextureAsset: failed to load ready conditioned cache CPU fallback for '{}': {}",
									TextureTelemetryLabel(*this),
									ex.what());
							}
						}
						if (uploadSourceDataThroughFactory(
								fallbackSourceData,
								loadedFromCache ? TextureUploadPathTelemetry::ProcessingCacheUpload : TextureUploadPathTelemetry::AsyncProcessingReadyUpload,
								loadedFromCache
									? "processed texture conditioned cache hit; uploaded sibling DDS through TextureFactory"
									: "async processing completed with conditioned cache; uploaded sibling DDS through TextureFactory")) {
							m_processingHandle.reset();
							return makeResult();
						}
					}
				}

				if (result) {
					ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::ReadyResultCpuUpload");
					if (m_streamingState.enabled) {
						result = ClipTextureSourceDataTopMip(result, desiredResidentTopMip);
					}
					m_meta.isProcessingCacheArtifact = loadedFromCache;
					if (!promoteStreamingSourceToProcessedCachePath(conditionedCachePath)) {
						promoteStreamingSourceToProcessedCache();
					}
					if (loadedFromCache) {
						m_meta.fileType = ImageFiletype::DDS;
						m_meta.loader = ImageLoader::DirectXTex;
					}
					if (uploadSourceDataThroughFactory(
							result,
							loadedFromCache ? TextureUploadPathTelemetry::ProcessingCacheUpload : TextureUploadPathTelemetry::AsyncProcessingReadyUpload,
							loadedFromCache
								? "async processing completed from DDS cache artifact"
								: "async processing completed and uploaded through TextureFactory")) {
						m_processingHandle.reset();
						return makeResult();
					}
					return makeResult();
				}
			}
			else if (state == TextureProcessingJobState::Failed) {
				ZoneScopedN("TextureAsset::EnsureUploaded::PollProcessingHandle::Failed");
				std::string processingError;
				{
					std::scoped_lock lock(m_processingHandle->mutex);
					processingError = m_processingHandle->error;
				}
				if (!useConditionedCacheResidency && tryAdvanceAsyncDirectStorageReload(
						processingError.empty()
							? "async processing failed; residency restored asynchronously from file-backed DDS through DirectStorage GPU queue"
							: "async processing failed ('" + processingError + "'); residency restored asynchronously from file-backed DDS through DirectStorage GPU queue")) {
					ensureProcessingPlaceholder(
						processingError.empty()
							? "async processing failed; DirectStorage fallback upload pending"
							: "async processing failed ('" + processingError + "'); DirectStorage fallback upload pending");
					return makeResult();
				}
				if (!allowBlockingFallback) {
					m_processingHandle.reset();
					m_processingFallbackRequested = true;
					requestAsyncSourceDataIfNeeded(
						desiredResidentTopMip,
						m_streamingState.enabled,
						"ProcessingFailed::AsyncOriginalDdsFallback");
					ensureProcessingPlaceholder(
						processingError.empty()
							? "async processing failed; async CPU fallback queued"
							: "async processing failed ('" + processingError + "'); async CPU fallback queued");
					return makeResult();
				}
				if (useConditionedCacheResidency) {
					ZoneScopedN("TextureAsset::EnsureUploaded::ProcessingFailed::ConditionedFallback");
					try {
						ZoneScopedN("TextureAsset::EnsureUploaded::ProcessingFailed::ConditionedFallback::BlockingBuildAndUpload");
						const auto fallbackSourceData = BuildSourceData("ProcessingFailed::ConditionedBlockingFallback");
						const uint32_t residentMipCount = CalcMipCountFromDescription(fallbackSourceData->desc);
						m_meta.isProcessingCacheArtifact = false;
						m_desc = fallbackSourceData->desc;
						m_image = factory.CreateAlwaysResidentPixelBuffer(
							fallbackSourceData->desc,
							TextureFactory::TextureInitialData::FromBytes(fallbackSourceData->subresources),
							m_name,
							ShouldPreserveAlphaCoverage(m_meta, fallbackSourceData->desc),
							false,
							m_meta.processing.maxMipLevels);
						if (!m_meta.processing.isParticipatingMaterialTexture) m_publishedImage = m_image;
						RefreshStreamingStateFromDescription();
						SetResidentMipWindow(desiredResidentTopMip, residentMipCount);
						SetPendingTopMip(desiredResidentTopMip);
						RecordUploadPath(
							TextureUploadPathTelemetry::ProcessingFailedFallback,
							processingError.empty()
								? "async processing failed; conditioned residency unavailable, uploaded original bytes through TextureFactory"
								: "async processing failed ('" + processingError + "'); conditioned residency unavailable, uploaded original bytes through TextureFactory");
						m_hasUploadedFinalImage = true;
						m_hasUploadedPlaceholder = false;
						didMainThreadUpload = true;
						m_processingFallbackRequested = false;
						BumpBindingRevision();
					}
					catch (const std::exception& ex) {
						spdlog::warn(
							"TextureAsset: failed to upload processing failure fallback for '{}': {}",
							TextureTelemetryLabel(*this),
							ex.what());
						ensureProcessingPlaceholder(
							processingError.empty()
								? "async processing failed; keeping placeholder texture"
								: "async processing failed ('" + processingError + "'); keeping placeholder texture");
					}
					m_processingHandle.reset();
					return makeResult();
				}
				std::shared_ptr<TextureSourceData> fallbackSourceData;
				{
					ZoneScopedN("TextureAsset::EnsureUploaded::ProcessingFailed::BlockingBuildSourceData");
					fallbackSourceData = BuildSourceData("ProcessingFailed::BlockingOriginalFallback");
				}
				const uint32_t residentMipCount = CalcMipCountFromDescription(fallbackSourceData->desc);
				m_meta.isProcessingCacheArtifact = false;
				m_desc = fallbackSourceData->desc;
				{
					ZoneScopedN("TextureAsset::EnsureUploaded::ProcessingFailed::CreateAlwaysResidentPixelBuffer");
					m_image = factory.CreateAlwaysResidentPixelBuffer(
						fallbackSourceData->desc,
						TextureFactory::TextureInitialData::FromBytes(fallbackSourceData->subresources),
						m_name,
						ShouldPreserveAlphaCoverage(m_meta, fallbackSourceData->desc),
						false,
						m_meta.processing.maxMipLevels);
					if (!m_meta.processing.isParticipatingMaterialTexture) m_publishedImage = m_image;
				}
				RefreshStreamingStateFromDescription();
				SetResidentMipWindow(desiredResidentTopMip, residentMipCount);
				SetPendingTopMip(desiredResidentTopMip);
				RecordUploadPath(
					TextureUploadPathTelemetry::ProcessingFailedFallback,
					processingError.empty()
						? "async processing failed; uploaded original bytes through TextureFactory"
						: "async processing failed ('" + processingError + "'); uploaded original bytes through TextureFactory");
				m_hasUploadedFinalImage = true;
				m_hasUploadedPlaceholder = false;
				didMainThreadUpload = true;
				m_processingFallbackRequested = false;
				BumpBindingRevision();
				m_processingHandle.reset();
				return makeResult();
			}
		}

		ensureProcessingPlaceholder("async processing pending; placeholder texture uploaded");

		return makeResult();
	}

	if (m_hasUploadedPlaceholder && m_directStorageReloadHandle) {
		ZoneScopedN("TextureAsset::EnsureUploaded::PlaceholderDirectStorageAdvance");
		if (tryAdvanceAsyncDirectStorageReload("fallback texture kept resident while DirectStorage upload advances asynchronously")) {
			return makeResult();
		}

		if (!sourceData && !allowBlockingFallback) {
			if (isConditionedCacheSource) {
				requestCpuSourceDataFallbackIfNeeded(
					desiredResidentTopMip,
					m_streamingState.enabled,
					"PlaceholderDirectStorageAdvance::ConditionedCacheAsyncCpuFallback");
			}
			else {
				requestAsyncSourceDataIfNeeded(
					desiredResidentTopMip,
					m_streamingState.enabled,
					"PlaceholderDirectStorageAdvance::DdsAsyncCpuFallback");
			}
			ensureProcessingPlaceholder("DirectStorage fallback did not complete; async CPU fallback queued");
			return makeResult();
		}
	}

	if (m_hasUploadedPlaceholder && sourceData) {
		ZoneScopedN("TextureAsset::EnsureUploaded::PlaceholderReplaceWithSourceData");
		if (uploadSourceDataThroughFactory(
				sourceData,
				TextureUploadPathTelemetry::CpuImmediateUpload,
				"placeholder replaced with asynchronously rebuilt source data")) {
			return makeResult();
		}
		ensureProcessingPlaceholder("rebuilt source data could not be uploaded; keeping placeholder resident");
		return makeResult();
	}

	if (!HasUsableImage()) {
		ZoneScopedN("TextureAsset::EnsureUploaded::NoUsableImageFallback");
		if (tryAdvanceAsyncDirectStorageReload("texture uploaded asynchronously from file-backed DDS through DirectStorage GPU queue without preprocessing")) {
			ensureProcessingPlaceholder("DirectStorage texture upload pending; fallback texture uploaded");
			return makeResult();
		}
		if (!sourceData && m_reloadHandle) {
			ensureProcessingPlaceholder("async reload source build pending; fallback texture uploaded");
			return makeResult();
		}
		if (!sourceData && !allowBlockingFallback) {
			if (isConditionedCacheSource) {
				requestCpuSourceDataFallbackIfNeeded(
					desiredResidentTopMip,
					m_streamingState.enabled,
					"NoUsableImageFallback::ConditionedCacheAsyncCpuFallback");
			}
			else {
				requestAsyncSourceDataIfNeeded(
					desiredResidentTopMip,
					m_streamingState.enabled,
					"NoUsableImageFallback::DdsAsyncCpuFallback");
			}
			ensureProcessingPlaceholder("async source-data build queued; fallback texture uploaded");
			return makeResult();
		}
		std::shared_ptr<TextureSourceData> immediateSourceData;
		if (sourceData) {
			immediateSourceData = sourceData;
		}
		else {
			ZoneScopedN("TextureAsset::EnsureUploaded::NoUsableImageFallback::BlockingBuildSourceData");
			immediateSourceData = BuildSourceData("NoUsableImageFallback::BlockingSourceBuild");
		}
		const uint32_t residentMipCount = CalcMipCountFromDescription(immediateSourceData->desc);
		m_desc = immediateSourceData->desc;
		{
			ZoneScopedN("TextureAsset::EnsureUploaded::NoUsableImageFallback::CreateAlwaysResidentPixelBuffer");
			m_image = factory.CreateAlwaysResidentPixelBuffer(
				immediateSourceData->desc,
				TextureFactory::TextureInitialData::FromBytes(immediateSourceData->subresources),
				m_name,
				ShouldPreserveAlphaCoverage(m_meta, immediateSourceData->desc),
				false,
				m_meta.processing.maxMipLevels);
			if (!m_meta.processing.isParticipatingMaterialTexture) m_publishedImage = m_image;
		}
		RecordUploadPath(TextureUploadPathTelemetry::CpuImmediateUpload, "texture uploaded through TextureFactory without preprocessing");
		m_hasUploadedFinalImage = true;
		m_hasUploadedPlaceholder = false;
		didMainThreadUpload = true;
		RefreshStreamingStateFromDescription();
		SetResidentMipWindow(desiredResidentTopMip, residentMipCount);
		SetPendingTopMip(desiredResidentTopMip);
		BumpBindingRevision();
		if (!m_initialDataString.empty()) {
			m_initialStorage = m_initialDataString;
		}
		else {
			m_initialStorage = std::monostate{};
		}
	}
	return makeResult();
}
