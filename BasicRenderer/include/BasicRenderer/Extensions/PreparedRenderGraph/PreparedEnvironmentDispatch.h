#pragma once

#include "Render/PreparedPass.h"
#include <array>
#include <vector>

namespace br::render {
// Face/mip workloads for the environment shaders' small custom root layouts.
struct PreparedEnvironmentDispatch {
    struct Face { std::array<uint32_t, 5> constants{}; uint32_t groups = 0; };
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    org::PreparedProgramReference program;
    uint32_t constantCount = 0;
    std::vector<Face> faces;
};
inline void RecordEnvironmentDispatch(const PreparedEnvironmentDispatch& data, org::RecordingContext& recording) {
    if (data.faces.empty()) return;
    auto& commands = recording.Commands();
    commands.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
    commands.BindLayout(recording.ResolveLayout(data.program));
    commands.BindPipeline(recording.Resolve(data.program));
    for (const auto& face : data.faces) {
        commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, data.constantCount, face.constants.data());
        commands.Dispatch(face.groups, face.groups, 1);
    }
}
} // namespace br::render
