#pragma once

// Where the static import pipeline streams around.
//
// The host publishes a target every frame; the pipeline derives the desired cell area,
// the stream key and cell priorities from it. The ids follow StaticCellGeometry.h: a
// target sits in world `worldFormID`, either inside interior `interiorCellFormID` or,
// when that is 0, in exterior grid cell (cellX, cellY).

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>

#include <BasicRenderer/StaticImport/StaticCellGeometry.h>

namespace br::staticimport
{
	struct StaticStreamTarget
	{
		bool valid{ false };
		std::uint32_t worldFormID{ 0 };
		std::uint32_t interiorCellFormID{ 0 };
		std::int32_t cellX{ 0 };
		std::int32_t cellY{ 0 };
		// Host-owned data the target was built from (SARP: its scene camera), for host
		// code that needs more than the cell position. Opaque to the pipeline.
		std::shared_ptr<const void> hostData;
	};

	// The stream key for a target streamed at the given cell radius.
	[[nodiscard]] inline StaticStreamKey MakeStaticStreamKey(const StaticStreamTarget& target, std::uint32_t radius)
	{
		return StaticStreamKey{
			target.worldFormID,
			target.interiorCellFormID,
			target.cellX,
			target.cellY,
			radius
		};
	}

	// Squared cell distance from the target to an exterior cell. An interior cell, or
	// any cell while the target is inside an interior, is at distance 0 when it is the
	// target's interior and at the maximum distance otherwise.
	[[nodiscard]] inline std::uint32_t StaticCellDistanceToTarget(const StaticStreamTarget& target, const StaticCellKey& cellKey)
	{
		if (cellKey.interiorCellFormID != 0 || target.interiorCellFormID != 0) {
			return cellKey.interiorCellFormID == target.interiorCellFormID ? 0u : std::numeric_limits<std::uint32_t>::max();
		}
		const auto dx = static_cast<std::int64_t>(cellKey.cellX) - static_cast<std::int64_t>(target.cellX);
		const auto dy = static_cast<std::int64_t>(cellKey.cellY) - static_cast<std::int64_t>(target.cellY);
		return static_cast<std::uint32_t>((std::min<std::int64_t>)(
			std::numeric_limits<std::uint32_t>::max(),
			dx * dx + dy * dy));
	}
}
