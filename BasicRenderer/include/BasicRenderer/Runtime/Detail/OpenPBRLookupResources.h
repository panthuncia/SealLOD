#pragma once

#include <memory>

namespace org { class PixelBuffer; }
class TextureFactory;

struct OpenPBRLookupResources {
    std::shared_ptr<org::PixelBuffer> idealDielectricEnergyComplement;
    std::shared_ptr<org::PixelBuffer> idealDielectricAverageEnergyComplement;
    std::shared_ptr<org::PixelBuffer> idealDielectricReflectionRatio;
    std::shared_ptr<org::PixelBuffer> opaqueDielectricEnergyComplement;
    std::shared_ptr<org::PixelBuffer> opaqueDielectricAverageEnergyComplement;
    std::shared_ptr<org::PixelBuffer> idealMetalEnergyComplement;
    std::shared_ptr<org::PixelBuffer> idealMetalAverageEnergyComplement;
    std::shared_ptr<org::PixelBuffer> fuzzLTC;

    bool HasAny() const;
};

OpenPBRLookupResources CreateOpenPBRLookupResources(const TextureFactory& textureFactory);