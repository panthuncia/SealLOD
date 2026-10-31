#pragma once

// A placement source for the static import pipeline over an in-memory scene.
//
// The host lists its static placements (an asset key plus a world matrix each) and
// the source buckets them into exterior cells of one world: a square grid of
// `cellSize` world units on the X/Z plane (BasicRenderer is Y-up), cell (x, y) covering
// X in [x*cellSize, (x+1)*cellSize) and Z in [y*cellSize, (y+1)*cellSize). The pipeline
// then streams those cells around a stream target like any other cell source;
// MakeStreamTarget turns a world position (the camera's) into one.
//
// The placement list is fixed once the pipeline starts: add every placement before
// the first StaticImportRuntime::UpdateStreaming. Asset keys are passed through
// ImportedAssetLibrary::MakeAssetKey, so they name the same assets as the library does.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include <BasicRenderer/StaticImport/StaticPlacementSource.h>
#include <BasicRenderer/StaticImport/StaticStreamTarget.h>

namespace br::staticimport
{
	struct ScenePlacement
	{
		std::string assetKey;
		DirectX::XMFLOAT4X4 world{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	};

	class ScenePlacementSource final : public IStaticPlacementSource
	{
	public:
		struct Options
		{
			// Identity of the scene's single world; any non-zero id.
			std::uint32_t worldID = 1;
			float cellSize = 64.0f;
			// Names the scene in the recipe cache; placements and asset files are
			// hashed in as well.
			std::string sceneName = "scene";
		};

		ScenePlacementSource();
		explicit ScenePlacementSource(Options options);

		// Adds a placement; returns its placement id (1-based, in insertion order).
		// Only before the pipeline starts (EnsureReady freezes the list).
		std::uint32_t AddPlacement(std::string_view assetKey, DirectX::FXMMATRIX world);

		[[nodiscard]] std::size_t PlacementCount() const { return _placements.size(); }
		[[nodiscard]] const Options& GetOptions() const { return _options; }

		// The exterior cell containing a world position.
		[[nodiscard]] StaticCellKey CellAt(const DirectX::XMFLOAT3& position) const;
		// A valid stream target at a world position.
		[[nodiscard]] StaticStreamTarget MakeStreamTarget(const DirectX::XMFLOAT3& position) const;

		// ---- IStaticPlacementSource ----

		bool EnsureReady() override;
		[[nodiscard]] recipe::EnvironmentIdentity RecipeEnvironment() const override;
		[[nodiscard]] bool IsRealCell(const StaticCellKey& cell) const override;
		[[nodiscard]] std::size_t ExteriorCellCount(std::uint32_t worldFormID) const override;
		void ForEachExteriorCell(
			std::uint32_t worldFormID,
			const std::function<void(const StaticCellKey&)>& visit) const override;
		[[nodiscard]] recipe::CellRecipe BuildCellRecipe(const recipe::CellKey& cell) const override;

	private:
		Options _options;
		std::vector<ScenePlacement> _placements;
		// Filled by EnsureReady: placement indices per cell, cells in a stable order.
		std::unordered_map<StaticCellKey, std::vector<std::uint32_t>, StaticCellKeyHash> _placementsByCell;
		std::vector<StaticCellKey> _cells;
		recipe::EnvironmentIdentity _environment{};
		bool _ready{ false };
	};
}
