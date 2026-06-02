#include "Import/CLodCache.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <cwctype>

#include <boost/container_hash/hash.hpp>
#include <spdlog/spdlog.h>

#if BASICRENDERER_HAS_DIRECTSTORAGE
#include "Managers/Singletons/DirectStorageManager.h"
#endif
#include "Utilities/CachePathUtilities.h"

#include "../shaders/Common/defines.h"

namespace CLodCache {

	namespace {
		std::wstring SanitizeFolderName(const std::wstring& input)
		{
			if (input.empty()) {
				return L"scene";
			}

			std::wstring out;
			out.reserve(input.size());
			for (wchar_t ch : input) {
				if (std::iswalnum(ch) != 0 || ch == L'_' || ch == L'-') {
					out.push_back(ch);
				}
				else {
					out.push_back(L'_');
				}
			}

			if (out.empty()) {
				return L"scene";
			}

			return out;
		}

		std::wstring BuildSceneCacheSubdirectory(const std::string& sourceIdentifier)
		{
			std::wstring stem = L"scene";
			if (!sourceIdentifier.empty()) {
				std::filesystem::path sourcePath = s2ws(sourceIdentifier);
				std::wstring sourceStem = sourcePath.stem().wstring();
				if (!sourceStem.empty()) {
					stem = sourceStem;
				}
			}

			stem = SanitizeFolderName(stem);

			size_t hashSeed = 0;
			boost::hash_combine(hashSeed, sourceIdentifier);

			std::wstringstream folderName;
			folderName << stem << L"_" << std::hex << hashSeed;
			return L"clod\\" + folderName.str();
		}

		std::wstring GetCacheFilePathBySource(const std::wstring& fileName, const std::string& sourceIdentifier)
		{
			return GetCacheFilePath(fileName, BuildSceneCacheSubdirectory(sourceIdentifier));
		}

		template<typename T>
		void WritePod(std::vector<std::byte>& out, const T& value)
		{
			const std::byte* ptr = reinterpret_cast<const std::byte*>(&value);
			out.insert(out.end(), ptr, ptr + sizeof(T));
		}

		template<typename T>
		bool ReadPod(const std::vector<std::byte>& in, size_t& offset, T& out)
		{
			if (offset + sizeof(T) > in.size()) {
				return false;
			}
			std::memcpy(&out, in.data() + offset, sizeof(T));
			offset += sizeof(T);
			return true;
		}

		template<typename T>
		void WriteVectorPod(std::vector<std::byte>& out, const std::vector<T>& values)
		{
			const uint64_t count = static_cast<uint64_t>(values.size());
			WritePod(out, count);
			if (!values.empty()) {
				const std::byte* ptr = reinterpret_cast<const std::byte*>(values.data());
				out.insert(out.end(), ptr, ptr + sizeof(T) * values.size());
			}
		}

		template<typename T>
		bool ReadVectorPod(const std::vector<std::byte>& in, size_t& offset, std::vector<T>& values)
		{
			uint64_t count = 0;
			if (!ReadPod(in, offset, count)) {
				return false;
			}
			if (count > (std::numeric_limits<size_t>::max)()) {
				return false;
			}
			const size_t byteCount = sizeof(T) * static_cast<size_t>(count);
			if (offset + byteCount > in.size()) {
				return false;
			}
			values.resize(static_cast<size_t>(count));
			if (byteCount > 0) {
				std::memcpy(values.data(), in.data() + offset, byteCount);
			}
			offset += byteCount;
			return true;
		}

		void WriteString(std::vector<std::byte>& out, const std::string& value)
		{
			const uint64_t length = static_cast<uint64_t>(value.size());
			WritePod(out, length);
			if (!value.empty()) {
				const std::byte* ptr = reinterpret_cast<const std::byte*>(value.data());
				out.insert(out.end(), ptr, ptr + value.size());
			}
		}

		bool ReadString(const std::vector<std::byte>& in, size_t& offset, std::string& value)
		{
			uint64_t length = 0;
			if (!ReadPod(in, offset, length)) {
				return false;
			}
			if (length > (std::numeric_limits<size_t>::max)()) {
				return false;
			}
			if (offset + static_cast<size_t>(length) > in.size()) {
				return false;
			}
			value.resize(static_cast<size_t>(length));
			if (length > 0) {
				std::memcpy(value.data(), in.data() + offset, static_cast<size_t>(length));
			}
			offset += static_cast<size_t>(length);
			return true;
		}

