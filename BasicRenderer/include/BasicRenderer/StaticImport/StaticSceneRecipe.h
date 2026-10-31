#pragma once

// Static scene recipes and their on-disk cache.
//
// A recipe is the import plan of one batch of static groups: the groups (model,
// material overrides, instance matrices, placement ids), the import batches the
// pipeline schedules and, once compiled, the prepared per-object rows of each group.
// Recipes persist in memory-mapped packs, one per batch key scope, under a directory
// named after the EnvironmentIdentity. The host decides what a batch is (SARP: one
// cell's placements), builds recipes from its own scene data and supplies the
// identity: `fingerprint` names the scene source (SARP hashes its plugin load order),
// `settingsFingerprint` the import settings that shaped the recipes.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <BasicRenderer/StaticImport/StaticImportSettings.h>

namespace br::staticimport::recipe
{
	struct StoreState;
	inline constexpr std::uint32_t kSchemaVersion = 8;
	inline constexpr std::uint32_t kMaterialTextureSlots = 14;

	// The host's key of a recipe batch, opaque to the store but for two rules: the
	// batches of one `scope` persist in one pack, and a pack's index orders its
	// batches by `id` read as two signed 32-bit halves, high half first
	// (BatchKeyLess). Its 16 bytes are part of the on-disk index.
	struct BatchKey
	{
		std::uint64_t scope{ 0 };
		std::uint64_t id{ 0 };
		bool operator==(const BatchKey&) const = default;
	};
	static_assert(sizeof(BatchKey) == 16);

