#pragma once

// Where the static import pipeline gets its placements.
//
// The pipeline streams cells around a target; which cells exist and what is placed in
// them is the host's scene data. SARP reads Skyrim's plugin load order; a standalone
// scene describes its own. A cell's placements reach the pipeline as an owning recipe
// (StaticSceneRecipe.h): the pipeline uses a cached recipe when the recipe cache has one
// and asks the source to group the cell's placements only on a cache miss.

#include <cstddef>
#include <cstdint>
#include <functional>

#include <BasicRenderer/StaticImport/StaticCellGeometry.h>
#include <BasicRenderer/StaticImport/StaticSceneRecipe.h>

namespace br::staticimport
{
	class IStaticPlacementSource
	{
	public:
		virtual ~IStaticPlacementSource() = default;

		// Loads the placement data on first use and returns whether it is available.
		// Called on the thread that publishes stream targets, before every update; the
		// pipeline streams nothing until it returns true, and from then on it must keep
		// returning true. A source whose load failed keeps returning false.
		virtual bool EnsureReady() = 0;

		// Identity of the loaded placement data; names the recipe cache environment.
		// Valid once EnsureReady returned true.
		[[nodiscard]] virtual recipe::EnvironmentIdentity RecipeEnvironment() const = 0;

		// The remaining queries are valid once EnsureReady returned true, from any
		// pipeline thread, and must not change afterwards.

		// Whether `cell` exists in the scene data (an interior, or an exterior grid cell
		// of its world).
		[[nodiscard]] virtual bool IsRealCell(const StaticCellKey& cell) const = 0;

		// The exterior cells of `worldFormID`, in a stable order.
		[[nodiscard]] virtual std::size_t ExteriorCellCount(std::uint32_t worldFormID) const = 0;
		virtual void ForEachExteriorCell(
			std::uint32_t worldFormID,
			const std::function<void(const StaticCellKey&)>& visit) const = 0;

		// Groups the placements of a real cell into an owning recipe, for a recipe cache
		// miss. Pipeline worker threads; may run concurrently for different cells.
		[[nodiscard]] virtual recipe::CellRecipe BuildCellRecipe(const recipe::CellKey& cell) const = 0;
	};
}