		std::vector<std::byte> SerializeMetadata(
			uint64_t buildConfigHash,
			const ClusterLODPrebuiltData& prebuiltData,
			const std::vector<ClusterLODGroupDiskLocator>& pageDiskLocators,
			const ClusterLODCacheSource& cacheSource)
		{
			std::vector<std::byte> out;
			WritePod(out, kSchemaVersion);
			WritePod(out, buildConfigHash);

			WriteVectorPod(out, prebuiltData.groups);
			WriteVectorPod(out, prebuiltData.segments);
			WriteVectorPod(out, prebuiltData.segmentBounds);
			WritePod(out, prebuiltData.objectBoundingSphere);
			const uint8_t hasInlineGroupChunks = prebuiltData.groupChunks.empty() ? 0u : 1u;
			WritePod(out, hasInlineGroupChunks);
			if (hasInlineGroupChunks != 0u) {
				WriteVectorPod(out, prebuiltData.groupChunks);
			}
			WriteVectorPod(out, prebuiltData.groupDiskLocators);
			WriteVectorPod(out, pageDiskLocators);
			WriteVectorPod(out, prebuiltData.groupPageReferences);
			WriteVectorPod(out, prebuiltData.groupPageReferenceOffsets);
			WritePod(out, prebuiltData.trianglePageCount);
			WritePod(out, prebuiltData.voxelPageBase);
			WritePod(out, prebuiltData.voxelPageCount);
			WriteString(out, cacheSource.sourceIdentifier);
			WriteString(out, cacheSource.primPath);
			WriteString(out, cacheSource.subsetName);
			WritePod(out, cacheSource.buildConfigHash);
			WriteString(out, ws2s(cacheSource.containerFileName));
			WriteVectorPod(out, prebuiltData.nodes);
			WriteVectorPod(out, prebuiltData.lodNodeRanges);
			WriteVectorPod(out, prebuiltData.lodLevelRoots);
			WritePod(out, prebuiltData.maxDepth);
			WritePod(out, prebuiltData.maxTraversalDepth);

			return out;
		}

		bool DeserializeMetadata(const std::vector<std::byte>& blob, CacheData& out)
		{
			size_t offset = 0;
			if (!ReadPod(blob, offset, out.schemaVersion)) return false;
			if (out.schemaVersion != kSchemaVersion) return false;
			if (!ReadPod(blob, offset, out.buildConfigHash)) return false;

			if (!ReadVectorPod(blob, offset, out.prebuiltData.groups)) return false;
			if (!ReadVectorPod(blob, offset, out.prebuiltData.segments)) return false;
			if (!ReadVectorPod(blob, offset, out.prebuiltData.segmentBounds)) return false;
			if (!ReadPod(blob, offset, out.prebuiltData.objectBoundingSphere)) return false;

			uint8_t hasInlineGroupChunks = 0u;
			if (!ReadPod(blob, offset, hasInlineGroupChunks)) return false;
			if (hasInlineGroupChunks != 0u) {
				if (!ReadVectorPod(blob, offset, out.prebuiltData.groupChunks)) return false;
			}
			else {
				out.prebuiltData.groupChunks.clear();
			}
			if (!ReadVectorPod(blob, offset, out.prebuiltData.groupDiskLocators)) return false;
			if (!ReadVectorPod(blob, offset, out.prebuiltData.pageDiskLocators)) return false;
			if (!ReadVectorPod(blob, offset, out.prebuiltData.groupPageReferences)) return false;
			if (!ReadVectorPod(blob, offset, out.prebuiltData.groupPageReferenceOffsets)) return false;
			if (!ReadPod(blob, offset, out.prebuiltData.trianglePageCount)) return false;
			if (!ReadPod(blob, offset, out.prebuiltData.voxelPageBase)) return false;
			if (!ReadPod(blob, offset, out.prebuiltData.voxelPageCount)) return false;
			if (!ReadString(blob, offset, out.prebuiltData.cacheSource.sourceIdentifier)) return false;
			if (!ReadString(blob, offset, out.prebuiltData.cacheSource.primPath)) return false;
			if (!ReadString(blob, offset, out.prebuiltData.cacheSource.subsetName)) return false;
			if (!ReadPod(blob, offset, out.prebuiltData.cacheSource.buildConfigHash)) return false;
			std::string containerFileName;
			if (!ReadString(blob, offset, containerFileName)) return false;
			out.prebuiltData.cacheSource.containerFileName = s2ws(containerFileName);
			if (!ReadVectorPod(blob, offset, out.prebuiltData.nodes)) return false;
			if (!ReadVectorPod(blob, offset, out.prebuiltData.lodNodeRanges)) return false;
			if (!ReadVectorPod(blob, offset, out.prebuiltData.lodLevelRoots)) return false;
			if (!ReadPod(blob, offset, out.prebuiltData.maxDepth)) return false;
			if (!ReadPod(blob, offset, out.prebuiltData.maxTraversalDepth)) return false;

			return offset == blob.size();
		}