	// The index order of the batches of one scope.
	[[nodiscard]] inline bool BatchKeyLess(const BatchKey& lhs, const BatchKey& rhs)
	{
		const auto high = [](std::uint64_t id) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(id >> 32u)); };
		const auto low = [](std::uint64_t id) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(id)); };
		if (lhs.scope != rhs.scope) return lhs.scope < rhs.scope;
		if (high(lhs.id) != high(rhs.id)) return high(lhs.id) < high(rhs.id);
		return low(lhs.id) < low(rhs.id);
	}

	struct BatchKeyHash
	{
		std::size_t operator()(const BatchKey& key) const noexcept
		{
			std::uint64_t value = key.scope;
			value ^= key.id + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
			return static_cast<std::size_t>(value);
		}
	};

	struct Matrix4x4
	{
		float values[16]{};
	};

	struct MaterialOverride
	{
		std::string name3D;
		std::string normalizedName3D;
		std::string textureSetEditorID;
		std::uint32_t index3D{ 0 };
		std::array<std::string, kMaterialTextureSlots> texturePaths;
		std::uint16_t flags{ 0 };
	};

	struct Group
	{
		std::uint64_t stableGroupID{ 0 };
		std::uint64_t materialOverrideHash{ 0 };
		bool preparedAuthoritative{ false };
		std::string modelPath;
		std::string name;
		std::vector<MaterialOverride> materialOverrides;
		std::vector<Matrix4x4> instanceWorldMatrices;
		std::vector<std::uint32_t> placementFormIDs;
		struct ImportCost
		{
			std::uint64_t transformRows{ 0 };
			std::uint64_t drawRecords{ 0 };
			std::uint64_t activeInsertIndices{ 0 };
			std::uint64_t preparedBytes{ 0 };
			std::uint64_t uploadBytes{ 0 };

			friend bool operator==(const ImportCost&, const ImportCost&) = default;
		};
		ImportCost importCost;
	};

	struct WorkloadKey
	{
		std::uint64_t compileFlags{ 0 };
		std::string renderPhase;
		bool clodOnly{ false };
	};

	struct PreparedTemplate
	{
		std::uint32_t partIndex{ 0 };
		std::uint32_t meshIndex{ 0 };
		std::vector<WorkloadKey> workloadKeys;
	};

	struct PreparedPart
	{
		std::uint64_t sourceGroupID{ 0 };
		std::uint64_t stableGroupID{ 0 };
		std::uint64_t allocationScopeID{ 0 };
		std::uint32_t perObjectRowStride{ 0 };
		std::vector<std::byte> perObjectRows;
		std::vector<Matrix4x4> normalMatrices;
		std::vector<PreparedTemplate> templates;
	};

	// A scheduling unit owns a contiguous range of geometry/material entries in
	// one recipe. The renderer still prepares each entry's templates separately.
	struct ImportBatch
	{
		std::uint64_t stableBatchID{ 0 };
		std::uint32_t firstGroup{ 0 };
		std::uint32_t groupCount{ 0 };
		std::uint64_t placementCount{ 0 };
		Group::ImportCost importCost;

		friend bool operator==(const ImportBatch&, const ImportBatch&) = default;
	};

	struct BatchRecipe
	{
		BatchKey key;
		bool authoritativeEmpty{ false };
		std::uint64_t sourceFingerprint{ 0 };
		std::uint64_t skippedPlacements{ 0 };
		std::vector<Group> groups;
		std::vector<ImportBatch> importBatches;
		std::vector<PreparedPart> preparedParts;

		[[nodiscard]] std::uint64_t PlacementCount() const;
	};

	inline void DiscardUnsupportedStaticRecipeGroups(BatchRecipe& recipe,
		std::span<const std::uint64_t> unsupportedGroupIDs)
	{
		if (unsupportedGroupIDs.empty()) return;
		std::erase_if(recipe.groups, [&](const Group& group) {
			if (std::ranges::find(unsupportedGroupIDs, group.stableGroupID) == unsupportedGroupIDs.end())
				return false;
			recipe.skippedPlacements += group.placementFormIDs.size();
			return true;
		});
		recipe.authoritativeEmpty = recipe.groups.empty();
	}

	struct RecipeRange
	{
		std::uint64_t first{ 0 };
		std::uint64_t count{ 0 };
	};

	struct RecipeStringRef
	{
		std::uint64_t offset{ 0 };
		std::uint64_t size{ 0 };
	};

	struct MappedMaterialOverride
	{
		RecipeStringRef name3D;
		RecipeStringRef normalizedName3D;
		RecipeStringRef textureSetEditorID;
		std::array<RecipeStringRef, kMaterialTextureSlots> texturePaths;
		std::uint32_t index3D{ 0 };
		std::uint16_t flags{ 0 };
		std::uint16_t reserved{ 0 };
	};

	struct MappedGroup
	{
		std::uint64_t stableGroupID{ 0 };
		std::uint64_t materialOverrideHash{ 0 };
		RecipeStringRef modelPath;
		RecipeStringRef name;
		RecipeRange materialOverrides;
		RecipeRange instanceWorldMatrices;
		RecipeRange placementFormIDs;
		Group::ImportCost importCost;
		std::uint32_t flags{ 0 };
		std::uint32_t reserved{ 0 };
	};

	struct MappedPreparedTemplate
	{
		std::uint32_t partIndex{ 0 };
		std::uint32_t meshIndex{ 0 };
		RecipeRange workloadKeys;
	};

	struct MappedWorkloadKey
	{
		std::uint64_t compileFlags{ 0 };
		RecipeStringRef renderPhase;
		std::uint8_t clodOnly{ 0 };
		std::array<std::uint8_t, 7> reserved{};
	};

	struct MappedPreparedPart
	{
		std::uint64_t sourceGroupID{ 0 };
		std::uint64_t stableGroupID{ 0 };
		std::uint64_t allocationScopeID{ 0 };
		std::uint32_t perObjectRowStride{ 0 };
		std::uint32_t reserved{ 0 };
		RecipeRange perObjectRows;
		RecipeRange normalMatrices;
		RecipeRange templates;
	};

	class BatchRecipeView
	{
	public:
		[[nodiscard]] bool Valid() const { return _valid; }
		[[nodiscard]] const BatchKey& Key() const { return _key; }
		[[nodiscard]] bool AuthoritativeEmpty() const { return _authoritativeEmpty; }
		[[nodiscard]] std::uint64_t SourceFingerprint() const { return _sourceFingerprint; }
		[[nodiscard]] std::uint64_t SkippedPlacements() const { return _skippedPlacements; }
		[[nodiscard]] std::span<const MappedGroup> Groups() const { return _groups; }
		[[nodiscard]] std::span<const ImportBatch> ImportBatches() const { return _importBatches; }
		[[nodiscard]] std::span<const MappedMaterialOverride> MaterialOverrides() const { return _materialOverrides; }
		[[nodiscard]] std::span<const Matrix4x4> Matrices() const { return _matrices; }
		[[nodiscard]] std::span<const std::uint32_t> PlacementFormIDs() const { return _placementFormIDs; }
		[[nodiscard]] std::span<const MappedPreparedPart> PreparedParts() const { return _preparedParts; }
		[[nodiscard]] std::span<const std::byte> PerObjectRows() const { return _perObjectRows; }
		[[nodiscard]] std::span<const Matrix4x4> NormalMatrices() const { return _normalMatrices; }
		[[nodiscard]] std::span<const MappedPreparedTemplate> Templates() const { return _templates; }
		[[nodiscard]] std::span<const MappedWorkloadKey> Workloads() const { return _workloads; }
		[[nodiscard]] std::string_view String(RecipeStringRef ref) const;

	private:
		friend class MappedRecipePack;
		friend class Store;
		std::shared_ptr<const void> _mappingOwner;
		BatchKey _key{};
		bool _valid{ false };
		bool _authoritativeEmpty{ false };
		std::uint64_t _sourceFingerprint{ 0 };
		std::uint64_t _skippedPlacements{ 0 };
		std::span<const MappedGroup> _groups;
		std::span<const ImportBatch> _importBatches;
		std::span<const MappedMaterialOverride> _materialOverrides;
		std::span<const Matrix4x4> _matrices;
		std::span<const std::uint32_t> _placementFormIDs;
		std::span<const MappedPreparedPart> _preparedParts;
		std::span<const std::byte> _perObjectRows;
		std::span<const Matrix4x4> _normalMatrices;
		std::span<const MappedPreparedTemplate> _templates;
		std::span<const MappedWorkloadKey> _workloads;
		std::span<const char> _strings;
	};

	struct EnvironmentIdentity
	{
		std::uint64_t fingerprint{ 0 };
		std::uint64_t settingsFingerprint{ 0 };
		std::uint64_t abiFingerprint{ 0 };
	};

	// Finalizes renderer-independent transaction costs from the serialized rows,
	// template ordinals, and workload keys. Both preprocessing and runtime miss
	// compilation call this before publication.
	// The import batches honour the preparation packet and transaction limits of
	// `settings` (zero means unbounded).
	void FinalizeImportCosts(BatchRecipe& recipe, std::uint64_t drawRecordStride, const StaticImportSettings& settings);

	enum class LoadStatus
	{
		Hit,
		Missing,
		Stale,
		Corrupt,
		IoError
	};

	struct LoadResult
	{
		LoadStatus status{ LoadStatus::Missing };
		std::optional<BatchRecipe> recipe;
		std::string reason;
		std::uint64_t mappedBytes{ 0 };
	};

	struct MappedBatchResult
	{
		LoadStatus status{ LoadStatus::Missing };
		BatchRecipeView view;
		std::string reason;
	};

	struct RecipeSceneView
	{
		std::vector<MappedBatchResult> batches;
		std::uint64_t mappedBytes{ 0 };
		std::uint64_t hitCount{ 0 };
		std::uint64_t missCount{ 0 };
	};

	class MappedRecipePack
	{
	public:
		[[nodiscard]] RecipeSceneView LookupBatches(std::span<const BatchKey> keys) const;
		[[nodiscard]] std::uint64_t MappedBytes() const;

	private:
		friend class Store;
		struct Impl;
		explicit MappedRecipePack(std::shared_ptr<Impl> impl) : _impl(std::move(impl)) {}
		std::shared_ptr<Impl> _impl;
	};

	struct OpenResult
	{
		LoadStatus status{ LoadStatus::Missing };
		std::shared_ptr<MappedRecipePack> pack;
		std::string reason;
	};

	struct PublishResult
	{
		bool success{ false };
		std::filesystem::path packPath;
		std::uint64_t bytesWritten{ 0 };
		std::string reason;
	};

	[[nodiscard]] std::uint64_t HashBytes(std::span<const std::byte> bytes, std::uint64_t seed = 14695981039346656037ull);
	[[nodiscard]] std::uint64_t HashString(std::string_view value, std::uint64_t seed = 14695981039346656037ull);
	[[nodiscard]] inline std::uint64_t AbiFingerprint()
	{
		// Recipe compatibility is governed by the serialized schema and the few
		// fixed-width payload layouts below, not by the source-control revision.
		// Including the revision abandoned a complete world cache after virtually
		// every development build even when its on-disk representation was unchanged.
		// Recipes compiled before unsupported-mesh pruning can still contain groups
		// whose only meshes the runtime intentionally discards.
		auto fingerprint = HashString("sarp-static-scene-recipe-abi-v2-unsupported-mesh-pruning");
		fingerprint ^= static_cast<std::uint64_t>(kSchemaVersion) << 48u;
		fingerprint ^= static_cast<std::uint64_t>(sizeof(Matrix4x4)) << 32u;
		fingerprint ^= static_cast<std::uint64_t>(sizeof(RecipeRange)) << 16u;
		fingerprint ^= static_cast<std::uint64_t>(sizeof(PreparedTemplate));
		fingerprint = HashBytes(std::as_bytes(std::span{ &fingerprint, 1 }), sizeof(ImportBatch));
		return fingerprint;
	}
	[[nodiscard]] std::string EnvironmentDirectoryName(const EnvironmentIdentity& identity);

	class Store
	{
	public:
		Store(std::filesystem::path cacheRoot, EnvironmentIdentity identity);

		[[nodiscard]] LoadResult Load(const BatchKey& key) const;
		[[nodiscard]] OpenResult OpenNamespace(const BatchKey& namespaceKey) const;
		[[nodiscard]] PublishResult PublishBulk(std::span<const BatchRecipe> recipes) const;
		// Appends multiple runtime-generated batches while publishing one new index
		// generation. Readers retaining an older mapping remain valid.
		[[nodiscard]] PublishResult PublishBatches(std::span<const BatchRecipe> recipes) const;
		[[nodiscard]] PublishResult PublishBatches(std::span<const BatchRecipe* const> recipes) const;
		[[nodiscard]] PublishResult PublishBatch(const BatchRecipe& recipe) const;
		[[nodiscard]] std::filesystem::path PackPath(const BatchKey& key) const;
		[[nodiscard]] std::filesystem::path IndexPath(const BatchKey& key) const;

	private:
		std::filesystem::path _root;
		EnvironmentIdentity _identity;
		std::shared_ptr<StoreState> _state;
	};
}
