#pragma once

// Sharded store of static group lifecycle records.
//
// The coordinator and the streaming pump both hold one.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <BasicTelemetry/Tracy.h>

#include <BasicRenderer/StaticImport/StaticCellGeometry.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>

namespace br::staticimport
{

	// The descriptor is built on the cell-preparation worker; the coordinator
	// seals it into its group's shard when it accepts the add and retains only a
	// lightweight locator. Graph packets share this immutable record instead of
	// copying strings, placement matrices, and recipe metadata into a global
	// pending map.
	class StaticGroupLifecycleStore
	{
	public:
		void Configure(std::uint32_t arenaConcurrency)
		{
			std::lock_guard lock(_configureMutex);
			if (!_shards.empty()) return;
			const auto requested = static_cast<std::size_t>((std::max)(2u, arenaConcurrency * 2u));
			const auto count = std::bit_ceil(requested);
			_shards.reserve(count);
			for (std::size_t index = 0; index < count; ++index) {
				_shards.push_back(std::make_unique<Shard>());
			}
			_mask = count - 1u;
		}

		void Insert(const std::shared_ptr<StaticGroupLifecycleRecord>& record)
		{
			if (!record || _shards.empty()) return;
			auto& shard = Select(record->groupID);
			std::lock_guard lock(shard.mutex);
			shard.records.insert_or_assign(record->groupID, record);
		}

		void Erase(
			std::uint64_t groupID,
			const StaticGroupLifecycleRecord* expected = nullptr)
		{
			if (_shards.empty()) return;
			auto& shard = Select(groupID);
			std::lock_guard lock(shard.mutex);
			const auto record = shard.records.find(groupID);
			if (record == shard.records.end() ||
				(expected && record->second.get() != expected)) return;
			shard.records.erase(record);
		}

		[[nodiscard]] std::size_t ShardCount() const noexcept { return _shards.size(); }

	private:
		struct Shard
		{
			std::mutex mutex;
			std::unordered_map<std::uint64_t, std::shared_ptr<StaticGroupLifecycleRecord>> records;
		};

		// Group ids are already well-mixed hashes; fold the high half in so the
		// low bits the mask keeps still see all of them.
		[[nodiscard]] Shard& Select(std::uint64_t groupID)
		{
			return *_shards[static_cast<std::size_t>(groupID ^ (groupID >> 32)) & _mask];
		}

		std::mutex _configureMutex;
		std::vector<std::unique_ptr<Shard>> _shards;
		std::size_t _mask{ 0 };
	};

	// Turns a prepared group into an unsealed lifecycle record (no revision, not in
	// the store), moving its payload out of `group`. `cell` is the cell that added
	// it (default for a group with no cell); `batchKey` is the host batch key, which
	// must be non-zero. StaticImportCoordinator::ApplyGroupChanges seals the record.
	[[nodiscard]] inline std::shared_ptr<StaticGroupLifecycleRecord> MakeStaticGroupLifecycleRecord(
		PreparedStaticGroup& group, const StaticCellKey& cell, std::uint64_t batchKey)
	{
		auto record = std::make_shared<StaticGroupLifecycleRecord>();
		record->groupID = group.groupID;
		auto& pending = record->descriptor;
		pending.cellKey = cell;
		pending.batchKey = batchKey;
		pending.materialVariant = std::move(group.materialVariant);
		pending.modelPath = std::move(group.modelPath);
		pending.mappedModelPath = group.mappedModelPath;
		pending.name = std::move(group.name);
		pending.instanceWorldMatrices = std::move(group.instanceWorldMatrices);
		pending.placementIDs = std::move(group.placementIDs);
		pending.recipePreparedParts = std::move(group.recipePreparedParts);
		pending.mappedRecipe = std::move(group.mappedRecipe);
		pending.recipeSceneBinding = std::move(group.recipeSceneBinding);
		pending.recipeVariantSlot = std::move(group.recipeVariantSlot);
		pending.mappedPreparedParts = group.mappedPreparedParts;
		pending.recipeVariantIndex = group.recipeVariantIndex;
		pending.recipePreparedAuthoritative = group.recipePreparedAuthoritative;
		pending.recipeImportCost = group.recipeImportCost;
		pending.placementCount = group.placementCount;
		pending.residencyClass = group.residencyClass;
		pending.residencyGeneration = group.residencyGeneration;
		return record;
	}

	// Turns a prepared cell's groups into unsealed lifecycle records under `batchKey`
	// (0: the cell's StaticCellKeyHash).
	inline void BuildPreparedStaticCellLifecycle(PreparedStaticCell& prepared, std::uint64_t batchKey = 0)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::BuildCellLifecycle");
		if (batchKey == 0) {
			batchKey = static_cast<std::uint64_t>(StaticCellKeyHash{}(prepared.key));
		}
		prepared.lifecycleGroups.reserve(prepared.groups.size());
		for (auto& group : prepared.groups) {
			prepared.lifecycleGroups.push_back(MakeStaticGroupLifecycleRecord(group, prepared.key, batchKey));
		}
		prepared.groups.clear();
		prepared.groups.shrink_to_fit();
		BT_PLOT("SARP.StaticImportLifecycle.CellRecords",
			static_cast<int64_t>(prepared.lifecycleGroups.size()));
	}
}