		static constexpr uint32_t kContainerMagic = 0x444F4C43u; // CLOD
		static constexpr uint32_t kMetadataMagic = 0x4D4C4F43u; // COLM
		static constexpr uint32_t kMetadataVersion = 1u;

		struct ContainerHeader {
			uint32_t magic = kContainerMagic;
			uint32_t version = 4;
			uint32_t reserved = 0;
			uint32_t pageCount = 0;
		};

		bool ReadPageBlobDirect(std::ifstream& file,
			const ClusterLODGroupDiskLocator& locator,
			std::vector<std::byte>& outBlob)
		{
			if (locator.blobOffset > static_cast<uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
				return false;
			}
			file.seekg(static_cast<std::streamoff>(locator.blobOffset), std::ios::beg);
			if (!file.good()) {
				return false;
			}
			outBlob.resize(locator.blobSizeBytes);
			if (locator.blobSizeBytes == 0u) {
				return true;
			}
			file.read(reinterpret_cast<char*>(outBlob.data()), static_cast<std::streamsize>(locator.blobSizeBytes));
			return file.good();
		}

		std::wstring BuildGroupContainerFileName(const CacheKey& key, uint64_t buildConfigHash)
		{
			size_t hashSeed = 0;
			boost::hash_combine(hashSeed, key.sourceIdentifier);
			boost::hash_combine(hashSeed, key.primPath);
			boost::hash_combine(hashSeed, key.subsetName);
			boost::hash_combine(hashSeed, buildConfigHash);

			std::stringstream ss;
			ss << "clod_" << std::hex << hashSeed << ".clodbin";
			return s2ws(ss.str());
		}

		template<typename T>
		bool TryGetByteSize(const std::vector<T>& values, uint32_t& outSizeBytes)
		{
			const uint64_t sizeBytes64 = static_cast<uint64_t>(values.size()) * static_cast<uint64_t>(sizeof(T));
			if (sizeBytes64 > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
				return false;
			}
			outSizeBytes = static_cast<uint32_t>(sizeBytes64);
			return true;
		}

		bool WriteMetadataBlob(const std::wstring& cachePath, const std::vector<std::byte>& blob)
		{
			static std::atomic<uint64_t> tempCounter{ 0 };
			static std::array<std::mutex, 64> replacementLocks;

			const std::filesystem::path finalPath(cachePath);
			std::wstringstream tempName;
			tempName << L".clodmeta."
					 << std::hash<std::thread::id>{}(std::this_thread::get_id())
					 << L"." << tempCounter.fetch_add(1, std::memory_order_relaxed)
					 << L".tmp";
			const auto tempPath = (finalPath.parent_path() / tempName.str()).wstring();
			auto cleanupTemp = [&tempPath]()
			{
				std::error_code cleanupEc;
				std::filesystem::remove(tempPath, cleanupEc);
			};
			{
				std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
				if (!file.is_open()) {
					spdlog::warn("Failed to open temporary CLod metadata file: {}", ws2s(tempPath));
					return false;
				}

				if (blob.size() > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
					cleanupTemp();
					return false;
				}
				const uint32_t magic = kMetadataMagic;
				const uint32_t version = kMetadataVersion;
				const uint64_t blobSize = static_cast<uint64_t>(blob.size());
				file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
				file.write(reinterpret_cast<const char*>(&version), sizeof(version));
				file.write(reinterpret_cast<const char*>(&blobSize), sizeof(blobSize));
				if (!blob.empty()) {
					file.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
				}
				if (!file.good()) {
					cleanupTemp();
					return false;
				}
			}

			std::error_code ec;
			const auto lockIndex = std::hash<std::wstring>{}(cachePath) % replacementLocks.size();
			{
				std::scoped_lock lock(replacementLocks[lockIndex]);
				for (int attempt = 0; attempt < 16; ++attempt) {
					std::filesystem::remove(cachePath, ec);
					ec.clear();
					std::filesystem::rename(tempPath, cachePath, ec);
					if (!ec) {
						return true;
					}
					std::error_code existsEc;
					if (std::filesystem::exists(cachePath, existsEc)) {
						spdlog::debug("CLod metadata already exists after replace race: {}", ws2s(cachePath));
						cleanupTemp();
						return true;
					}

					if (attempt < 15) {
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
					}
				}
				if (ec) {
					spdlog::warn("Failed to replace CLod metadata '{}' with '{}': {}",
						ws2s(cachePath), ws2s(tempPath), ec.message());
					cleanupTemp();
					return false;
				}
			}
			return true;
		}

