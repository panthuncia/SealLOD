#include "VirtualGeometry/RayTracing/CLodRtShaderTable.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <rhi_helpers.h>

namespace br::render {

namespace {

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return alignment == 0 ? value : (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

std::shared_ptr<const CLodRtShaderTable> CLodRtShaderTable::Create(rhi::Device device, const RayTracingFeatureInfo& features,
    const org::PipelineState& pipeline, const Layout& layout, const char* debugName) {
    auto payload = pipeline.GetPayload();
    if (!payload || !payload->pso) throw std::invalid_argument("Shader table needs a created pipeline");
    if (!features.pipeline || features.shaderGroupHandleSize == 0) throw std::invalid_argument("Shader table needs ray tracing pipelines");

    const uint64_t handleSize = features.shaderGroupHandleSize;
    const auto groupCount = static_cast<uint32_t>(layout.pipelineGroups.size());
    std::vector<uint8_t> handles(handleSize * groupCount);
    if (!rhi::IsOk(device.GetRayTracingShaderGroupHandles(payload->pso.Get().GetHandle(), 0, groupCount, handles.data(), static_cast<uint32_t>(handles.size())))) {
        throw std::runtime_error("Failed to read shader group handles");
    }
    const auto handleOf = [&](const std::string& name) -> const uint8_t* {
        const auto found = std::ranges::find(layout.pipelineGroups, name);
        if (found == layout.pipelineGroups.end()) throw std::invalid_argument("Shader table names an unknown group: " + name);
        return handles.data() + handleSize * static_cast<size_t>(found - layout.pipelineGroups.begin());
    };

    const uint64_t stride = rhi::AlignRayTracingShaderRecordSize(handleSize, features.shaderTableStrideAlignment);
    const uint64_t baseAlignment = features.shaderGroupBaseAlignment;
    const uint64_t missOffset = AlignUp(stride, baseAlignment);
    const uint64_t missSize = stride * layout.miss.size();
    const uint64_t hitOffset = AlignUp(missOffset + missSize, baseAlignment);
    const uint64_t hitSize = stride * layout.hit.size();
    const uint64_t totalSize = hitOffset + hitSize;

    std::vector<uint8_t> records(totalSize);
    const auto writeRecord = [&](uint64_t offset, const std::string& name) {
        if (!rhi::WriteRayTracingShaderRecord(records.data() + offset, stride, handleOf(name), static_cast<uint32_t>(handleSize))) {
            throw std::runtime_error("Failed to write shader record for " + name);
        }
    };
    writeRecord(0, layout.rayGen);
    for (size_t index = 0; index < layout.miss.size(); ++index) writeRecord(missOffset + stride * index, layout.miss[index]);
    for (size_t index = 0; index < layout.hit.size(); ++index) writeRecord(hitOffset + stride * index, layout.hit[index]);

    rhi::ResourcePtr buffer;
    const auto description = rhi::helpers::ResourceDesc::Buffer(totalSize, rhi::HeapType::Upload, {}, debugName);
    if (!rhi::IsOk(device.CreateCommittedResource(description, buffer)) || !buffer) {
        throw std::runtime_error("Failed to create shader table buffer");
    }
    void* mapped = nullptr;
    buffer->Map(&mapped, 0, totalSize);
    if (!mapped) throw std::runtime_error("Failed to map shader table buffer");
    std::memcpy(mapped, records.data(), totalSize);
    buffer->Unmap(0, totalSize);

    const rhi::ResourceHandle handle = buffer->GetHandle();
    const auto addressOf = [&](uint64_t offset) { return device.GetBufferDeviceAddress({ handle, offset }); };

    std::shared_ptr<CLodRtShaderTable> table(new CLodRtShaderTable());
    table->m_payload = std::move(payload);
    table->m_buffer = std::move(buffer);
    table->m_rayGen = { handle, 0, stride, stride };
    if (missSize != 0) table->m_miss = { handle, missOffset, missSize, stride };
    if (hitSize != 0) table->m_hit = { handle, hitOffset, hitSize, stride };
    table->m_arguments.rayGenerationAddress = addressOf(0);
    table->m_arguments.rayGenerationSize = stride;
    if (missSize != 0) {
        table->m_arguments.missAddress = addressOf(missOffset);
        table->m_arguments.missSize = missSize;
        table->m_arguments.missStride = stride;
    }
    if (hitSize != 0) {
        table->m_arguments.hitGroupAddress = addressOf(hitOffset);
        table->m_arguments.hitGroupSize = hitSize;
        table->m_arguments.hitGroupStride = stride;
    }
    return table;
}

void CLodRtShaderTable::WriteDispatchArguments(rhi::Resource buffer, uint64_t offset, uint32_t width, uint32_t height, uint32_t depth) const {
    const rhi::RayTracingDispatchRaysArguments arguments = DispatchArguments(width, height, depth);
    void* mapped = nullptr;
    buffer.Map(&mapped, offset, sizeof(arguments));
    if (!mapped) throw std::runtime_error("Failed to map dispatch argument buffer");
    std::memcpy(mapped, &arguments, sizeof(arguments));
    buffer.Unmap(offset, sizeof(arguments));
}

} // namespace br::render
