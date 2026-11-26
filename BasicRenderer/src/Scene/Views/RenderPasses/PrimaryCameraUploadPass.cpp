#include "Scene/Views/RenderPasses/PrimaryCameraUploadPass.h"

#include "Runtime/Frame/RendererFrameInputs.h"
#include "Resources/Buffers/Buffer.h"
#include "Render/PassBuilders.h"
#include "Render/PreparedPass.h"
#include <BasicTelemetry/Telemetry.h>

#include <cstring>
#include <stdexcept>

namespace br::render {
namespace {
constexpr std::uint64_t CullingOffset = (sizeof(CameraInfo) + 15u) & ~std::uint64_t{15u};
constexpr std::uint64_t UploadBytes = CullingOffset + sizeof(CullingCameraInfo);
}

PrimaryCameraUploadPass::PrimaryCameraUploadPass(
    std::shared_ptr<org::Resource> cameraDestination,
    std::shared_ptr<org::Resource> cullingDestination, std::uint32_t frameSlotCount)
    : m_cameraDestination(std::move(cameraDestination)),
      m_cullingDestination(std::move(cullingDestination)) {
    m_staging.reserve(frameSlotCount);
    for (std::uint32_t slot = 0; slot < frameSlotCount; ++slot) {
        auto buffer = org::Buffer::CreateShared(rhi::HeapType::Upload, UploadBytes, false);
        buffer->SetName("PrimaryCameraUpload_" + std::to_string(slot));
        m_staging.push_back(std::move(buffer));
    }
}

PrimaryCameraUploadBindings PrimaryCameraUploadPass::Declare(org::PassBuilder& builder) {
    PrimaryCameraUploadBindings bindings;
    bindings.cameraDestination = builder.CopyDestination(m_cameraDestination);
    bindings.cullingDestination = builder.CopyDestination(m_cullingDestination);
    for (const auto& staging : m_staging) bindings.staging.push_back(builder.CopySource(staging));
    builder.PreferQueue(org::QueueKind::Graphics);
    // The copy takes this run's staging slot (PrimaryCameraUploadRun): recorded at execution, in a batch of its own.
    builder.RecordAtExecution();
    return bindings;
}

// Each run, kept recording or not: the frame slot's staging buffer gets this frame's cameras. A frame slot's buffer is not
// written while a run that copies from it is in flight (frame pacing waits for the slot's last run first).
void PrimaryCameraUploadPass::Update(const org::UpdateExecutionContext& execution) {
    const auto* inputs = execution.hostData ? execution.hostData->Get<RendererFrameInputs>() : nullptr;
    if (!inputs || inputs->FrameSlot() >= m_staging.size())
        throw std::logic_error("PrimaryCameraUploadPass requires valid frame-owned inputs");
    const auto& upload = inputs->PrimaryCameraUpload();
    if (!upload.viewID || upload.cameraBufferIndex != 0)
        throw std::logic_error("Primary camera upload must target reserved view slot zero");

    const auto& staging = m_staging[inputs->FrameSlot()];
    void* mapped = nullptr;
    staging->GetAPIResource().Map(&mapped, 0, 0);
    if (!mapped) throw std::runtime_error("Failed to map primary camera upload staging buffer");
    std::memcpy(mapped, &upload.camera, sizeof(upload.camera));
    std::memcpy(static_cast<std::byte*>(mapped) + CullingOffset,
        &upload.cullingCamera, sizeof(upload.cullingCamera));
    staging->GetAPIResource().Unmap(0, UploadBytes);

    // Revision order of what is uploaded (formerly counted at submission, which a recording made at execution has no
    // callback for).
    if (m_run->revision != 0 && upload.revision < m_run->revision)
        basic_telemetry::AddCounter("SARP.Camera.Upload.RevisionRegression");
    else if (m_run->revision != 0 && upload.revision != m_run->revision + 1)
        basic_telemetry::AddCounter("SARP.Camera.Upload.RevisionGap");
    *m_run = {inputs->FrameSlot(), upload.revision, upload.frameNumber};
    basic_telemetry::AddCounter("SARP.Camera.Upload.Prepared");
    basic_telemetry::SetGauge("SARP.Camera.Upload.PreparedRevision",
        static_cast<std::int64_t>(upload.revision));
}

PreparedPrimaryCameraUpload PrimaryCameraUploadPass::Prepare(
    const PrimaryCameraUploadBindings&, const org::PassPrepareContext&) const {
    return {m_run};
}

void PrimaryCameraUploadPass::Record(const PrimaryCameraUploadBindings& bindings,
    const PreparedPrimaryCameraUpload& prepared, org::PassRecordContext& recording) {
    const auto& data = *prepared.run;
    const auto source = recording.Resolve(bindings.staging.at(data.stagingSlot)).GetHandle();
    recording.Commands().CopyBufferRegion(
        recording.Resolve(bindings.cameraDestination).GetHandle(), 0, source, 0, sizeof(CameraInfo));
    recording.Commands().CopyBufferRegion(
        recording.Resolve(bindings.cullingDestination).GetHandle(), 0, source, CullingOffset,
        sizeof(CullingCameraInfo));
    basic_telemetry::AddCounter("SARP.Camera.Upload.Recorded");
    basic_telemetry::SetGauge("SARP.Camera.Upload.RecordedRevision",
        static_cast<std::int64_t>(data.revision));
}

} // namespace br::render