		bool ReadMetadataBlob(const std::wstring& cachePath, std::vector<std::byte>& blob)
		{
			std::ifstream file(cachePath, std::ios::binary);
			if (!file.is_open()) {
				return false;
			}

			uint32_t magic = 0;
			uint32_t version = 0;
			uint64_t blobSize = 0;
			file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
			file.read(reinterpret_cast<char*>(&version), sizeof(version));
			file.read(reinterpret_cast<char*>(&blobSize), sizeof(blobSize));
			if (!file.good() || magic != kMetadataMagic || version != kMetadataVersion) {
				return false;
			}
			if (blobSize > static_cast<uint64_t>((std::numeric_limits<std::size_t>::max)())) {
				return false;
			}
			if (blobSize > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
				return false;
			}

			blob.resize(static_cast<std::size_t>(blobSize));
			if (!blob.empty()) {
				file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
			}
			return file.good();
		}

		template<typename T>
		bool WriteVectorRaw(std::ofstream& file, const std::vector<T>& values)
		{
			if (values.empty()) {
				return true;
			}
			file.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
			return file.good();
		}

		bool SaveContainerPayload(
			const std::wstring& containerPath,
			const ClusterLODPrebuiltData& prebuiltData,
			const ClusterLODCacheBuildPayload& payload,
			std::vector<ClusterLODGroupDiskLocator>& outPageLocators)
		{
			(void)prebuiltData;
			const std::vector<std::vector<std::byte>> emptyPageBlobs;
			const auto& pageBlobs = payload.meshPageBlobs != nullptr ? *payload.meshPageBlobs : emptyPageBlobs;
			const uint32_t pageCount = static_cast<uint32_t>(pageBlobs.size());
			outPageLocators.assign(pageCount, {});

			std::ofstream file(containerPath, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				return false;
			}

			ContainerHeader header{};
			header.pageCount = pageCount;
			file.write(reinterpret_cast<const char*>(&header), sizeof(header));
			if (!file.good()) {
				return false;
			}

			const std::streamoff directoryOffset = static_cast<std::streamoff>(file.tellp());
			if (pageCount > 0) {
				std::vector<ClusterLODGroupDiskLocator> emptyDirectory(pageCount);
				file.write(reinterpret_cast<const char*>(emptyDirectory.data()), static_cast<std::streamsize>(emptyDirectory.size() * sizeof(ClusterLODGroupDiskLocator)));
				if (!file.good()) {
					return false;
				}
			}

			for (uint32_t pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
				const uint64_t blobOffset64 = static_cast<uint64_t>(file.tellp());
				const auto& pageBlob = pageBlobs[pageIndex];
				if (!pageBlob.empty()) {
					file.write(reinterpret_cast<const char*>(pageBlob.data()),
						static_cast<std::streamsize>(pageBlob.size()));
					if (!file.good()) return false;
				}

				const uint64_t blobEnd64 = static_cast<uint64_t>(file.tellp());
				if (blobEnd64 < blobOffset64) return false;
				const uint64_t blobSize64 = blobEnd64 - blobOffset64;
				if (blobSize64 > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) return false;

				auto& locator = outPageLocators[pageIndex];
				locator.blobOffset = blobOffset64;
				locator.blobSizeBytes = static_cast<uint32_t>(blobSize64);
				locator.reserved = 0;
			}

			if (pageCount > 0) {
				file.seekp(directoryOffset, std::ios::beg);
				if (!file.good()) return false;
				file.write(reinterpret_cast<const char*>(outPageLocators.data()), static_cast<std::streamsize>(outPageLocators.size() * sizeof(ClusterLODGroupDiskLocator)));
				if (!file.good()) return false;
			}

			return file.good();
		}

		template<typename T>
		bool ReadVectorRaw(std::ifstream& file, uint32_t sizeBytes, std::vector<T>& outValues)
		{
			if ((sizeBytes % sizeof(T)) != 0u) {
				return false;
			}
			outValues.resize(static_cast<size_t>(sizeBytes / sizeof(T)));
			if (sizeBytes == 0u) {
				return true;
			}
			file.read(reinterpret_cast<char*>(outValues.data()), static_cast<std::streamsize>(sizeBytes));
			return file.good();
		}

	}

