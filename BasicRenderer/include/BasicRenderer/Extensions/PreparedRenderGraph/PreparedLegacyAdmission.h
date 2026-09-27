#pragma once

#include <stdexcept>

#include "Render/PreparedPass.h"
#include "Render/PassExecutionContext.h"

namespace br::render {

// Transitional adapter for command-only passes whose legacy Execute callback
// is invoked synchronously by the ordered admission owner. It deliberately
// rejects dynamic submission effects; those require an owned reservation
// packet and must not be smuggled through recording.
template<class PassT>
struct PreparedLegacyAdmission {
    PassT* pass = nullptr;
    rhi::Device device;
    const org::IHostExecutionData* admissionData = nullptr;
    uint32_t frameIndex = 0;
    float deltaTime = 0.0f;
};

template<class PassT>
void RecordPreparedLegacyAdmission(
    const PreparedLegacyAdmission<PassT>& data,
    org::RecordingContext& recording) {
    if (!data.pass || !data.admissionData)
        throw std::logic_error("Incomplete legacy admission packet");
    org::PassExecutionContext execution{};
    execution.device = data.device;
    execution.commandList = recording.Commands();
    execution.frameIndex = data.frameIndex;
    execution.deltaTime = data.deltaTime;
    execution.hostData = data.admissionData;
    auto result = data.pass->Execute(execution);
    if (result.fence || result.fenceValue || !result.externalSignalsAfterCompletion.empty())
        throw std::logic_error("Legacy admission packet produced unsupported submission effects");
}

template<class PassT>
org::PreparedPass PrepareLegacyAdmission(
    PassT* pass,
    org::FramePreparationContext& preparation,
    rhi::Device device) {
    if (!pass || !preparation.admissionData) return {};
    PreparedLegacyAdmission<PassT> data{
        .pass = pass,
        .device = device,
        .admissionData = preparation.admissionData,
        .frameIndex = preparation.frameIndex,
        .deltaTime = preparation.deltaTime,
    };
    return org::PreparedPass::Make(std::move(data), &RecordPreparedLegacyAdmission<PassT>);
}

} // namespace br::render
