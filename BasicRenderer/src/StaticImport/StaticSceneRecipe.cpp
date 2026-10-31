#include <BasicRenderer/StaticImport/StaticSceneRecipe.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <type_traits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace fs = std::filesystem;

namespace br::staticimport::recipe
{
	namespace
	{
		constexpr std::uint64_t kPackMagic = 0x4B43415052535353ull; // SSRPACK
		constexpr std::uint64_t kIndexMagic = 0x58444E4952535353ull; // SSRINDX
		constexpr std::uint64_t kCellMagic = 0x4C4C454352535353ull; // SSRCELL
		constexpr std::uint32_t kEndianMarker = 0x01020304u;
		constexpr std::uint64_t kAlignment = 4096;
		constexpr std::uint64_t kMaxCellBytes = 2ull * 1024ull * 1024ull * 1024ull;
		constexpr std::uint64_t kMaxCollectionCount = 100'000'000ull;
		constexpr std::uint64_t kMaxStringBytes = 16ull * 1024ull * 1024ull;

		#pragma pack(push, 1)
		struct PackHeader
		{
			std::uint64_t magic{ kPackMagic };
			std::uint32_t schema{ kSchemaVersion };
			std::uint32_t endian{ kEndianMarker };
			EnvironmentIdentity identity;
			std::uint32_t worldFormID{ 0 };
			std::uint32_t interiorCellFormID{ 0 };
			std::uint64_t headerChecksum{ 0 };
		};

		struct IndexHeader
		{
			std::uint64_t magic{ kIndexMagic };
			std::uint32_t schema{ kSchemaVersion };
			std::uint32_t endian{ kEndianMarker };
			EnvironmentIdentity identity;
			std::uint32_t worldFormID{ 0 };
			std::uint32_t interiorCellFormID{ 0 };
			std::uint64_t committedPackBytes{ 0 };
			std::uint64_t entryCount{ 0 };
			std::uint64_t entriesChecksum{ 0 };
		};

		struct IndexEntry
		{
			CellKey key;
			std::uint64_t offset{ 0 };
			std::uint64_t size{ 0 };
			std::uint64_t checksum{ 0 };
		};

		struct CellHeader
		{
			std::uint64_t magic{ kCellMagic };
			std::uint32_t schema{ kSchemaVersion };
			std::uint32_t flags{ 0 };
			CellKey key;
			std::uint64_t sourceFingerprint{ 0 };
			std::uint64_t skippedPlacements{ 0 };
			// Retained so the v5 decoder remains available to reject/diagnose old packs.
			std::uint64_t groupCount{ 0 };
			RecipeRange groups;
			RecipeRange importBatches;
			RecipeRange materialOverrides;
			RecipeRange matrices;
			RecipeRange placementFormIDs;
			RecipeRange preparedParts;
			RecipeRange perObjectRows;
			RecipeRange normalMatrices;
			RecipeRange templates;
			RecipeRange workloads;
			RecipeRange strings;
			std::uint64_t byteSize{ 0 };
		};
		#pragma pack(pop)

		static_assert(std::is_trivially_copyable_v<CellKey>);
		static_assert(std::is_trivially_copyable_v<Matrix4x4>);
		static_assert(std::is_trivially_copyable_v<IndexEntry>);

		std::uint64_t AlignUp(std::uint64_t value)
		{
			return (value + kAlignment - 1) & ~(kAlignment - 1);
		}

		std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		template <class T>
		void AppendPod(std::vector<std::byte>& bytes, const T& value)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* begin = reinterpret_cast<const std::byte*>(std::addressof(value));
			bytes.insert(bytes.end(), begin, begin + sizeof(T));
		}

		void AppendString(std::vector<std::byte>& bytes, std::string_view value)
		{
			const auto length = static_cast<std::uint64_t>(value.size());
			AppendPod(bytes, length);
			const auto* begin = reinterpret_cast<const std::byte*>(value.data());
			bytes.insert(bytes.end(), begin, begin + value.size());
		}