	namespace {
		bool SaveImpl(const CacheKey& key, uint64_t buildConfigHash, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload, ClusterLODPrebuiltData* outSavedPrebuiltData)
		{
			const std::wstring fileName = BuildCacheFileName(key, buildConfigHash);
			const std::wstring cachePath = GetCacheFilePathBySource(fileName, key.sourceIdentifier);
			const std::wstring containerFileName = BuildGroupContainerFileName(key, buildConfigHash);
			const std::wstring containerPath = GetCacheFilePathBySource(containerFileName, key.sourceIdentifier);
			if (std::filesystem::exists(cachePath)) {
				spdlog::warn(
					"Skipping CLod cache save because metadata file already exists but did not load cleanly: {}",
					ws2s(cachePath));
				return false;
			}

			spdlog::info("CLodCache::SaveImpl  metadata='{}' container='{}'",
				ws2s(cachePath), ws2s(containerPath));

			std::vector<ClusterLODGroupDiskLocator> pageDiskLocators;
			if (!SaveContainerPayload(containerPath, prebuiltData, payload, pageDiskLocators)) {
				spdlog::warn("Failed to write CLod container payload: {}", ws2s(containerPath));
				return false;
			}

			ClusterLODCacheSource cacheSource = prebuiltData.cacheSource;
			cacheSource.sourceIdentifier = key.sourceIdentifier;
			cacheSource.primPath = key.primPath;
			cacheSource.subsetName = key.subsetName;
			cacheSource.buildConfigHash = buildConfigHash;
			cacheSource.containerFileName = containerFileName;

			auto blob = SerializeMetadata(buildConfigHash, prebuiltData, pageDiskLocators, cacheSource);
			if (!WriteMetadataBlob(cachePath, blob)) {
				spdlog::warn("Failed to write CLod cache metadata: {}", ws2s(cachePath));
				return false;
			}

			if (outSavedPrebuiltData != nullptr) {
				*outSavedPrebuiltData = prebuiltData;
				outSavedPrebuiltData->pageDiskLocators = std::move(pageDiskLocators);
				outSavedPrebuiltData->cacheSource = std::move(cacheSource);
			}

			return true;
		}
	}

