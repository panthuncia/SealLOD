#include <BasicRenderer/StaticImport/ScenePlacementSource.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <span>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include <BasicRenderer/StaticImport/ImportedAssetLibrary.h>

namespace br::staticimport
{
	namespace
	{
		template <class T>
		std::uint64_t HashPod(const T& value, std::uint64_t seed)
		{
			return recipe::HashBytes(std::as_bytes(std::span{ &value, 1 }), seed);
		}

		std::int32_t CellCoordinate(float position, float cellSize)
		{
			const double cell = std::floor(static_cast<double>(position) / static_cast<double>(cellSize));
			return static_cast<std::int32_t>(std::clamp(cell,
				static_cast<double>((std::numeric_limits<std::int32_t>::min)()),
				static_cast<double>((std::numeric_limits<std::int32_t>::max)())));
		}
	}

	ScenePlacementSource::ScenePlacementSource()
		: ScenePlacementSource(Options{})
	{
	}

	ScenePlacementSource::ScenePlacementSource(Options options)
		: _options(std::move(options))
	{
		if (_options.worldID == 0) _options.worldID = 1;
		if (!(_options.cellSize > 0.0f)) _options.cellSize = 64.0f;
	}

	std::uint32_t ScenePlacementSource::AddPlacement(std::string_view assetKey, DirectX::FXMMATRIX world)
	{
		if (_ready) {
			spdlog::error("ScenePlacementSource: placement '{}' added after the pipeline started; ignored", assetKey);
			return 0;
		}
		ScenePlacement placement;
		placement.assetKey = ImportedAssetLibrary::MakeAssetKey(assetKey);
		DirectX::XMStoreFloat4x4(&placement.world, world);
		_placements.push_back(std::move(placement));
		return static_cast<std::uint32_t>(_placements.size());
	}

	StaticCellKey ScenePlacementSource::CellAt(const DirectX::XMFLOAT3& position) const
	{
		return StaticCellKey{
			_options.worldID,
			0u,
			CellCoordinate(position.x, _options.cellSize),
			CellCoordinate(position.z, _options.cellSize)
		};
	}

	StaticStreamTarget ScenePlacementSource::MakeStreamTarget(const DirectX::XMFLOAT3& position) const
	{
		const auto cell = CellAt(position);
		return StaticStreamTarget{
			.valid = true,
			.worldFormID = cell.worldFormID,
			.interiorCellFormID = 0u,
			.cellX = cell.cellX,
			.cellY = cell.cellY,
		};
	}

	bool ScenePlacementSource::EnsureReady()
	{
		if (_ready) return true;

		_placementsByCell.clear();
		_cells.clear();
		for (std::uint32_t index = 0; index < _placements.size(); ++index) {
			const auto& world = _placements[index].world;
			const auto cell = CellAt(DirectX::XMFLOAT3{ world._41, world._42, world._43 });
			auto [it, inserted] = _placementsByCell.try_emplace(cell);
			if (inserted) _cells.push_back(cell);
			it->second.push_back(index);
		}
		std::ranges::sort(_cells, [](const StaticCellKey& lhs, const StaticCellKey& rhs) {
			return std::pair{ lhs.cellY, lhs.cellX } < std::pair{ rhs.cellY, rhs.cellX };
		});

		// The recipe cache is keyed by everything a recipe is built from: the scene, its
		// placements and (through their size and write time) the asset files.
		std::uint64_t fingerprint = recipe::HashString(_options.sceneName);
		std::map<std::string, bool> assetFiles;
		for (const auto& placement : _placements) {
			fingerprint = recipe::HashString(placement.assetKey, fingerprint);
			fingerprint = HashPod(placement.world, fingerprint);
			assetFiles.emplace(placement.assetKey, true);
		}
		for (const auto& [file, unused] : assetFiles) {
			(void)unused;
			std::error_code error;
			const auto size = std::filesystem::file_size(file, error);
			if (!error) fingerprint = HashPod(size, fingerprint);
			const auto writeTime = std::filesystem::last_write_time(file, error);
			if (!error) fingerprint = HashPod(writeTime.time_since_epoch().count(), fingerprint);
		}
		_environment.fingerprint = fingerprint;
		_environment.settingsFingerprint = HashPod(std::bit_cast<std::uint32_t>(_options.cellSize),
			recipe::HashString("br-scene-placement-source-v1"));
		_environment.abiFingerprint = recipe::AbiFingerprint();

		_ready = true;
		spdlog::info("ScenePlacementSource: scene '{}' placements={} cells={} cellSize={} world={}",
			_options.sceneName, _placements.size(), _cells.size(), _options.cellSize, _options.worldID);
		return true;
	}

	recipe::EnvironmentIdentity ScenePlacementSource::RecipeEnvironment() const
	{
		return _environment;
	}

	bool ScenePlacementSource::IsRealCell(const StaticCellKey& cell) const
	{
		return cell.interiorCellFormID == 0 && _placementsByCell.contains(cell);
	}

	std::size_t ScenePlacementSource::ExteriorCellCount(std::uint32_t worldFormID) const
	{
		return worldFormID == _options.worldID ? _cells.size() : 0u;
	}

	void ScenePlacementSource::ForEachExteriorCell(
		std::uint32_t worldFormID,
		const std::function<void(const StaticCellKey&)>& visit) const
	{
		if (worldFormID != _options.worldID) return;
		for (const auto& cell : _cells) visit(cell);
	}

	recipe::CellRecipe ScenePlacementSource::BuildCellRecipe(const recipe::CellKey& cell) const
	{
		recipe::CellRecipe result;
		result.key = cell;
		const auto found = _placementsByCell.find(cell);
		if (found == _placementsByCell.end()) {
			result.authoritativeEmpty = true;
			return result;
		}

		std::unordered_map<std::string_view, std::size_t> groupByAsset;
		std::uint64_t sourceHash = HashPod(cell, recipe::HashString("br-scene-cell"));
		for (const auto index : found->second) {
			const auto& placement = _placements[index];
			const auto placementID = index + 1u;
			sourceHash = HashPod(placementID, sourceHash);
			sourceHash = recipe::HashString(placement.assetKey, sourceHash);
			sourceHash = HashPod(placement.world, sourceHash);

			auto [it, inserted] = groupByAsset.try_emplace(placement.assetKey, result.groups.size());
			if (inserted) {
				auto& group = result.groups.emplace_back();
				group.stableGroupID = recipe::HashString(placement.assetKey, HashPod(cell, recipe::HashString("br-scene-group")));
				group.modelPath = placement.assetKey;
				group.name = std::filesystem::path(placement.assetKey).filename().string();
			}
			auto& group = result.groups[it->second];
			recipe::Matrix4x4 matrix{};
			static_assert(sizeof(matrix.values) == sizeof(placement.world));
			std::memcpy(matrix.values, &placement.world, sizeof(matrix.values));
			group.instanceWorldMatrices.push_back(matrix);
			group.placementFormIDs.push_back(placementID);
		}
		result.authoritativeEmpty = result.groups.empty();
		result.sourceFingerprint = sourceHash;
		return result;
	}
}
