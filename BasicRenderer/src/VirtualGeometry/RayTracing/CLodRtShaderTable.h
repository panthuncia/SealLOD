#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Render/PipelineState.h"
#include <rhi.h>

namespace br::render {

// The shader records of one state object, in an upload buffer that needs no barriers. Shader identifiers belong to the state
// object they came from, and a live reload replaces it, so a table holds that exact payload. A pass:
// - rebuilds the table in preparation when IsCurrent turns false,
// - binds Payload(), never the pipeline's current payload, which a reload can change between preparation and recording,
// - calls RecordingContext::Retain on the table, because recorded dispatches name its buffer.
class CLodRtShaderTable {
public:
    struct Layout {
        // All groups of the pipeline in creation order: RayTracingPipelineRecipe::GroupNames().
        std::vector<std::string> pipelineGroups;
        std::string rayGen;
        std::vector<std::string> miss;
        std::vector<std::string> hit;
    };

    // Throws when a group is unknown or the buffer can't be made.
    static std::shared_ptr<const CLodRtShaderTable> Create(rhi::Device device, const RayTracingFeatureInfo& features,
        const org::PipelineState& pipeline, const Layout& layout, const char* debugName);

    const std::shared_ptr<const org::PipelineStatePayload>& Payload() const noexcept { return m_payload; }
    bool IsCurrent(const org::PipelineState& pipeline) const noexcept { return pipeline.PeekPayload() == m_payload.get(); }

    rhi::RayTracingDispatchDesc DispatchDesc(uint32_t width, uint32_t height, uint32_t depth = 1) const noexcept {
        rhi::RayTracingDispatchDesc desc{};
        desc.rayGenerationShaderTable = m_rayGen;
        desc.missShaderTable = m_miss;
        desc.hitGroupTable = m_hit;
        desc.width = width;
        desc.height = height;
        desc.depth = depth;
        return desc;
    }
    rhi::RayTracingDispatchIndirectDesc IndirectDesc() const noexcept {
        rhi::RayTracingDispatchIndirectDesc desc{};
        desc.rayGenerationShaderTable = m_rayGen;
        desc.missShaderTable = m_miss;
        desc.hitGroupTable = m_hit;
        return desc;
    }
    // Arguments for TraceRaysIndirect. A compute step can take these as constants and write them to its output.
    rhi::RayTracingDispatchRaysArguments DispatchArguments(uint32_t width, uint32_t height, uint32_t depth = 1) const noexcept {
        rhi::RayTracingDispatchRaysArguments arguments = m_arguments;
        arguments.width = width;
        arguments.height = height;
        arguments.depth = depth;
        return arguments;
    }
    // Writes DispatchArguments into a CPU-visible buffer, which a dispatch may read without a barrier if it is on an upload heap.
    void WriteDispatchArguments(rhi::Resource buffer, uint64_t offset, uint32_t width, uint32_t height, uint32_t depth = 1) const;

private:
    CLodRtShaderTable() = default;

    std::shared_ptr<const org::PipelineStatePayload> m_payload;
    rhi::ResourcePtr m_buffer;
    rhi::RayTracingShaderTableRegion m_rayGen{};
    rhi::RayTracingShaderTableRegion m_miss{};
    rhi::RayTracingShaderTableRegion m_hit{};
    rhi::RayTracingDispatchRaysArguments m_arguments{};
};

} // namespace br::render
