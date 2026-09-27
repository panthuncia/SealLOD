#pragma once

#include "Render/PreparedPass.h"
#include <vector>

namespace br::render {

struct PreparedResourceClears {
    struct Clear {
        org::PreparedResourceReference resource;
        org::PreparedDescriptorReference cpu, gpu;
        float floatValue = 0;
        uint32_t uintValue = 0;
        bool isFloat = false;
    };
    struct Target { org::PreparedDescriptorReference descriptor; rhi::ClearValue value; };
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    std::vector<Clear> clears;
    std::vector<Target> targets;
};

inline void RecordPreparedResourceClears(const PreparedResourceClears& data, org::RecordingContext& recording) {
    auto& commands = recording.Commands();
    commands.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
    for (const auto& clear : data.clears) {
        rhi::UavClearInfo info{};
        info.resource = recording.Resolve(clear.resource);
        info.cpuVisible = recording.Resolve(clear.cpu);
        info.shaderVisible = recording.Resolve(clear.gpu);
        if (clear.isFloat) {
            rhi::UavClearFloat value{};
            for (auto& component : value.v) component = clear.floatValue;
            commands.ClearUavFloat(info, value);
        } else {
            rhi::UavClearUint value{};
            for (auto& component : value.v) component = clear.uintValue;
            commands.ClearUavUint(info, value);
        }
    }
    for (const auto& target : data.targets)
        commands.ClearRenderTargetView(recording.Resolve(target.descriptor), target.value);
}

} // namespace br::render
