#pragma once

#include <cstdint>

enum class TextureSemantic : uint8_t {
    Unknown = 0,
    BaseColor,
    Emissive,
    Normal,
    Height,
    AO,
    Opacity,
    Metallic,
    Roughness,
    MetallicRoughness,
    OpenPBRColor,
    OpenPBRScalar,
};

enum class NormalMapConvention : uint8_t {
    DirectX = 0,
    OpenGL,
};

