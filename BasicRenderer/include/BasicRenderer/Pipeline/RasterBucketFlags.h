#pragma once

#include <cstdint>
#include <vector>

// Per-material flags for rasterization
enum MaterialRasterFlags : uint32_t {
	MaterialRasterFlagsNone = 0,
	MaterialRasterFlagsAlphaTest = 1 << 0,
	MaterialRasterFlagsDoubleSided = 1 << 1,
	MaterialRasterFlagsGeometricDisplacement = 1 << 2,
	MaterialRasterFlagsSkinned = 1 << 3,
	// Encoded as (required UV set count + 1), so legacy callers which do not
	// specify a span retain the conservative eight-UV interface.
	MaterialRasterFlagsForwardUvCountMask = 0xf << 4,
	MaterialRasterFlagsForwardVertexColor = 1 << 8,
	MaterialRasterFlagsForwardVertexColorKnown = 1 << 9,
	MaterialRasterFlagsForwardGlint = 1 << 10,
	MaterialRasterFlagsForwardGlintKnown = 1 << 11,
	MaterialRasterFlagsForwardCoat = 1 << 12,
	MaterialRasterFlagsForwardCoatKnown = 1 << 13,
	MaterialRasterFlagsForwardFuzz = 1 << 14,
	MaterialRasterFlagsForwardFuzzKnown = 1 << 15,
	MaterialRasterFlagsForwardMetal = 1 << 16,
	MaterialRasterFlagsForwardMetalKnown = 1 << 17,
	MaterialRasterFlagsForwardDiffuseRoughness = 1 << 18,
	MaterialRasterFlagsForwardDiffuseRoughnessKnown = 1 << 19,
	MaterialRasterFlagsForwardEmission = 1 << 20,
	MaterialRasterFlagsForwardEmissionKnown = 1 << 21,
};

inline constexpr MaterialRasterFlags WithForwardUvSetCount(MaterialRasterFlags flags, uint32_t count) {
	const uint32_t clampedCount = count > 8u ? 8u : count;
	return static_cast<MaterialRasterFlags>(
		(static_cast<uint32_t>(flags) & ~static_cast<uint32_t>(MaterialRasterFlagsForwardUvCountMask)) |
		((clampedCount + 1u) << 4));
}

inline constexpr uint32_t GetForwardUvSetCount(MaterialRasterFlags flags) {
	const uint32_t encoded =
		(static_cast<uint32_t>(flags) & static_cast<uint32_t>(MaterialRasterFlagsForwardUvCountMask)) >> 4;
	return encoded == 0u ? 8u : encoded - 1u;
}

inline constexpr MaterialRasterFlags WithForwardVertexColor(MaterialRasterFlags flags, bool enabled) {
	uint32_t value = static_cast<uint32_t>(flags) |
		static_cast<uint32_t>(MaterialRasterFlagsForwardVertexColorKnown);
	if (enabled) value |= static_cast<uint32_t>(MaterialRasterFlagsForwardVertexColor);
	else value &= ~static_cast<uint32_t>(MaterialRasterFlagsForwardVertexColor);
	return static_cast<MaterialRasterFlags>(value);
}

inline constexpr bool HasForwardVertexColor(MaterialRasterFlags flags) {
	const bool known = (flags & MaterialRasterFlagsForwardVertexColorKnown) != 0;
	return !known || (flags & MaterialRasterFlagsForwardVertexColor) != 0;
}

inline constexpr MaterialRasterFlags WithForwardGlint(MaterialRasterFlags flags, bool enabled) {
	uint32_t value = static_cast<uint32_t>(flags) |
		static_cast<uint32_t>(MaterialRasterFlagsForwardGlintKnown);
	if (enabled) value |= static_cast<uint32_t>(MaterialRasterFlagsForwardGlint);
	else value &= ~static_cast<uint32_t>(MaterialRasterFlagsForwardGlint);
	return static_cast<MaterialRasterFlags>(value);
}

inline constexpr bool HasForwardGlint(MaterialRasterFlags flags) {
	const bool known = (flags & MaterialRasterFlagsForwardGlintKnown) != 0;
	return !known || (flags & MaterialRasterFlagsForwardGlint) != 0;
}