		template <class T>
		void AppendPods(std::vector<std::byte>& bytes, std::span<const T> values)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto raw = std::as_bytes(values);
			bytes.insert(bytes.end(), raw.begin(), raw.end());
		}

		class Reader
		{
		public:
			explicit Reader(std::span<const std::byte> bytes) : _remaining(bytes) {}

			template <class T>
			bool Pod(T& value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				if (_remaining.size() < sizeof(T)) return false;
				std::memcpy(std::addressof(value), _remaining.data(), sizeof(T));
				_remaining = _remaining.subspan(sizeof(T));
				return true;
			}

			bool String(std::string& value)
			{
				std::uint64_t length = 0;
				if (!Pod(length) || length > kMaxStringBytes || length > _remaining.size()) return false;
				value.assign(reinterpret_cast<const char*>(_remaining.data()), static_cast<std::size_t>(length));
				_remaining = _remaining.subspan(static_cast<std::size_t>(length));
				return true;
			}

			template <class T>
			bool Pods(std::span<T> values)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				const auto byteCount = values.size_bytes();
				if (_remaining.size() < byteCount) return false;
				if (byteCount != 0) {
					std::memcpy(values.data(), _remaining.data(), byteCount);
				}
				_remaining = _remaining.subspan(byteCount);
				return true;
			}

			[[nodiscard]] bool Empty() const { return _remaining.empty(); }

		private:
			std::span<const std::byte> _remaining;
		};

		std::vector<std::byte> SerializeCell(const CellRecipe& recipe)
		{
			std::vector<std::byte> bytes;
			CellHeader header;
			header.flags = recipe.authoritativeEmpty ? 1u : 0u;
			header.key = recipe.key;
			header.sourceFingerprint = recipe.sourceFingerprint;
			header.skippedPlacements = recipe.skippedPlacements;
			header.groupCount = recipe.groups.size();
			AppendPod(bytes, header);
			for (const auto& group : recipe.groups) {
				AppendPod(bytes, group.stableGroupID);
				AppendPod(bytes, group.materialOverrideHash);
				const std::uint8_t preparedAuthoritative = group.preparedAuthoritative ? 1u : 0u;
				AppendPod(bytes, preparedAuthoritative);
				AppendString(bytes, group.modelPath);
				AppendString(bytes, group.name);
				const auto overrideCount = static_cast<std::uint64_t>(group.materialOverrides.size());
				AppendPod(bytes, overrideCount);
				for (const auto& overrideInfo : group.materialOverrides) {
					AppendString(bytes, overrideInfo.name3D);
					AppendString(bytes, overrideInfo.normalizedName3D);
					AppendString(bytes, overrideInfo.textureSetEditorID);
					AppendPod(bytes, overrideInfo.index3D);
					AppendPod(bytes, overrideInfo.flags);
					for (const auto& path : overrideInfo.texturePaths) AppendString(bytes, path);
				}
				const auto matrixCount = static_cast<std::uint64_t>(group.instanceWorldMatrices.size());
				AppendPod(bytes, matrixCount);
				AppendPods(bytes, std::span<const Matrix4x4>{ group.instanceWorldMatrices });
				const auto placementCount = static_cast<std::uint64_t>(group.placementFormIDs.size());
				AppendPod(bytes, placementCount);
				AppendPods(bytes, std::span<const std::uint32_t>{ group.placementFormIDs });
			}
			const auto preparedCount = static_cast<std::uint64_t>(recipe.preparedParts.size());
			AppendPod(bytes, preparedCount);
			for (const auto& part : recipe.preparedParts) {
				AppendPod(bytes, part.sourceGroupID);
				AppendPod(bytes, part.stableGroupID);
				AppendPod(bytes, part.allocationScopeID);
				AppendPod(bytes, part.perObjectRowStride);
				const auto rowBytes = static_cast<std::uint64_t>(part.perObjectRows.size());
				AppendPod(bytes, rowBytes);
				bytes.insert(bytes.end(), part.perObjectRows.begin(), part.perObjectRows.end());
				const auto normalCount = static_cast<std::uint64_t>(part.normalMatrices.size());
				AppendPod(bytes, normalCount);
				AppendPods(bytes, std::span<const Matrix4x4>{ part.normalMatrices });
				const auto templateCount = static_cast<std::uint64_t>(part.templates.size());
				AppendPod(bytes, templateCount);
				for (const auto& meshTemplate : part.templates) {
					AppendPod(bytes, meshTemplate.partIndex);
					AppendPod(bytes, meshTemplate.meshIndex);
					const auto workloadCount = static_cast<std::uint64_t>(meshTemplate.workloadKeys.size());
					AppendPod(bytes, workloadCount);
					for (const auto& workload : meshTemplate.workloadKeys) {
						AppendPod(bytes, workload.compileFlags);
						AppendString(bytes, workload.renderPhase);
						const std::uint8_t clodOnly = workload.clodOnly ? 1u : 0u;
						AppendPod(bytes, clodOnly);
					}
				}
			}
			return bytes;
		}

		bool ReadCount(Reader& reader, std::uint64_t& count)
		{
			return reader.Pod(count) && count <= kMaxCollectionCount;
		}

		std::optional<CellRecipe> DeserializeCell(std::span<const std::byte> bytes, std::string& reason)
		{
			Reader reader(bytes);
			CellHeader header;
			if (!reader.Pod(header) || header.magic != kCellMagic || header.schema != kSchemaVersion || header.groupCount > kMaxCollectionCount) {
				reason = "invalid cell header";
				return std::nullopt;
			}
			CellRecipe recipe;
			recipe.key = header.key;
			recipe.authoritativeEmpty = (header.flags & 1u) != 0;
			recipe.sourceFingerprint = header.sourceFingerprint;
			recipe.skippedPlacements = header.skippedPlacements;
			recipe.groups.resize(static_cast<std::size_t>(header.groupCount));
			for (auto& group : recipe.groups) {
				std::uint8_t preparedAuthoritative = 0;
				if (!reader.Pod(group.stableGroupID) || !reader.Pod(group.materialOverrideHash) || !reader.Pod(preparedAuthoritative) ||
					!reader.String(group.modelPath) || !reader.String(group.name)) {
					reason = "truncated group";
					return std::nullopt;
				}
				group.preparedAuthoritative = preparedAuthoritative != 0;
				std::uint64_t count = 0;
				if (!ReadCount(reader, count)) { reason = "invalid override count"; return std::nullopt; }
				group.materialOverrides.resize(static_cast<std::size_t>(count));
				for (auto& overrideInfo : group.materialOverrides) {
					if (!reader.String(overrideInfo.name3D) || !reader.String(overrideInfo.normalizedName3D) ||
						!reader.String(overrideInfo.textureSetEditorID) || !reader.Pod(overrideInfo.index3D) || !reader.Pod(overrideInfo.flags)) {
						reason = "truncated material override";
						return std::nullopt;
					}
					for (auto& path : overrideInfo.texturePaths) {
						if (!reader.String(path)) { reason = "truncated texture path"; return std::nullopt; }
					}
				}
				if (!ReadCount(reader, count)) { reason = "invalid matrix count"; return std::nullopt; }
				group.instanceWorldMatrices.resize(static_cast<std::size_t>(count));
				if (!reader.Pods(std::span<Matrix4x4>{ group.instanceWorldMatrices })) { reason = "truncated matrix rows"; return std::nullopt; }
				if (!ReadCount(reader, count)) { reason = "invalid placement count"; return std::nullopt; }
				group.placementFormIDs.resize(static_cast<std::size_t>(count));
				if (!reader.Pods(std::span<std::uint32_t>{ group.placementFormIDs })) { reason = "truncated placement IDs"; return std::nullopt; }
				if (group.instanceWorldMatrices.size() != group.placementFormIDs.size()) {
					reason = "matrix and placement counts differ";
					return std::nullopt;
				}
			}
			std::uint64_t preparedCount = 0;
			if (!ReadCount(reader, preparedCount)) { reason = "invalid prepared part count"; return std::nullopt; }
			recipe.preparedParts.resize(static_cast<std::size_t>(preparedCount));
			for (auto& part : recipe.preparedParts) {
				std::uint64_t byteCount = 0;
				if (!reader.Pod(part.sourceGroupID) || !reader.Pod(part.stableGroupID) || !reader.Pod(part.allocationScopeID) || !reader.Pod(part.perObjectRowStride) ||
					!reader.Pod(byteCount) || byteCount > kMaxCellBytes) {
					reason = "invalid prepared rows"; return std::nullopt;
				}
				part.perObjectRows.resize(static_cast<std::size_t>(byteCount));
				if (!reader.Pods(std::span<std::byte>{ part.perObjectRows })) { reason = "truncated prepared rows"; return std::nullopt; }
				std::uint64_t count = 0;
				if (!ReadCount(reader, count)) { reason = "invalid normal count"; return std::nullopt; }
				part.normalMatrices.resize(static_cast<std::size_t>(count));
				if (!reader.Pods(std::span<Matrix4x4>{ part.normalMatrices })) { reason = "truncated normal rows"; return std::nullopt; }
				if (part.perObjectRowStride == 0 || part.perObjectRows.size() % part.perObjectRowStride != 0 ||
					part.perObjectRows.size() / part.perObjectRowStride != part.normalMatrices.size()) {
					reason = "prepared row cardinality mismatch"; return std::nullopt;
				}
				if (!ReadCount(reader, count)) { reason = "invalid prepared template count"; return std::nullopt; }
				part.templates.resize(static_cast<std::size_t>(count));
				for (auto& meshTemplate : part.templates) {
					if (!reader.Pod(meshTemplate.partIndex) || !reader.Pod(meshTemplate.meshIndex) || !ReadCount(reader, count)) {
						reason = "invalid prepared template"; return std::nullopt;
					}
					meshTemplate.workloadKeys.resize(static_cast<std::size_t>(count));
					for (auto& workload : meshTemplate.workloadKeys) {
						std::uint8_t clodOnly = 0;
						if (!reader.Pod(workload.compileFlags) || !reader.String(workload.renderPhase) || !reader.Pod(clodOnly)) {
							reason = "truncated workload key"; return std::nullopt;
						}
						workload.clodOnly = clodOnly != 0;
					}
				}
			}
			if (!reader.Empty()) {
				reason = "unexpected trailing cell bytes";
				return std::nullopt;
			}
			return recipe;
		}

		std::vector<std::byte> SerializeCellV6(const CellRecipe& recipe)
		{
			std::vector<MappedGroup> groups;
			std::vector<MappedMaterialOverride> overrides;
			std::vector<Matrix4x4> matrices;
			std::vector<std::uint32_t> placements;
			std::vector<MappedPreparedPart> parts;
			std::vector<std::byte> rows;
			std::vector<Matrix4x4> normals;
			std::vector<MappedPreparedTemplate> templates;
			std::vector<MappedWorkloadKey> workloads;
			std::vector<char> strings;
			std::unordered_map<std::string, RecipeStringRef> interned;
			auto addString = [&](const std::string& value) {
				if (const auto found = interned.find(value); found != interned.end()) return found->second;
				RecipeStringRef ref{ strings.size(), value.size() };
				strings.insert(strings.end(), value.begin(), value.end());
				interned.emplace(value, ref);
				return ref;
			};
			for (const auto& source : recipe.groups) {
				MappedGroup group;
				group.stableGroupID = source.stableGroupID;
				group.materialOverrideHash = source.materialOverrideHash;
				group.modelPath = addString(source.modelPath);
				group.name = addString(source.name);
				group.flags = source.preparedAuthoritative ? 1u : 0u;
				group.materialOverrides = { overrides.size(), source.materialOverrides.size() };
				for (const auto& sourceOverride : source.materialOverrides) {
					MappedMaterialOverride item;
					item.name3D = addString(sourceOverride.name3D);
					item.normalizedName3D = addString(sourceOverride.normalizedName3D);
					item.textureSetEditorID = addString(sourceOverride.textureSetEditorID);
					item.index3D = sourceOverride.index3D;
					item.flags = sourceOverride.flags;
					for (std::size_t i = 0; i < item.texturePaths.size(); ++i) item.texturePaths[i] = addString(sourceOverride.texturePaths[i]);
					overrides.push_back(item);
				}
				group.instanceWorldMatrices = { matrices.size(), source.instanceWorldMatrices.size() };
				matrices.insert(matrices.end(), source.instanceWorldMatrices.begin(), source.instanceWorldMatrices.end());
				group.placementFormIDs = { placements.size(), source.placementFormIDs.size() };
				placements.insert(placements.end(), source.placementFormIDs.begin(), source.placementFormIDs.end());
				group.importCost = source.importCost;
				groups.push_back(group);
			}
			for (const auto& source : recipe.preparedParts) {
				MappedPreparedPart part;
				part.sourceGroupID = source.sourceGroupID;
				part.stableGroupID = source.stableGroupID;
				part.allocationScopeID = source.allocationScopeID;
				part.perObjectRowStride = source.perObjectRowStride;
				part.perObjectRows = { rows.size(), source.perObjectRows.size() };
				rows.insert(rows.end(), source.perObjectRows.begin(), source.perObjectRows.end());
				part.normalMatrices = { normals.size(), source.normalMatrices.size() };
				normals.insert(normals.end(), source.normalMatrices.begin(), source.normalMatrices.end());
				part.templates = { templates.size(), source.templates.size() };
				for (const auto& sourceTemplate : source.templates) {
					MappedPreparedTemplate meshTemplate;
					meshTemplate.partIndex = sourceTemplate.partIndex;
					meshTemplate.meshIndex = sourceTemplate.meshIndex;
					meshTemplate.workloadKeys = { workloads.size(), sourceTemplate.workloadKeys.size() };
					for (const auto& sourceWorkload : sourceTemplate.workloadKeys) {
						MappedWorkloadKey workload;
						workload.compileFlags = sourceWorkload.compileFlags;
						workload.renderPhase = addString(sourceWorkload.renderPhase);
						workload.clodOnly = sourceWorkload.clodOnly ? 1u : 0u;
						workloads.push_back(workload);
					}
					templates.push_back(meshTemplate);
				}
				parts.push_back(part);
			}

			CellHeader header;
			header.flags = recipe.authoritativeEmpty ? 1u : 0u;
			header.key = recipe.key;
			header.sourceFingerprint = recipe.sourceFingerprint;
			header.skippedPlacements = recipe.skippedPlacements;
			header.groupCount = groups.size();
			std::vector<std::byte> bytes(sizeof(header));
			auto section = [&]<class T>(RecipeRange& range, std::span<const T> values) {
				const auto offset = AlignUp(bytes.size(), alignof(T));
				bytes.resize(static_cast<std::size_t>(offset));
				range = { offset, values.size() };
				AppendPods(bytes, values);
			};
			section(header.groups, std::span<const MappedGroup>{ groups });
			section(header.importBatches, std::span<const ImportBatch>{ recipe.importBatches });
			section(header.materialOverrides, std::span<const MappedMaterialOverride>{ overrides });
			section(header.matrices, std::span<const Matrix4x4>{ matrices });
			section(header.placementFormIDs, std::span<const std::uint32_t>{ placements });
			section(header.preparedParts, std::span<const MappedPreparedPart>{ parts });
			{
				const auto offset = AlignUp(bytes.size(), 64u);
				bytes.resize(static_cast<std::size_t>(offset));
				header.perObjectRows = { offset, rows.size() };
				AppendPods(bytes, std::span<const std::byte>{ rows });
			}
			section(header.normalMatrices, std::span<const Matrix4x4>{ normals });
			section(header.templates, std::span<const MappedPreparedTemplate>{ templates });
			section(header.workloads, std::span<const MappedWorkloadKey>{ workloads });
			section(header.strings, std::span<const char>{ strings });
			header.byteSize = bytes.size();
			std::memcpy(bytes.data(), std::addressof(header), sizeof(header));
			return bytes;
		}

		struct ParsedCellV6
		{
			CellHeader header;
			std::span<const MappedGroup> groups;
			std::span<const ImportBatch> importBatches;
			std::span<const MappedMaterialOverride> overrides;
			std::span<const Matrix4x4> matrices;
			std::span<const std::uint32_t> placements;
			std::span<const MappedPreparedPart> parts;
			std::span<const std::byte> rows;
			std::span<const Matrix4x4> normals;
			std::span<const MappedPreparedTemplate> templates;
			std::span<const MappedWorkloadKey> workloads;
			std::span<const char> strings;
		};

		template <class T>
		bool ReadSection(std::span<const std::byte> bytes, RecipeRange range, std::span<const T>& out)
		{
			if (range.count > kMaxCollectionCount || range.first > bytes.size() ||
				range.count > (bytes.size() - range.first) / sizeof(T) || range.first % alignof(T) != 0) return false;
			out = { reinterpret_cast<const T*>(bytes.data() + range.first), static_cast<std::size_t>(range.count) };
			return true;
		}

		std::optional<ParsedCellV6> ParseCellV6(std::span<const std::byte> bytes, std::string& reason)
		{
			if (bytes.size() < sizeof(CellHeader)) { reason = "truncated v6 cell header"; return std::nullopt; }
			ParsedCellV6 result;
			std::memcpy(std::addressof(result.header), bytes.data(), sizeof(CellHeader));
			if (result.header.magic != kCellMagic || result.header.schema != kSchemaVersion || result.header.byteSize != bytes.size() ||
				!ReadSection(bytes, result.header.groups, result.groups) ||
				!ReadSection(bytes, result.header.importBatches, result.importBatches) ||
				!ReadSection(bytes, result.header.materialOverrides, result.overrides) || !ReadSection(bytes, result.header.matrices, result.matrices) ||
				!ReadSection(bytes, result.header.placementFormIDs, result.placements) || !ReadSection(bytes, result.header.preparedParts, result.parts) ||
				!ReadSection(bytes, result.header.perObjectRows, result.rows) || !ReadSection(bytes, result.header.normalMatrices, result.normals) ||
				!ReadSection(bytes, result.header.templates, result.templates) || !ReadSection(bytes, result.header.workloads, result.workloads) ||
				!ReadSection(bytes, result.header.strings, result.strings)) {
				reason = "invalid v6 cell section"; return std::nullopt;
			}
			auto rangeValid = [](RecipeRange range, std::size_t size) { return range.first <= size && range.count <= size - range.first; };
			auto stringValid = [&](RecipeStringRef ref) { return rangeValid({ ref.offset, ref.size }, result.strings.size()); };
			for (const auto& group : result.groups) {
				if (!stringValid(group.modelPath) || !stringValid(group.name) || !rangeValid(group.materialOverrides, result.overrides.size()) ||
					!rangeValid(group.instanceWorldMatrices, result.matrices.size()) || !rangeValid(group.placementFormIDs, result.placements.size()) ||
					group.instanceWorldMatrices.count != group.placementFormIDs.count) { reason = "invalid v6 group range"; return std::nullopt; }
			}
			std::size_t nextGroup = 0;
			for (const auto& batch : result.importBatches) {
				if (batch.groupCount == 0 || batch.firstGroup != nextGroup ||
					batch.groupCount > result.groups.size() - nextGroup) {
					reason = "invalid v8 import batch range"; return std::nullopt;
				}
				std::uint64_t placements = 0;
				Group::ImportCost cost;
				for (const auto& group : result.groups.subspan(nextGroup, batch.groupCount)) {
					placements += group.placementFormIDs.count;
					cost.transformRows += group.importCost.transformRows;
					cost.drawRecords += group.importCost.drawRecords;
					cost.activeInsertIndices += group.importCost.activeInsertIndices;
					cost.preparedBytes += group.importCost.preparedBytes;
					cost.uploadBytes += group.importCost.uploadBytes;
				}
				if (batch.placementCount != placements || batch.importCost != cost) {
					reason = "invalid v8 import batch cost"; return std::nullopt;
				}
				nextGroup += batch.groupCount;
			}
			if (nextGroup != result.groups.size()) { reason = "incomplete v8 import batches"; return std::nullopt; }
			for (const auto& item : result.overrides) {
				if (!stringValid(item.name3D) || !stringValid(item.normalizedName3D) || !stringValid(item.textureSetEditorID) ||
					std::ranges::any_of(item.texturePaths, [&](auto ref) { return !stringValid(ref); })) { reason = "invalid v6 override string"; return std::nullopt; }
			}
			for (const auto& part : result.parts) {
				if (!rangeValid(part.perObjectRows, result.rows.size()) || !rangeValid(part.normalMatrices, result.normals.size()) ||
					!rangeValid(part.templates, result.templates.size()) || part.perObjectRowStride == 0 ||
					part.perObjectRows.count % part.perObjectRowStride != 0 || part.perObjectRows.count / part.perObjectRowStride != part.normalMatrices.count) {
					reason = "invalid v6 prepared part range"; return std::nullopt;
				}
			}
			for (const auto& item : result.templates) if (!rangeValid(item.workloadKeys, result.workloads.size())) { reason = "invalid v6 workload range"; return std::nullopt; }
			for (const auto& item : result.workloads) if (!stringValid(item.renderPhase)) { reason = "invalid v6 workload string"; return std::nullopt; }
			return result;
		}

		std::uint64_t HeaderChecksum(PackHeader header)
		{
			header.headerChecksum = 0;
			return HashBytes(std::as_bytes(std::span{ &header, 1 }));
		}

		std::vector<IndexEntry> ReadIndex(const fs::path& path, const EnvironmentIdentity& identity,
			const CellKey& key, IndexHeader& header, std::string& reason)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input) { reason = "index not found"; return {}; }
			input.read(reinterpret_cast<char*>(std::addressof(header)), sizeof(header));
			if (!input || header.magic != kIndexMagic || header.schema != kSchemaVersion || header.endian != kEndianMarker) {
				reason = "invalid index header"; return {};
			}
			if (std::memcmp(std::addressof(header.identity), std::addressof(identity), sizeof(identity)) != 0 ||
				header.worldFormID != key.worldFormID || header.interiorCellFormID != key.interiorCellFormID) {
				reason = "stale index identity"; return {};
			}
			if (header.entryCount > kMaxCollectionCount || header.entryCount > (std::numeric_limits<std::size_t>::max)() / sizeof(IndexEntry)) {
				reason = "index count out of range"; return {};
			}
			std::vector<IndexEntry> entries(static_cast<std::size_t>(header.entryCount));
			input.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size() * sizeof(IndexEntry)));
			if (!input || HashBytes(std::as_bytes(std::span(entries))) != header.entriesChecksum) {
				reason = "index checksum mismatch"; return {};
			}
			return entries;
		}

		bool WriteAll(HANDLE handle, const void* data, std::uint64_t size)
		{
			const auto* cursor = static_cast<const std::byte*>(data);
			while (size != 0) {
				const auto chunk = static_cast<DWORD>((std::min<std::uint64_t>)(size, (std::numeric_limits<DWORD>::max)()));
				DWORD written = 0;
				if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written != chunk) return false;
				cursor += written;
				size -= written;
			}
			return true;
		}

		std::wstring MutexName(const fs::path& packPath)
		{
			const auto utf8 = packPath.generic_u8string();
			const auto hash = HashBytes(std::as_bytes(std::span(utf8.data(), utf8.size())));
			std::wostringstream name;
			name << L"Local\\SARP.StaticSceneRecipe." << std::hex << hash;
			return name.str();
		}

		auto FindIndexEntry(std::vector<IndexEntry>& entries, const CellKey& key)
		{
			return std::lower_bound(entries.begin(), entries.end(), key, [](const IndexEntry& lhs, const CellKey& rhs) {
				return std::tie(lhs.key.interiorCellFormID, lhs.key.cellY, lhs.key.cellX) <
					std::tie(rhs.interiorCellFormID, rhs.cellY, rhs.cellX);
			});
		}

		struct HandleCloser { void operator()(HANDLE value) const { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
		using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

		bool ValidateCommittedCell(
			const fs::path& packPath,
			const IndexHeader& indexHeader,
			const IndexEntry& entry,
			std::string& reason)
		{
			if (entry.size > kMaxCellBytes || entry.offset > indexHeader.committedPackBytes ||
				entry.size > indexHeader.committedPackBytes - entry.offset) {
				reason = "cell range outside committed pack";
				return false;
			}

			UniqueHandle file(CreateFileW(
				packPath.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr));
			if (!file || file.get() == INVALID_HANDLE_VALUE) {
				reason = "open committed pack failed";
				return false;
			}

			LARGE_INTEGER fileSize{};
			if (!GetFileSizeEx(file.get(), std::addressof(fileSize)) ||
				fileSize.QuadPart < 0 ||
				static_cast<std::uint64_t>(fileSize.QuadPart) < indexHeader.committedPackBytes) {
				reason = "committed pack is truncated";
				return false;
			}

			LARGE_INTEGER offset{};
			offset.QuadPart = static_cast<LONGLONG>(entry.offset);
			if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) {
				reason = "seek committed cell failed";
				return false;
			}
			std::vector<std::byte> bytes(static_cast<std::size_t>(entry.size));
			DWORD bytesRead = 0;
			if (entry.size > (std::numeric_limits<DWORD>::max)() ||
				!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), std::addressof(bytesRead), nullptr) ||
				bytesRead != bytes.size()) {
				reason = "read committed cell failed";
				return false;
			}
			if (HashBytes(bytes) != entry.checksum) {
				reason = "committed cell checksum mismatch";
				return false;
			}
			auto parsed = ParseCellV6(bytes, reason);
			if (!parsed || parsed->header.key != entry.key) {
				if (reason.empty()) reason = "committed cell key mismatch";
				return false;
			}
			return true;
		}

	}

	struct StoreState
	{
		struct MappedPack
		{
			IndexHeader header;
			std::vector<IndexEntry> entries;
			UniqueHandle file;
			UniqueHandle mapping;
			const void* view{ nullptr };
			~MappedPack() { if (view) UnmapViewOfFile(view); }
		};
		std::mutex mutex;
		std::unordered_map<std::wstring, std::shared_ptr<MappedPack>> packs;
	};

	struct MappedRecipePack::Impl
	{
		std::shared_ptr<StoreState::MappedPack> mapped;
	};

	std::string_view CellRecipeView::String(RecipeStringRef ref) const
	{
		if (ref.offset > _strings.size() || ref.size > _strings.size() - ref.offset) return {};
		return { _strings.data() + ref.offset, static_cast<std::size_t>(ref.size) };
	}

	std::uint64_t MappedRecipePack::MappedBytes() const
	{
		return _impl && _impl->mapped ? _impl->mapped->header.committedPackBytes : 0;
	}

	RecipeSceneView MappedRecipePack::LookupCells(std::span<const CellKey> keys) const
	{
		RecipeSceneView scene;
		if (!_impl || !_impl->mapped) return scene;
		auto& pack = *_impl->mapped;
		scene.mappedBytes = pack.header.committedPackBytes;
		scene.cells.reserve(keys.size());
		for (const auto& key : keys) {
			MappedCellResult cell;
			const auto entry = FindIndexEntry(pack.entries, key);
			if (entry == pack.entries.end() || entry->key != key) {
				cell.reason = "cell absent from index";
				++scene.missCount;
				scene.cells.push_back(std::move(cell));
				continue;
			}
			if (entry->size > kMaxCellBytes || entry->offset > pack.header.committedPackBytes ||
				entry->size > pack.header.committedPackBytes - entry->offset) {
				cell.status = LoadStatus::Corrupt; cell.reason = "cell range outside committed pack";
				++scene.missCount; scene.cells.push_back(std::move(cell)); continue;
			}
			const auto* begin = static_cast<const std::byte*>(pack.view) + entry->offset;
			const std::span bytes(begin, static_cast<std::size_t>(entry->size));
			if (HashBytes(bytes) != entry->checksum) {
				cell.status = LoadStatus::Corrupt; cell.reason = "cell checksum mismatch";
				++scene.missCount; scene.cells.push_back(std::move(cell)); continue;
			}
			auto parsed = ParseCellV6(bytes, cell.reason);
			if (!parsed || parsed->header.key != key) {
				cell.status = LoadStatus::Corrupt;
				if (cell.reason.empty()) cell.reason = "cell key mismatch";
				++scene.missCount; scene.cells.push_back(std::move(cell)); continue;
			}
			cell.status = LoadStatus::Hit;
			auto& view = cell.view;
			view._mappingOwner = std::static_pointer_cast<const void>(_impl->mapped);
			view._key = parsed->header.key;
			view._valid = true;
			view._authoritativeEmpty = (parsed->header.flags & 1u) != 0;
			view._sourceFingerprint = parsed->header.sourceFingerprint;
			view._skippedPlacements = parsed->header.skippedPlacements;
			view._groups = parsed->groups; view._importBatches = parsed->importBatches; view._materialOverrides = parsed->overrides;
			view._matrices = parsed->matrices; view._placementFormIDs = parsed->placements;
			view._preparedParts = parsed->parts; view._perObjectRows = parsed->rows;
			view._normalMatrices = parsed->normals; view._templates = parsed->templates;
			view._workloads = parsed->workloads; view._strings = parsed->strings;
			++scene.hitCount;
			scene.cells.push_back(std::move(cell));
		}
		return scene;
	}

	std::uint64_t CellRecipe::PlacementCount() const
	{
		std::uint64_t count = 0;
		for (const auto& group : groups) count += group.placementFormIDs.size();
		return count;
	}

	void FinalizeImportCosts(CellRecipe& recipe, std::uint64_t drawRecordStride, const StaticImportSettings& settings)
	{
		for (auto& group : recipe.groups) group.importCost = {};
		for (const auto& part : recipe.preparedParts) {
			const auto found = std::ranges::find(recipe.groups, part.sourceGroupID, &Group::stableGroupID);
			if (found == recipe.groups.end() || part.perObjectRowStride == 0 ||
				part.perObjectRows.size() % part.perObjectRowStride != 0) continue;
			auto& cost = found->importCost;
			const auto transforms = static_cast<std::uint64_t>(
				part.perObjectRows.size() / part.perObjectRowStride);
			const auto templates = static_cast<std::uint64_t>(part.templates.size());
			cost.transformRows += transforms;
			cost.drawRecords += transforms * templates;
			for (const auto& meshTemplate : part.templates) {
				cost.activeInsertIndices += transforms * meshTemplate.workloadKeys.size();
			}
			cost.preparedBytes += part.perObjectRows.size();
			cost.preparedBytes += part.normalMatrices.size() * sizeof(Matrix4x4);
			cost.preparedBytes += transforms * templates * drawRecordStride;
		}
		for (auto& group : recipe.groups) group.importCost.uploadBytes = group.importCost.preparedBytes;
		recipe.importBatches.clear();
		const auto placementLimit = settings.preparationPacketPlacements;
		const auto publishByteLimit = settings.maxTransactionPublishBytes;
		const auto byteLimit = publishByteLimit == 0 ? settings.preparationPacketBytes :
			(std::min)(settings.preparationPacketBytes, publishByteLimit);
		const auto partLimit = settings.maxTransactionGroups;
		const auto drawLimit = settings.maxTransactionDrawRecords;
		const auto indexLimit = settings.maxTransactionActiveInsertIndices;
		std::unordered_map<std::uint64_t, std::uint64_t> partsByGroup;
		partsByGroup.reserve(recipe.groups.size());
		for (const auto& part : recipe.preparedParts) ++partsByGroup[part.sourceGroupID];
		std::uint64_t partCount = 0;
		for (std::uint32_t index = 0; index < recipe.groups.size(); ++index) {
			const auto& group = recipe.groups[index];
			const auto groupParts = partsByGroup[group.stableGroupID];
			const auto placements = static_cast<std::uint64_t>(group.placementFormIDs.size());
			const auto exceeds = [](std::uint64_t current, std::uint64_t incoming, std::uint64_t limit) {
				return limit != 0 && (current > limit || incoming > limit - current);
			};
			if (!recipe.importBatches.empty()) {
				const auto& batch = recipe.importBatches.back();
				if (exceeds(batch.placementCount, placements, placementLimit) ||
					exceeds(batch.importCost.preparedBytes, group.importCost.preparedBytes, byteLimit) ||
					exceeds(partCount, groupParts, partLimit) ||
					exceeds(batch.importCost.drawRecords, group.importCost.drawRecords, drawLimit) ||
					exceeds(batch.importCost.activeInsertIndices, group.importCost.activeInsertIndices, indexLimit)) {
					partCount = 0;
				}
				else {
					auto& target = recipe.importBatches.back();
					++target.groupCount;
					target.placementCount += placements;
					target.importCost.transformRows += group.importCost.transformRows;
					target.importCost.drawRecords += group.importCost.drawRecords;
					target.importCost.activeInsertIndices += group.importCost.activeInsertIndices;
					target.importCost.preparedBytes += group.importCost.preparedBytes;
					target.importCost.uploadBytes += group.importCost.uploadBytes;
					partCount += groupParts;
					continue;
				}
			}
			ImportBatch batch;
			batch.firstGroup = index;
			batch.groupCount = 1;
			batch.placementCount = placements;
			batch.importCost = group.importCost;
			recipe.importBatches.push_back(batch);
			partCount = groupParts;
		}
		for (auto& batch : recipe.importBatches) {
			batch.stableBatchID = HashBytes(std::as_bytes(std::span{ &recipe.key, 1 }));
			const auto firstID = recipe.groups[batch.firstGroup].stableGroupID;
			const auto lastID = recipe.groups[batch.firstGroup + batch.groupCount - 1].stableGroupID;
			batch.stableBatchID = HashBytes(std::as_bytes(std::span{ &firstID, 1 }), batch.stableBatchID);
			batch.stableBatchID = HashBytes(std::as_bytes(std::span{ &lastID, 1 }), batch.stableBatchID);
			batch.stableBatchID = HashBytes(std::as_bytes(std::span{ &batch.groupCount, 1 }), batch.stableBatchID);
		}
	}

	std::uint64_t HashBytes(std::span<const std::byte> bytes, std::uint64_t seed)
	{
		for (const auto value : bytes) {
			seed ^= std::to_integer<unsigned char>(value);
			seed *= 1099511628211ull;
		}
		return seed;
	}

	std::uint64_t HashString(std::string_view value, std::uint64_t seed)
	{
		return HashBytes(std::as_bytes(std::span(value.data(), value.size())), seed);
	}

	std::string EnvironmentDirectoryName(const EnvironmentIdentity& identity)
	{
		std::ostringstream value;
		value << std::hex << std::setfill('0') << std::setw(16) << identity.fingerprint
			<< '-' << std::setw(16) << identity.settingsFingerprint << '-' << std::setw(16) << identity.abiFingerprint;
		return value.str();
	}

	Store::Store(fs::path cacheRoot, EnvironmentIdentity identity) :
		_root(std::move(cacheRoot) / "static_scene_recipes" / EnvironmentDirectoryName(identity)),
		_identity(identity), _state(std::make_shared<StoreState>()) {}

	fs::path Store::PackPath(const CellKey& key) const
	{
		std::ostringstream name;
		if (key.interiorCellFormID != 0) name << "interior_" << std::hex << std::setw(8) << std::setfill('0') << key.interiorCellFormID;
		else name << "world_" << std::hex << std::setw(8) << std::setfill('0') << key.worldFormID;
		return _root / (name.str() + ".ssrpack");
	}

	fs::path Store::IndexPath(const CellKey& key) const { auto path = PackPath(key); path += ".index"; return path; }

	OpenResult Store::OpenNamespace(const CellKey& key) const
	{
		OpenResult result;
		std::shared_ptr<StoreState::MappedPack> pack;
		const auto packPath = PackPath(key);
		const auto cacheKey = packPath.native();
		{
			std::lock_guard lock(_state->mutex);
			if (const auto cached = _state->packs.find(cacheKey); cached != _state->packs.end()) {
				pack = cached->second;
			} else {
				auto candidate = std::make_shared<StoreState::MappedPack>();
				candidate->entries = ReadIndex(IndexPath(key), _identity, key, candidate->header, result.reason);
				if (candidate->entries.empty()) {
					result.status = result.reason == "stale index identity" ? LoadStatus::Stale :
						(result.reason == "index not found" ? LoadStatus::Missing : LoadStatus::Corrupt);
					return result;
				}
				candidate->file.reset(CreateFileW(packPath.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
				if (!candidate->file || candidate->file.get() == INVALID_HANDLE_VALUE) {
					result.status = LoadStatus::IoError; result.reason = "open pack failed"; return result;
				}
				PackHeader header;
				DWORD read = 0;
				if (!ReadFile(candidate->file.get(), &header, sizeof(header), &read, nullptr) || read != sizeof(header) ||
					header.magic != kPackMagic || header.schema != kSchemaVersion || header.endian != kEndianMarker ||
					header.headerChecksum != HeaderChecksum(header) ||
					std::memcmp(&header.identity, &_identity, sizeof(_identity)) != 0 ||
					header.worldFormID != key.worldFormID || header.interiorCellFormID != key.interiorCellFormID) {
					result.status = LoadStatus::Corrupt; result.reason = "pack header invalid"; return result;
				}
				candidate->mapping.reset(CreateFileMappingW(candidate->file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
				if (!candidate->mapping) { result.status = LoadStatus::IoError; result.reason = "map creation failed"; return result; }
				candidate->view = MapViewOfFile(candidate->mapping.get(), FILE_MAP_READ, 0, 0,
					static_cast<SIZE_T>(candidate->header.committedPackBytes));
				if (!candidate->view) { result.status = LoadStatus::IoError; result.reason = "map view failed"; return result; }
				pack = candidate;
				_state->packs.emplace(cacheKey, std::move(candidate));
			}
		}
		auto impl = std::make_shared<MappedRecipePack::Impl>();
		impl->mapped = std::move(pack);
		result.pack = std::shared_ptr<MappedRecipePack>(new MappedRecipePack(std::move(impl)));
		result.status = LoadStatus::Hit;
		result.reason.clear();
		return result;
	}

	LoadResult Store::Load(const CellKey& key) const
	{
		const auto opened = OpenNamespace(key);
		if (!opened.pack) return { opened.status, std::nullopt, opened.reason, 0 };
		auto scene = opened.pack->LookupCells(std::span{ &key, 1 });
		if (scene.cells.empty() || scene.cells.front().status != LoadStatus::Hit) {
			const auto status = scene.cells.empty() ? LoadStatus::Missing : scene.cells.front().status;
			const auto reason = scene.cells.empty() ? std::string{ "cell lookup returned no result" } : scene.cells.front().reason;
			return { status, std::nullopt, reason, scene.mappedBytes };
		}
		const auto& view = scene.cells.front().view;
		auto copyString = [&](RecipeStringRef ref) { return std::string(view.String(ref)); };
		CellRecipe owned;
		owned.key = view.Key(); owned.authoritativeEmpty = view.AuthoritativeEmpty();
		owned.sourceFingerprint = view.SourceFingerprint(); owned.skippedPlacements = view.SkippedPlacements();
		owned.importBatches.assign(view.ImportBatches().begin(), view.ImportBatches().end());
		for (const auto& source : view.Groups()) {
			auto& group = owned.groups.emplace_back();
			group.stableGroupID = source.stableGroupID; group.materialOverrideHash = source.materialOverrideHash;
			group.preparedAuthoritative = (source.flags & 1u) != 0; group.modelPath = copyString(source.modelPath); group.name = copyString(source.name);
			group.importCost = source.importCost;
			for (const auto& item : view.MaterialOverrides().subspan(source.materialOverrides.first, source.materialOverrides.count)) {
				auto& target = group.materialOverrides.emplace_back();
				target.name3D = copyString(item.name3D); target.normalizedName3D = copyString(item.normalizedName3D);
				target.textureSetEditorID = copyString(item.textureSetEditorID); target.index3D = item.index3D; target.flags = item.flags;
				for (std::size_t i = 0; i < target.texturePaths.size(); ++i) target.texturePaths[i] = copyString(item.texturePaths[i]);
			}
			const auto matrices = view.Matrices().subspan(source.instanceWorldMatrices.first, source.instanceWorldMatrices.count);
			group.instanceWorldMatrices.assign(matrices.begin(), matrices.end());
			const auto placements = view.PlacementFormIDs().subspan(source.placementFormIDs.first, source.placementFormIDs.count);
			group.placementFormIDs.assign(placements.begin(), placements.end());
		}
		for (const auto& source : view.PreparedParts()) {
			auto& part = owned.preparedParts.emplace_back();
			part.sourceGroupID = source.sourceGroupID; part.stableGroupID = source.stableGroupID;
			part.allocationScopeID = source.allocationScopeID; part.perObjectRowStride = source.perObjectRowStride;
			const auto rows = view.PerObjectRows().subspan(source.perObjectRows.first, source.perObjectRows.count);
			part.perObjectRows.assign(rows.begin(), rows.end());
			const auto normals = view.NormalMatrices().subspan(source.normalMatrices.first, source.normalMatrices.count);
			part.normalMatrices.assign(normals.begin(), normals.end());
			for (const auto& sourceTemplate : view.Templates().subspan(source.templates.first, source.templates.count)) {
				auto& target = part.templates.emplace_back(); target.partIndex = sourceTemplate.partIndex; target.meshIndex = sourceTemplate.meshIndex;
				for (const auto& workload : view.Workloads().subspan(sourceTemplate.workloadKeys.first, sourceTemplate.workloadKeys.count)) {
					target.workloadKeys.push_back({ workload.compileFlags, copyString(workload.renderPhase), workload.clodOnly != 0 });
				}
			}
		}
		return { LoadStatus::Hit, std::move(owned), {}, scene.mappedBytes };

		// Kept temporarily as a shadow decoder until all v6 call sites use mapped views.
		LoadResult result;
		std::shared_ptr<StoreState::MappedPack> pack;
		const auto packPath = PackPath(key);
		const auto cacheKey = packPath.native();
		{
			std::lock_guard lock(_state->mutex);
			if (auto cached = _state->packs.find(cacheKey); cached != _state->packs.end()) {
				pack = cached->second;
			} else {
				auto candidate = std::make_shared<StoreState::MappedPack>();
				std::string indexReason;
				candidate->entries = ReadIndex(IndexPath(key), _identity, key, candidate->header, indexReason);
				if (candidate->entries.empty()) {
					result.status = indexReason == "stale index identity" ? LoadStatus::Stale :
						(indexReason == "index not found" ? LoadStatus::Missing : LoadStatus::Corrupt);
					result.reason = std::move(indexReason);
					return result;
				}
				candidate->file.reset(CreateFileW(packPath.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
				if (!candidate->file || candidate->file.get() == INVALID_HANDLE_VALUE) {
					result.status = LoadStatus::IoError; result.reason = "open pack failed"; return result;
				}
				PackHeader packHeader;
				DWORD read = 0;
				if (!ReadFile(candidate->file.get(), &packHeader, sizeof(packHeader), &read, nullptr) || read != sizeof(packHeader) ||
					packHeader.magic != kPackMagic || packHeader.schema != kSchemaVersion ||
					packHeader.headerChecksum != HeaderChecksum(packHeader) ||
					std::memcmp(&packHeader.identity, &_identity, sizeof(_identity)) != 0) {
					result.status = LoadStatus::Corrupt; result.reason = "pack header invalid"; return result;
				}
				candidate->mapping.reset(CreateFileMappingW(candidate->file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
				if (!candidate->mapping) { result.status = LoadStatus::IoError; result.reason = "map creation failed"; return result; }
				candidate->view = MapViewOfFile(candidate->mapping.get(), FILE_MAP_READ, 0, 0,
					static_cast<SIZE_T>(candidate->header.committedPackBytes));
				if (!candidate->view) { result.status = LoadStatus::IoError; result.reason = "map view failed"; return result; }
				pack = std::move(candidate);
				_state->packs.emplace(cacheKey, pack);
			}
		}
		const auto entry = FindIndexEntry(pack->entries, key);
		if (entry == pack->entries.end() || entry->key != key) { result.reason = "cell absent from index"; return result; }
		if (entry->size > kMaxCellBytes || entry->offset > pack->header.committedPackBytes ||
			entry->size > pack->header.committedPackBytes - entry->offset) {
			result.status = LoadStatus::Corrupt; result.reason = "cell range outside committed pack"; return result;
		}
		result.mappedBytes = pack->header.committedPackBytes;
		const auto* bytes = static_cast<const std::byte*>(pack->view) + entry->offset;
		const auto cellBytes = std::span(bytes, static_cast<std::size_t>(entry->size));
		if (HashBytes(cellBytes) != entry->checksum) { result.status = LoadStatus::Corrupt; result.reason = "cell checksum mismatch"; return result; }
		std::string reason;
		auto recipe = DeserializeCell(cellBytes, reason);
		if (!recipe || recipe->key != key) { result.status = LoadStatus::Corrupt; result.reason = reason.empty() ? "cell key mismatch" : reason; return result; }
		result.status = LoadStatus::Hit;
		result.recipe = std::move(recipe);
		return result;
	}

	PublishResult Store::PublishBulk(std::span<const CellRecipe> recipes) const
	{
		PublishResult result;
		if (recipes.empty()) { result.success = true; return result; }
		const auto namespaceKey = recipes.front().key;
		result.packPath = PackPath(namespaceKey);
		if (std::ranges::any_of(recipes, [&](const CellRecipe& recipe) {
			return recipe.key.worldFormID != namespaceKey.worldFormID ||
				recipe.key.interiorCellFormID != namespaceKey.interiorCellFormID;
		})) {
			result.reason = "bulk recipes span multiple pack namespaces";
			return result;
		}

		std::error_code ec;
		fs::create_directories(result.packPath.parent_path(), ec);
		if (ec) { result.reason = "create cache directory failed: " + ec.message(); return result; }
		UniqueHandle mutex(CreateMutexW(nullptr, FALSE, MutexName(result.packPath).c_str()));
		if (!mutex) { result.reason = "create bulk writer mutex failed"; return result; }
		const auto wait = WaitForSingleObject(mutex.get(), 120'000);
		if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { result.reason = "bulk writer lock timeout"; return result; }
		const auto releaseMutex = std::unique_ptr<void, decltype(&ReleaseMutex)>(mutex.get(), &ReleaseMutex);

		const auto suffix = ".tmp." + std::to_string(GetCurrentProcessId());
		auto temporaryPack = result.packPath; temporaryPack += suffix;
		auto temporaryIndex = IndexPath(namespaceKey); temporaryIndex += suffix;
		fs::remove(temporaryPack, ec);
		fs::remove(temporaryIndex, ec);
		UniqueHandle file(CreateFileW(temporaryPack.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!file || file.get() == INVALID_HANDLE_VALUE) { result.reason = "create temporary bulk pack failed"; return result; }
		PackHeader packHeader;
		packHeader.identity = _identity;
		packHeader.worldFormID = namespaceKey.worldFormID;
		packHeader.interiorCellFormID = namespaceKey.interiorCellFormID;
		packHeader.headerChecksum = HeaderChecksum(packHeader);
		if (!WriteAll(file.get(), &packHeader, sizeof(packHeader))) { result.reason = "write bulk pack header failed"; return result; }

		std::vector<IndexEntry> entries;
		entries.reserve(recipes.size());
		std::uint64_t packSize = sizeof(packHeader);
		for (const auto& recipe : recipes) {
			auto cellBytes = SerializeCellV6(recipe);
			std::string validationReason;
			const auto validated = ParseCellV6(cellBytes, validationReason);
			if (!validated || validated->header.key != recipe.key) {
				result.reason = "bulk cell validation failed: " + validationReason;
				file.reset(); fs::remove(temporaryPack, ec); return result;
			}
			const auto offset = AlignUp(packSize);
			if (offset > packSize) {
				std::vector<std::byte> padding(static_cast<std::size_t>(offset - packSize));
				if (!WriteAll(file.get(), padding.data(), padding.size())) { result.reason = "write bulk alignment failed"; return result; }
			}
			if (!WriteAll(file.get(), cellBytes.data(), cellBytes.size())) { result.reason = "write bulk cell failed"; return result; }
			entries.push_back(IndexEntry{ recipe.key, offset, cellBytes.size(), HashBytes(cellBytes) });
			packSize = offset + cellBytes.size();
			result.bytesWritten += cellBytes.size();
		}
		if (!FlushFileBuffers(file.get())) { result.reason = "flush bulk pack failed"; return result; }
		file.reset();
		std::ranges::sort(entries, [](const IndexEntry& lhs, const IndexEntry& rhs) {
			return std::tie(lhs.key.interiorCellFormID, lhs.key.cellY, lhs.key.cellX) <
				std::tie(rhs.key.interiorCellFormID, rhs.key.cellY, rhs.key.cellX);
		});
		IndexHeader indexHeader;
		indexHeader.identity = _identity;
		indexHeader.worldFormID = namespaceKey.worldFormID;
		indexHeader.interiorCellFormID = namespaceKey.interiorCellFormID;
		indexHeader.committedPackBytes = packSize;
		indexHeader.entryCount = entries.size();
		indexHeader.entriesChecksum = HashBytes(std::as_bytes(std::span(entries)));
		UniqueHandle indexFile(CreateFileW(temporaryIndex.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!indexFile || indexFile.get() == INVALID_HANDLE_VALUE ||
			!WriteAll(indexFile.get(), &indexHeader, sizeof(indexHeader)) ||
			!WriteAll(indexFile.get(), entries.data(), entries.size() * sizeof(IndexEntry)) ||
			!FlushFileBuffers(indexFile.get())) {
			result.reason = "write bulk index failed"; fs::remove(temporaryPack, ec); fs::remove(temporaryIndex, ec); return result;
		}
		indexFile.reset();
		if (!MoveFileExW(temporaryPack.c_str(), result.packPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
			!MoveFileExW(temporaryIndex.c_str(), IndexPath(namespaceKey).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			result.reason = "atomic bulk publish failed"; fs::remove(temporaryPack, ec); fs::remove(temporaryIndex, ec); return result;
		}
		{
			std::lock_guard lock(_state->mutex);
			_state->packs.erase(result.packPath.native());
		}
		result.success = true;
		return result;
	}

	PublishResult Store::PublishCells(std::span<const CellRecipe* const> recipes) const
	{
		PublishResult result;
		if (recipes.empty()) { result.success = true; return result; }
		if (std::ranges::any_of(recipes, [](const CellRecipe* recipe) { return recipe == nullptr; })) {
			result.reason = "runtime recipe list contains null cell"; return result;
		}
		const auto namespaceKey = recipes.front()->key;
		result.packPath = PackPath(namespaceKey);
		if (std::ranges::any_of(recipes, [&](const CellRecipe* recipe) {
			return recipe->key.worldFormID != namespaceKey.worldFormID ||
				recipe->key.interiorCellFormID != namespaceKey.interiorCellFormID;
		})) {
			result.reason = "runtime recipes span multiple pack namespaces";
			return result;
		}
		if (!fs::exists(IndexPath(namespaceKey)) || !fs::exists(result.packPath)) {
			std::vector<CellRecipe> owning;
			owning.reserve(recipes.size());
			for (const auto* recipe : recipes) owning.push_back(*recipe);
			return PublishBulk(owning);
		}
		UniqueHandle mutex(CreateMutexW(nullptr, FALSE, MutexName(result.packPath).c_str()));
		if (!mutex) { result.reason = "create runtime writer mutex failed"; return result; }
		// Runtime publication is opportunistic and must remain bounded.  The
		// preprocessor's complete-pack replacement retains the longer writer wait.
		const auto wait = WaitForSingleObject(mutex.get(), 2'000);
		if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { result.reason = "runtime writer lock timeout"; return result; }
		const auto releaseMutex = std::unique_ptr<void, decltype(&ReleaseMutex)>(mutex.get(), &ReleaseMutex);

		IndexHeader indexHeader;
		std::string indexReason;
		auto entries = ReadIndex(IndexPath(namespaceKey), _identity, namespaceKey, indexHeader, indexReason);
		if (entries.empty()) { result.reason = "reopen committed index failed: " + indexReason; return result; }
		UniqueHandle file(CreateFileW(result.packPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!file || file.get() == INVALID_HANDLE_VALUE) { result.reason = "open pack for append failed"; return result; }
		LARGE_INTEGER committed; committed.QuadPart = static_cast<LONGLONG>(indexHeader.committedPackBytes);
		// Existing readers intentionally retain their old mapping while a writer
		// publishes the next index generation. Windows will not truncate a file
		// with live mapped views, and truncation is unnecessary here: writing from
		// the committed offset overwrites any unreachable tail. The replacement
		// index remains the sole authority for the newly committed byte range.
		if (!SetFilePointerEx(file.get(), committed, nullptr, FILE_BEGIN)) {
			result.reason = "seek committed pack tail failed"; return result;
		}
		// Runtime recipe publication is an opportunistic cache fill. Closing the
		// handle makes the appended bytes visible before the index is replaced;
		// forcing both files to stable storage for every individual cell serialized
		// all import workers behind tens of milliseconds of disk latency. A process
		// or power failure can only lose/corrupt cache data, which the checksums make
		// detectable and rebuildable on the next load.
		for (const auto* recipe : recipes) {
			auto existing = FindIndexEntry(entries, recipe->key);
			if (existing != entries.end() && existing->key == recipe->key) {
				std::string committedReason;
				if (ValidateCommittedCell(result.packPath, indexHeader, *existing, committedReason)) continue;
			}
			auto bytes = SerializeCellV6(*recipe);
			std::string validationReason;
			const auto validated = ParseCellV6(bytes, validationReason);
			if (!validated || validated->header.key != recipe->key) {
				result.reason = "cell validation failed: " + validationReason; return result;
			}
			const auto offset = AlignUp(indexHeader.committedPackBytes);
			if (offset != indexHeader.committedPackBytes) {
				std::vector<std::byte> padding(static_cast<std::size_t>(offset - indexHeader.committedPackBytes));
				if (!WriteAll(file.get(), padding.data(), padding.size())) { result.reason = "write append alignment failed"; return result; }
			}
			if (!WriteAll(file.get(), bytes.data(), bytes.size())) {
				result.reason = "write appended cell failed"; return result;
			}
			const IndexEntry replacement{ recipe->key, offset, bytes.size(), HashBytes(bytes) };
			if (existing != entries.end() && existing->key == recipe->key) *existing = replacement;
			else entries.push_back(replacement);
			std::ranges::sort(entries, [](const IndexEntry& lhs, const IndexEntry& rhs) {
				return std::tie(lhs.key.interiorCellFormID, lhs.key.cellY, lhs.key.cellX) <
					std::tie(rhs.key.interiorCellFormID, rhs.key.cellY, rhs.key.cellX);
			});
			indexHeader.committedPackBytes = offset + bytes.size();
			result.bytesWritten += bytes.size();
		}
		file.reset();
		if (result.bytesWritten == 0) { result.success = true; result.reason = "cells already committed"; return result; }
		std::ranges::sort(entries, [](const IndexEntry& lhs, const IndexEntry& rhs) {
			return std::tie(lhs.key.interiorCellFormID, lhs.key.cellY, lhs.key.cellX) <
				std::tie(rhs.key.interiorCellFormID, rhs.key.cellY, rhs.key.cellX);
		});
		indexHeader.entryCount = entries.size();
		indexHeader.entriesChecksum = HashBytes(std::as_bytes(std::span(entries)));
		auto temporaryIndex = IndexPath(namespaceKey);
		temporaryIndex += ".tmp." + std::to_string(GetCurrentProcessId());
		UniqueHandle indexFile(CreateFileW(temporaryIndex.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!indexFile || indexFile.get() == INVALID_HANDLE_VALUE ||
			!WriteAll(indexFile.get(), &indexHeader, sizeof(indexHeader)) ||
			!WriteAll(indexFile.get(), entries.data(), entries.size() * sizeof(IndexEntry))) {
			result.reason = "write appended index failed"; return result;
		}
		indexFile.reset();
		if (!MoveFileExW(temporaryIndex.c_str(), IndexPath(namespaceKey).c_str(), MOVEFILE_REPLACE_EXISTING)) {
			result.reason = "publish appended index failed"; return result;
		}
		{
			std::lock_guard lock(_state->mutex);
			_state->packs.erase(result.packPath.native());
		}
		result.success = true;
		return result;
	}

	PublishResult Store::PublishCells(std::span<const CellRecipe> recipes) const
	{
		std::vector<const CellRecipe*> views;
		views.reserve(recipes.size());
		for (const auto& recipe : recipes) views.push_back(std::addressof(recipe));
		return PublishCells(views);
	}

	PublishResult Store::PublishCell(const CellRecipe& recipe) const
	{
		const CellRecipe* view = std::addressof(recipe);
		return PublishCells(std::span{ &view, 1 });
	}
}
