#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "Render/Runtime/FrameWorkQueue.h"

namespace org { class PixelBuffer; class ResourceGroup; }

namespace br::render {
struct EnvironmentSHWork {
    std::shared_ptr<org::PixelBuffer> srcCubemap;
    std::uint32_t environmentIndex = 0;
    std::uint32_t cubemapResolution = 0;
};
struct EnvironmentConversionWork {
    std::shared_ptr<org::PixelBuffer> srcTexture, dstCubemap;
    std::uint32_t environmentIndex = 0;
    std::shared_ptr<org::ResourceGroup> sourceGroup;
    std::shared_ptr<std::mutex> publicationMutex;
    void Commit() const;
    void Discard() const { Commit(); }
};
struct EnvironmentPrefilterWork {
    std::shared_ptr<org::PixelBuffer> srcCubemap, dstPrefilteredCubemap;
    std::uint32_t baseResolution = 0, environmentIndex = 0;
    std::shared_ptr<org::ResourceGroup> sourceGroup;
    std::shared_ptr<std::mutex> publicationMutex;
    void Commit() const;
    void Discard() const { Commit(); }
};
using EnvironmentSHWorkQueue = org::runtime::FrameWorkQueue<EnvironmentSHWork>;
using EnvironmentConversionWorkQueue = org::runtime::FrameWorkQueue<EnvironmentConversionWork>;
using EnvironmentPrefilterWorkQueue = org::runtime::FrameWorkQueue<EnvironmentPrefilterWork>;

struct EnvironmentWorkServices {
    EnvironmentConversionWorkQueue conversion;
    EnvironmentPrefilterWorkQueue prefilter;
    EnvironmentSHWorkQueue sphericalHarmonics;

    void PublishTelemetry() const;
};
}
