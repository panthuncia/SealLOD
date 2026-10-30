#pragma once

// Static-import cell identity and cell-area geometry.
//
// A static scene is divided into cells. A cell belongs to a world (worldFormID) and is either an exterior grid cell
// (interiorCellFormID == 0, at cellX/cellY) or a single interior (interiorCellFormID != 0). The identifiers are the
// host's: SARP uses Skyrim worldspace and cell form IDs; a standalone scene picks its own.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace br::staticimport
{
	struct StaticCellKey
	{
		std::uint32_t worldFormID{ 0 };
		std::uint32_t interiorCellFormID{ 0 };
		std::int32_t cellX{ 0 };
		std::int32_t cellY{ 0 };
		bool operator==(const StaticCellKey&) const = default;
	};

	struct StaticCellKeyHash
	{
		std::size_t operator()(const StaticCellKey& key) const noexcept
		{
			std::uint64_t hash = 14695981039346656037ull;
			const auto mix = [&hash](std::uint64_t value) {
				for (std::uint32_t byteIndex = 0; byteIndex < 8; ++byteIndex) {
					hash ^= static_cast<unsigned char>((value >> (byteIndex * 8u)) & 0xFFu);
					hash *= 1099511628211ull;
				}
			};
			mix(key.worldFormID);
			mix(key.interiorCellFormID);
			mix(static_cast<std::uint32_t>(key.cellX));
			mix(static_cast<std::uint32_t>(key.cellY));
			return static_cast<std::size_t>(hash);
		}
	};

	struct StaticStreamKey
	{
		std::uint32_t worldFormID{ 0 };
		std::uint32_t interiorCellFormID{ 0 };
		std::int32_t cellX{ 0 };
		std::int32_t cellY{ 0 };
		std::uint32_t radius{ 0 };
		bool operator==(const StaticStreamKey&) const = default;
	};

	struct StaticCellRect
	{
		std::uint32_t worldFormID{ 0 };
		std::int32_t minX{ 0 };
		std::int32_t maxX{ -1 };
		std::int32_t minY{ 0 };
		std::int32_t maxY{ -1 };

		[[nodiscard]] bool valid() const
		{
			return worldFormID != 0 && minX <= maxX && minY <= maxY;
		}

		[[nodiscard]] bool contains(const StaticCellKey& key) const
		{
			return valid() &&
				key.interiorCellFormID == 0 &&
				key.worldFormID == worldFormID &&
				key.cellX >= minX &&
				key.cellX <= maxX &&
				key.cellY >= minY &&
				key.cellY <= maxY;
		}

		[[nodiscard]] std::size_t size() const
		{
			if (!valid()) {
				return 0;
			}
			const auto width = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(maxX) - static_cast<std::int64_t>(minX) + 1);
			const auto height = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(maxY) - static_cast<std::int64_t>(minY) + 1);
			const auto count = width * height;
			return static_cast<std::size_t>((std::min<std::uint64_t>)(
				count,
				static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())));
		}
	};

	enum class StaticDesiredCellAreaKind : std::uint8_t
	{
		Empty,
		Interior,
		ExteriorRect
	};

	struct StaticDesiredCellArea
	{
		StaticDesiredCellAreaKind kind{ StaticDesiredCellAreaKind::Empty };
		StaticCellKey interior;
		StaticCellRect exterior;

		[[nodiscard]] bool contains(const StaticCellKey& key) const
		{
			switch (kind) {
			case StaticDesiredCellAreaKind::Interior:
				return key == interior;
			case StaticDesiredCellAreaKind::ExteriorRect:
				return exterior.contains(key);
			case StaticDesiredCellAreaKind::Empty:
				break;
			}
			return false;
		}

		[[nodiscard]] std::size_t size() const
		{
			switch (kind) {
			case StaticDesiredCellAreaKind::Interior:
				return 1;
			case StaticDesiredCellAreaKind::ExteriorRect:
				return exterior.size();
			case StaticDesiredCellAreaKind::Empty:
				break;
			}
			return 0;
		}

		void clear()
		{
			kind = StaticDesiredCellAreaKind::Empty;
			interior = {};
			exterior = {};
		}
	};

	inline std::int32_t ClampStaticCellCoord(std::int64_t value)
	{
		return static_cast<std::int32_t>((std::clamp<std::int64_t>)(
			value,
			(std::numeric_limits<std::int32_t>::min)(),
			(std::numeric_limits<std::int32_t>::max)()));
	}

	inline StaticCellRect MakeStaticCellRect(
		std::uint32_t worldFormID,
		std::int32_t centerX,
		std::int32_t centerY,
		std::uint32_t radius)
	{
		const auto r = static_cast<std::int64_t>(radius);
		return StaticCellRect{
			.worldFormID = worldFormID,
			.minX = ClampStaticCellCoord(static_cast<std::int64_t>(centerX) - r),
			.maxX = ClampStaticCellCoord(static_cast<std::int64_t>(centerX) + r),
			.minY = ClampStaticCellCoord(static_cast<std::int64_t>(centerY) - r),
			.maxY = ClampStaticCellCoord(static_cast<std::int64_t>(centerY) + r)
		};
	}

	inline std::optional<StaticCellRect> IntersectStaticCellRects(const StaticCellRect& lhs, const StaticCellRect& rhs)
	{
		if (!lhs.valid() || !rhs.valid() || lhs.worldFormID != rhs.worldFormID) {
			return std::nullopt;
		}
		StaticCellRect intersection{
			.worldFormID = lhs.worldFormID,
			.minX = (std::max)(lhs.minX, rhs.minX),
			.maxX = (std::min)(lhs.maxX, rhs.maxX),
			.minY = (std::max)(lhs.minY, rhs.minY),
			.maxY = (std::min)(lhs.maxY, rhs.maxY)
		};
		if (!intersection.valid()) {
			return std::nullopt;
		}
		return intersection;
	}

	inline void ExpandStaticCellRect(StaticCellRect& rect, std::uint32_t worldFormID, std::int32_t cellX, std::int32_t cellY)
	{
		if (!rect.valid()) {
			rect = StaticCellRect{
				.worldFormID = worldFormID,
				.minX = cellX,
				.maxX = cellX,
				.minY = cellY,
				.maxY = cellY
			};
			return;
		}
		if (rect.worldFormID != worldFormID) {
			return;
		}
		rect.minX = (std::min)(rect.minX, cellX);
		rect.maxX = (std::max)(rect.maxX, cellX);
		rect.minY = (std::min)(rect.minY, cellY);
		rect.maxY = (std::max)(rect.maxY, cellY);
	}

	template <class Fn>
	inline void ForEachStaticCellInRect(const StaticCellRect& rect, Fn&& fn)
	{
		if (!rect.valid()) {
			return;
		}
		for (std::int32_t y = rect.minY; y <= rect.maxY; ++y) {
			for (std::int32_t x = rect.minX; x <= rect.maxX; ++x) {
				fn(StaticCellKey{ rect.worldFormID, 0, x, y });
				if (x == (std::numeric_limits<std::int32_t>::max)()) {
					break;
				}
			}
			if (y == (std::numeric_limits<std::int32_t>::max)()) {
				break;
			}
		}
	}

	template <class Fn>
	inline void ForEachStaticCellInRectDifference(const StaticCellRect& rect, const StaticCellRect& excluded, Fn&& fn)
	{
		const auto overlap = IntersectStaticCellRects(rect, excluded);
		if (!overlap) {
			ForEachStaticCellInRect(rect, fn);
			return;
		}

		if (rect.minY < overlap->minY) {
			ForEachStaticCellInRect(StaticCellRect{ rect.worldFormID, rect.minX, rect.maxX, rect.minY, overlap->minY - 1 }, fn);
		}
		if (overlap->maxY < rect.maxY) {
			ForEachStaticCellInRect(StaticCellRect{ rect.worldFormID, rect.minX, rect.maxX, overlap->maxY + 1, rect.maxY }, fn);
		}
		if (rect.minX < overlap->minX) {
			ForEachStaticCellInRect(StaticCellRect{ rect.worldFormID, rect.minX, overlap->minX - 1, overlap->minY, overlap->maxY }, fn);
		}
		if (overlap->maxX < rect.maxX) {
			ForEachStaticCellInRect(StaticCellRect{ rect.worldFormID, overlap->maxX + 1, rect.maxX, overlap->minY, overlap->maxY }, fn);
		}
	}

	template <class Fn>
	inline void ForEachStaticCellInArea(const StaticDesiredCellArea& area, Fn&& fn)
	{
		switch (area.kind) {
		case StaticDesiredCellAreaKind::Interior:
			fn(area.interior);
			break;
		case StaticDesiredCellAreaKind::ExteriorRect:
			ForEachStaticCellInRect(area.exterior, fn);
			break;
		case StaticDesiredCellAreaKind::Empty:
			break;
		}
	}

	template <class Fn>
	inline void ForEachStaticCellInAreaDifference(
		const StaticDesiredCellArea& area,
		const StaticDesiredCellArea& excluded,
		Fn&& fn)
	{
		if (area.kind == StaticDesiredCellAreaKind::ExteriorRect &&
			excluded.kind == StaticDesiredCellAreaKind::ExteriorRect) {
			ForEachStaticCellInRectDifference(area.exterior, excluded.exterior, std::forward<Fn>(fn));
			return;
		}
		ForEachStaticCellInArea(area, [&](const StaticCellKey& cell) {
			if (!excluded.contains(cell)) {
				fn(cell);
			}
		});
	}

	inline bool StaticDesiredCellAreaEquals(const StaticDesiredCellArea& lhs, const StaticDesiredCellArea& rhs)
	{
		if (lhs.kind != rhs.kind) {
			return false;
		}
		switch (lhs.kind) {
		case StaticDesiredCellAreaKind::Interior:
			return lhs.interior == rhs.interior;
		case StaticDesiredCellAreaKind::ExteriorRect:
			return lhs.exterior.worldFormID == rhs.exterior.worldFormID &&
				lhs.exterior.minX == rhs.exterior.minX &&
				lhs.exterior.maxX == rhs.exterior.maxX &&
				lhs.exterior.minY == rhs.exterior.minY &&
				lhs.exterior.maxY == rhs.exterior.maxY;
		case StaticDesiredCellAreaKind::Empty:
			return true;
		}
		return false;
	}

	inline std::string FormatStaticStreamKey(const StaticStreamKey& key)
	{
		std::ostringstream stream;
		stream << "world=" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << key.worldFormID
			<< " interior=" << std::setw(8) << key.interiorCellFormID
			<< std::dec << " cell=(" << key.cellX << "," << key.cellY << ") radius=" << key.radius;
		return stream.str();
	}

	inline std::string FormatStaticCellKey(const StaticCellKey& key)
	{
		std::ostringstream stream;
		stream << "world=" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << key.worldFormID
			<< " interior=" << std::setw(8) << key.interiorCellFormID
			<< std::dec << " cell=(" << key.cellX << "," << key.cellY << ")";
		return stream.str();
	}
}