inline constexpr MaterialRasterFlags WithForwardCoat(MaterialRasterFlags flags, bool enabled) {
	uint32_t value = static_cast<uint32_t>(flags) | static_cast<uint32_t>(MaterialRasterFlagsForwardCoatKnown);
	if (enabled) value |= static_cast<uint32_t>(MaterialRasterFlagsForwardCoat);
	else value &= ~static_cast<uint32_t>(MaterialRasterFlagsForwardCoat);
	return static_cast<MaterialRasterFlags>(value);
}

inline constexpr bool HasForwardCoat(MaterialRasterFlags flags) {
	return (flags & MaterialRasterFlagsForwardCoatKnown) == 0 || (flags & MaterialRasterFlagsForwardCoat) != 0;
}

inline constexpr MaterialRasterFlags WithForwardFuzz(MaterialRasterFlags flags, bool enabled) {
	uint32_t value = static_cast<uint32_t>(flags) | static_cast<uint32_t>(MaterialRasterFlagsForwardFuzzKnown);
	if (enabled) value |= static_cast<uint32_t>(MaterialRasterFlagsForwardFuzz);
	else value &= ~static_cast<uint32_t>(MaterialRasterFlagsForwardFuzz);
	return static_cast<MaterialRasterFlags>(value);
}

inline constexpr bool HasForwardFuzz(MaterialRasterFlags flags) {
	return (flags & MaterialRasterFlagsForwardFuzzKnown) == 0 || (flags & MaterialRasterFlagsForwardFuzz) != 0;
}

inline constexpr MaterialRasterFlags WithForwardMetal(MaterialRasterFlags flags, bool enabled) {
	uint32_t value = static_cast<uint32_t>(flags) | static_cast<uint32_t>(MaterialRasterFlagsForwardMetalKnown);
	if (enabled) value |= static_cast<uint32_t>(MaterialRasterFlagsForwardMetal);
	else value &= ~static_cast<uint32_t>(MaterialRasterFlagsForwardMetal);
	return static_cast<MaterialRasterFlags>(value);
}

inline constexpr bool HasForwardMetal(MaterialRasterFlags flags) {
	return (flags & MaterialRasterFlagsForwardMetalKnown) == 0 || (flags & MaterialRasterFlagsForwardMetal) != 0;
}

inline constexpr MaterialRasterFlags WithForwardDiffuseRoughness(MaterialRasterFlags flags, bool enabled) {
	uint32_t value = static_cast<uint32_t>(flags) |
		static_cast<uint32_t>(MaterialRasterFlagsForwardDiffuseRoughnessKnown);
	if (enabled) value |= static_cast<uint32_t>(MaterialRasterFlagsForwardDiffuseRoughness);
	else value &= ~static_cast<uint32_t>(MaterialRasterFlagsForwardDiffuseRoughness);
	return static_cast<MaterialRasterFlags>(value);
}

inline constexpr bool HasForwardDiffuseRoughness(MaterialRasterFlags flags) {
	return (flags & MaterialRasterFlagsForwardDiffuseRoughnessKnown) == 0 ||
		(flags & MaterialRasterFlagsForwardDiffuseRoughness) != 0;
}

inline constexpr MaterialRasterFlags WithForwardEmission(MaterialRasterFlags flags, bool enabled) {
	uint32_t value = static_cast<uint32_t>(flags) | static_cast<uint32_t>(MaterialRasterFlagsForwardEmissionKnown);
	if (enabled) value |= static_cast<uint32_t>(MaterialRasterFlagsForwardEmission);
	else value &= ~static_cast<uint32_t>(MaterialRasterFlagsForwardEmission);
	return static_cast<MaterialRasterFlags>(value);
}

inline constexpr bool HasForwardEmission(MaterialRasterFlags flags) {
	return (flags & MaterialRasterFlagsForwardEmissionKnown) == 0 ||
		(flags & MaterialRasterFlagsForwardEmission) != 0;
}

// operators
inline MaterialRasterFlags operator|(MaterialRasterFlags a, MaterialRasterFlags b) {
	return static_cast<MaterialRasterFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline MaterialRasterFlags operator|=(MaterialRasterFlags& a, MaterialRasterFlags b) {
	a = static_cast<MaterialRasterFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	return a;
}