	uint64_t ComputeBuildConfigHash()
	{
		size_t seed = 0;
		auto hashEnvironmentString = [&seed](const char* name)
		{
			boost::hash_combine(seed, GetClusterLODEnvironmentVariable(name));
		};

		boost::hash_combine(seed, static_cast<uint32_t>(kSchemaVersion));
		boost::hash_combine(seed, static_cast<uint32_t>(MS_MESHLET_SIZE));
		boost::hash_combine(seed, static_cast<uint32_t>(32)); // target bucket clusters
		boost::hash_combine(seed, static_cast<uint32_t>(4));  // max group children
		boost::hash_combine(seed, static_cast<uint32_t>(4));  // traversal node fanout
		boost::hash_combine(seed, static_cast<uint32_t>(1));  // compressed group position bitstream enabled
		boost::hash_combine(seed, static_cast<uint32_t>(1));  // compressed group normal stream enabled
		boost::hash_combine(seed, static_cast<uint32_t>(8));  // page-header-authoritative native float3 position stream + tangent-frame stream
		boost::hash_combine(seed, static_cast<uint32_t>(1));  // compressed meshlet vertex index bitstream enabled
		boost::hash_combine(seed, static_cast<uint32_t>(1));  // mesh quantization heuristic version
		boost::hash_combine(seed, static_cast<uint32_t>(1));  // UV quantization heuristic version
		boost::hash_combine(seed, static_cast<uint32_t>(7));  // USD compliance layout + inherited primvar card isolation
		boost::hash_combine(seed, static_cast<uint32_t>(27));  // voxel page descriptors use local segment addressing and SGGX voxel attributes
		boost::hash_combine(seed, static_cast<uint32_t>(4));  // traversal leaves use owner group error; segments split by refined domain
		boost::hash_combine(seed, static_cast<uint32_t>(1));  // skinned CLod builds run serially for deterministic group/page ordering
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_MODE");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_GRID");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_MIN_RES");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_RAYS");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_SCALE");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_RETRIES");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_GROWTH");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_ACCEPTANCE_BIAS");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_OPACITY_THRESHOLD");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_CARRY_ZERO_COVERAGE");
		hashEnvironmentString("BASICRENDERER_CLOD_VOXEL_PRUNING");
		return static_cast<uint64_t>(seed);
	}

	std::wstring BuildCacheFileName(const CacheKey& key, uint64_t buildConfigHash)
	{
		size_t hashSeed = 0;
		boost::hash_combine(hashSeed, key.sourceIdentifier);
		boost::hash_combine(hashSeed, key.primPath);
		boost::hash_combine(hashSeed, key.subsetName);
		boost::hash_combine(hashSeed, buildConfigHash);

		std::stringstream ss;
		ss << "clod_" << std::hex << hashSeed << ".clodmeta";
		return s2ws(ss.str());
	}

	std::wstring GetCacheFilePathForSource(const std::wstring& fileName, const std::string& sourceIdentifier)
	{
		return GetCacheFilePathBySource(fileName, sourceIdentifier);
	}

	std::optional<CacheData> TryLoad(const CacheKey& key, uint64_t expectedBuildConfigHash)
	{
		const std::wstring fileName = BuildCacheFileName(key, expectedBuildConfigHash);
		const std::wstring cachePath = GetCacheFilePathBySource(fileName, key.sourceIdentifier);
		if (!std::filesystem::exists(cachePath)) {
			return std::nullopt;
		}

		CacheData out;
		std::vector<std::byte> bytes;
		if (!ReadMetadataBlob(cachePath, bytes) || !DeserializeMetadata(bytes, out)) {
			spdlog::warn("Failed to deserialize CLod cache blob: {}", ws2s(cachePath));
			return std::nullopt;
		}

		if (out.buildConfigHash != expectedBuildConfigHash || out.schemaVersion != kSchemaVersion) {
			return std::nullopt;
		}

		if (out.prebuiltData.cacheSource.sourceIdentifier.empty()) {
			out.prebuiltData.cacheSource.sourceIdentifier = key.sourceIdentifier;
		}
		if (out.prebuiltData.cacheSource.primPath.empty()) {
			out.prebuiltData.cacheSource.primPath = key.primPath;
		}
		if (out.prebuiltData.cacheSource.subsetName.empty()) {
			out.prebuiltData.cacheSource.subsetName = key.subsetName;
		}
		if (out.prebuiltData.cacheSource.buildConfigHash == 0) {
			out.prebuiltData.cacheSource.buildConfigHash = expectedBuildConfigHash;
		}
		if (out.prebuiltData.cacheSource.containerFileName.empty()) {
			out.prebuiltData.cacheSource.containerFileName = BuildGroupContainerFileName(key, expectedBuildConfigHash);
		}

		const uint32_t pageCount = out.prebuiltData.voxelPageBase + out.prebuiltData.voxelPageCount;
		const bool hasContainerLocators = pageCount > 0u && (out.prebuiltData.pageDiskLocators.size() == pageCount);
		if (hasContainerLocators) {
			return out;
		}

		spdlog::warn(
			"CLod cache '{}' is missing disk locator metadata for {} mesh pages; treating as cache miss.",
			ws2s(cachePath),
			pageCount);
		return std::nullopt;
	}

	bool Save(const CacheKey& key, const CacheData& data)
	{
		if (data.schemaVersion != kSchemaVersion) {
			return false;
		}
		ClusterLODCacheBuildPayload payload{};
		return SaveImpl(key, data.buildConfigHash, data.prebuiltData, payload, nullptr);
	}

	bool Save(const CacheKey& key, uint64_t buildConfigHash, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload)
	{
		return SaveImpl(key, buildConfigHash, prebuiltData, payload, nullptr);
	}

	bool Save(const CacheKey& key, uint64_t buildConfigHash, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload, ClusterLODPrebuiltData* outSavedPrebuiltData)
	{
		return SaveImpl(key, buildConfigHash, prebuiltData, payload, outSavedPrebuiltData);
	}

	bool LoadGroupPayload(const CacheData& cacheData, uint32_t groupLocalIndex, LoadedGroupPayload& outPayload)
	{
		const auto& prebuilt = cacheData.prebuiltData;
		if (groupLocalIndex >= prebuilt.groups.size()) {
			return false;
		}
		std::ifstream file;
		uint32_t pageCount = 0u;
		if (!OpenContainerFile(prebuilt.cacheSource, file, pageCount) ||
			pageCount != prebuilt.pageDiskLocators.size()) {
			return false;
		}
		const ClusterLODGroup& group = prebuilt.groups[groupLocalIndex];
		const uint64_t groupPageEnd = static_cast<uint64_t>(group.pageMapBase) + static_cast<uint64_t>(group.pageCount);
		if (groupPageEnd > prebuilt.pageDiskLocators.size()) {
			return false;
		}
		if (groupLocalIndex < prebuilt.groupChunks.size()) {
			outPayload.groupChunkMetadata = prebuilt.groupChunks[groupLocalIndex];
		}
		std::vector<uint32_t> meshPageIndices;
		if (groupLocalIndex + 1u < prebuilt.groupPageReferenceOffsets.size()) {
			const uint32_t refBegin = prebuilt.groupPageReferenceOffsets[groupLocalIndex];
			const uint32_t refEnd = prebuilt.groupPageReferenceOffsets[groupLocalIndex + 1u];
			if (refBegin <= refEnd && refEnd <= prebuilt.groupPageReferences.size()) {
				meshPageIndices.assign(prebuilt.groupPageReferences.begin() + refBegin, prebuilt.groupPageReferences.begin() + refEnd);
			}
		}
		if (!meshPageIndices.empty()) {
			return LoadMeshPagesSelective(
				file,
				std::span<const ClusterLODGroupDiskLocator>(prebuilt.pageDiskLocators.data(), prebuilt.pageDiskLocators.size()),
				std::span<const uint32_t>(meshPageIndices.data(), meshPageIndices.size()),
				{},
				outPayload);
		}
		return LoadMeshPagesSelective(
			file,
			std::span<const ClusterLODGroupDiskLocator>(prebuilt.pageDiskLocators.data(), prebuilt.pageDiskLocators.size()),
			group.pageMapBase,
			group.pageCount,
			{},
			outPayload);
	}

	std::wstring ResolveContainerPath(const ClusterLODCacheSource& cacheSource)
	{
		if (cacheSource.containerFileName.empty()) {
			return {};
		}

		return GetCacheFilePathBySource(cacheSource.containerFileName, cacheSource.sourceIdentifier);
	}

	bool LoadMeshPagesSelective(std::ifstream& file,
		std::span<const ClusterLODGroupDiskLocator> pageLocators,
		uint32_t firstPage,
		uint32_t pageCount,
		const std::vector<bool>& pageNeedsFetch,
		LoadedGroupPayload& outPayload)
	{
		outPayload.pageBlobs.assign(pageCount, {});
		const uint64_t endPage = static_cast<uint64_t>(firstPage) + static_cast<uint64_t>(pageCount);
		if (endPage > pageLocators.size()) {
			return false;
		}
		for (uint32_t pageOffset = 0; pageOffset < pageCount; ++pageOffset) {
			if (!pageNeedsFetch.empty() &&
				pageOffset < static_cast<uint32_t>(pageNeedsFetch.size()) &&
				!pageNeedsFetch[pageOffset]) {
				continue;
			}
			if (!ReadPageBlobDirect(file, pageLocators[firstPage + pageOffset], outPayload.pageBlobs[pageOffset])) {
				return false;
			}
		}
		return true;
	}

	bool LoadMeshPagesSelective(std::ifstream& file,
		std::span<const ClusterLODGroupDiskLocator> pageLocators,
		std::span<const uint32_t> meshPageIndices,
		const std::vector<bool>& pageNeedsFetch,
		LoadedGroupPayload& outPayload)
	{
		outPayload.pageBlobs.assign(meshPageIndices.size(), {});
		for (uint32_t pageOffset = 0; pageOffset < static_cast<uint32_t>(meshPageIndices.size()); ++pageOffset) {
			if (!pageNeedsFetch.empty() &&
				pageOffset < static_cast<uint32_t>(pageNeedsFetch.size()) &&
				!pageNeedsFetch[pageOffset]) {
				continue;
			}
			const uint32_t meshPageIndex = meshPageIndices[pageOffset];
			if (meshPageIndex >= pageLocators.size() ||
				!ReadPageBlobDirect(file, pageLocators[meshPageIndex], outPayload.pageBlobs[pageOffset])) {
				return false;
			}
		}
		return true;
	}

	bool GetMeshPagePayloadLayout(std::span<const ClusterLODGroupDiskLocator> pageLocators,
		uint32_t firstPage,
		uint32_t pageCount,
		PagePayloadLayoutMetadata& outLayout)
	{
		outLayout.Clear();
		const uint64_t endPage = static_cast<uint64_t>(firstPage) + static_cast<uint64_t>(pageCount);
		if (endPage > pageLocators.size()) {
			return false;
		}
		outLayout.pageBlobSizes.reserve(pageCount);
		outLayout.pageBlobOffsets.reserve(pageCount);
		for (uint32_t pageOffset = 0; pageOffset < pageCount; ++pageOffset) {
			const ClusterLODGroupDiskLocator& locator = pageLocators[firstPage + pageOffset];
			outLayout.pageBlobSizes.push_back(locator.blobSizeBytes);
			outLayout.pageBlobOffsets.push_back(locator.blobOffset);
		}
		return true;
	}

	bool GetMeshPagePayloadLayout(std::span<const ClusterLODGroupDiskLocator> pageLocators,
		std::span<const uint32_t> meshPageIndices,
		PagePayloadLayoutMetadata& outLayout)
	{
		outLayout.Clear();
		outLayout.pageBlobSizes.reserve(meshPageIndices.size());
		outLayout.pageBlobOffsets.reserve(meshPageIndices.size());
		for (uint32_t meshPageIndex : meshPageIndices) {
			if (meshPageIndex >= pageLocators.size()) {
				outLayout.Clear();
				return false;
			}
			const ClusterLODGroupDiskLocator& locator = pageLocators[meshPageIndex];
			outLayout.pageBlobSizes.push_back(locator.blobSizeBytes);
			outLayout.pageBlobOffsets.push_back(locator.blobOffset);
		}
		return true;
	}

	bool LoadMeshPagesSelectiveDirectStorage(
		const std::wstring& containerPath,
		std::span<const ClusterLODGroupDiskLocator> pageLocators,
		uint32_t firstPage,
		uint32_t pageCount,
		const std::vector<bool>& pageNeedsFetch,
		LoadedGroupPayload& outPayload,
		std::string* outMessage)
	{
#if !BASICRENDERER_HAS_DIRECTSTORAGE
		(void)containerPath;
		(void)pageLocators;
		(void)firstPage;
		(void)pageCount;
		(void)pageNeedsFetch;
		(void)outPayload;
		if (outMessage) {
			*outMessage = "DirectStorage support is not compiled into this target";
		}
		return false;
#else
		if (outMessage) {
			outMessage->clear();
		}
		if (!DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::SystemMemory)) {
			if (outMessage) {
				*outMessage = "DirectStorage system-memory queue unavailable";
			}
			return false;
		}
		outPayload.pageBlobs.assign(pageCount, {});
		const uint64_t endPage = static_cast<uint64_t>(firstPage) + static_cast<uint64_t>(pageCount);
		if (endPage > pageLocators.size()) {
			return false;
		}
		for (uint32_t pageOffset = 0; pageOffset < pageCount; ++pageOffset) {
			if (!pageNeedsFetch.empty() &&
				pageOffset < static_cast<uint32_t>(pageNeedsFetch.size()) &&
				!pageNeedsFetch[pageOffset]) {
				continue;
			}
			const ClusterLODGroupDiskLocator& locator = pageLocators[firstPage + pageOffset];
			std::string readMessage;
			if (!DirectStorageManager::GetInstance().ReadFileRegionToMemory(
				containerPath,
				locator.blobOffset,
				locator.blobSizeBytes,
				outPayload.pageBlobs[pageOffset],
				&readMessage)) {
				if (outMessage) {
					*outMessage = readMessage.empty() ? "DirectStorage CLod page read failed" : readMessage;
				}
				return false;
			}
		}
		if (outMessage) {
			*outMessage = "loaded selected CLod mesh pages through DirectStorage";
		}
		return true;
