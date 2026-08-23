#pragma once

#include <memory>

namespace org { class PixelBuffer; }
using org::PixelBuffer;
class TextureFactory;

struct OpenPBRLookupResources {
    std::shared_ptr<PixelBuffer> idealDielectricEnergyComplement;
    std::shared_ptr<PixelBuffer> idealDielectricAverageEnergyComplement;
    std::shared_ptr<PixelBuffer> idealDielectricReflectionRatio;
    std::shared_ptr<PixelBuffer> opaqueDielectricEnergyComplement;
    std::shared_ptr<PixelBuffer> opaqueDielectricAverageEnergyComplement;
    std::shared_ptr<PixelBuffer> idealMetalEnergyComplement;
    std::shared_ptr<PixelBuffer> idealMetalAverageEnergyComplement;
    std::shared_ptr<PixelBuffer> fuzzLTC;

    bool HasAny() const;
};

OpenPBRLookupResources CreateOpenPBRLookupResources(const TextureFactory& textureFactory);