#endif
	}

	bool LoadMeshPagesSelectiveDirectStorage(
		const std::wstring& containerPath,
		std::span<const ClusterLODGroupDiskLocator> pageLocators,
		std::span<const uint32_t> meshPageIndices,
		const std::vector<bool>& pageNeedsFetch,
		LoadedGroupPayload& outPayload,
		std::string* outMessage)
	{
#if !BASICRENDERER_HAS_DIRECTSTORAGE
		(void)containerPath;
		(void)pageLocators;
		(void)meshPageIndices;
		(void)pageNeedsFetch;
		(void)outPayload;
		if (outMessage) {
			*outMessage = "DirectStorage support is not compiled into this target";
		}
		return false;
#else
		if (outMessage) {
			outMessage->clear();
		}
		if (!DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::SystemMemory)) {
			if (outMessage) {
				*outMessage = "DirectStorage system-memory queue unavailable";
			}
			return false;
		}
		outPayload.pageBlobs.assign(meshPageIndices.size(), {});
		for (uint32_t pageOffset = 0; pageOffset < static_cast<uint32_t>(meshPageIndices.size()); ++pageOffset) {
			if (!pageNeedsFetch.empty() &&
				pageOffset < static_cast<uint32_t>(pageNeedsFetch.size()) &&
				!pageNeedsFetch[pageOffset]) {
				continue;
			}
			const uint32_t meshPageIndex = meshPageIndices[pageOffset];
			if (meshPageIndex >= pageLocators.size()) {
				return false;
			}
			const ClusterLODGroupDiskLocator& locator = pageLocators[meshPageIndex];
			std::string readMessage;
			if (!DirectStorageManager::GetInstance().ReadFileRegionToMemory(
				containerPath,
				locator.blobOffset,
				locator.blobSizeBytes,
				outPayload.pageBlobs[pageOffset],
				&readMessage)) {
				if (outMessage) {
					*outMessage = readMessage.empty() ? "DirectStorage CLod page read failed" : readMessage;
				}
				return false;
			}
		}
		if (outMessage) {
			*outMessage = "loaded selected CLod mesh pages through DirectStorage";
		}
		return true;
#endif
	}


	bool OpenContainerFile(const ClusterLODCacheSource& cacheSource,
		std::ifstream& outFile,
		uint32_t& outPageCount)
	{
		outPageCount = 0u;
		if (cacheSource.containerFileName.empty()) {
			return false;
		}

		const std::wstring containerPath = GetCacheFilePathBySource(cacheSource.containerFileName, cacheSource.sourceIdentifier);
		outFile.open(containerPath, std::ios::binary);
		if (!outFile.is_open()) {
			return false;
		}

		ContainerHeader header{};
		outFile.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!outFile.good() || header.magic != kContainerMagic || header.version != 4u) {
			outFile.close();
			return false;
		}

		outPageCount = header.pageCount;
		return true;
	}

}
