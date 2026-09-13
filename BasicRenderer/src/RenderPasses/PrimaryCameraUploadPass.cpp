#include "RenderPasses/PrimaryCameraUploadPass.h"

#include "Render/RendererFrameInputs.h"
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

class CameraUploadLifecycle final : public org::PreparedLifecycleEffect {
public:
    explicit CameraUploadLifecycle(std::uint64_t revision) : m_revision(revision) {}
    void Submitted(org::SubmissionContext) const override {
        static std::atomic_uint64_t lastSubmittedRevision{0};
        const auto previous = lastSubmittedRevision.exchange(m_revision, std::memory_order_acq_rel);
        if (previous != 0 && m_revision < previous)
            basic_telemetry::AddCounter("SARP.Camera.Upload.RevisionRegression");
        else if (previous != 0 && m_revision != previous + 1)
            basic_telemetry::AddCounter("SARP.Camera.Upload.RevisionGap");
        basic_telemetry::AddCounter("SARP.Camera.Upload.Submitted");
        basic_telemetry::SetGauge("SARP.Camera.Upload.SubmittedRevision",
            static_cast<std::int64_t>(m_revision));
    }
    void Abandoned(org::AbandonReason) const override {
        basic_telemetry::AddCounter("SARP.Camera.Upload.Abandoned");
    }
private:
    std::uint64_t m_revision;
};
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

void PrimaryCameraUploadPass::Declare(org::PassBuilder& builder) {
    builder.WithCopyDest(m_cameraDestination).WithCopyDest(m_cullingDestination);
    for (const auto& staging : m_staging) builder.WithCopySource(staging);
    builder.PreferQueue(org::QueueKind::Graphics);
}

PreparedPrimaryCameraUpload PrimaryCameraUploadPass::Prepare(
    const org::PassPrepareContext& preparation) {
    const auto* inputs = preparation.preparationData->Get<RendererFrameInputs>();
    if (!inputs || inputs->FrameSlot() >= m_staging.size())
        throw std::logic_error("PrimaryCameraUploadPass requires valid frame-owned inputs");
    const auto& upload = inputs->PrimaryCameraUpload();
    if (!upload.viewID || upload.cameraBufferIndex != 0)
        throw std::logic_error("Primary camera upload must target reserved view slot zero");

    auto& staging = m_staging[inputs->FrameSlot()];
    void* mapped = nullptr;
    staging->GetAPIResource().Map(&mapped, 0, 0);
    if (!mapped) throw std::runtime_error("Failed to map primary camera upload staging buffer");
    std::memcpy(mapped, &upload.camera, sizeof(upload.camera));
    std::memcpy(static_cast<std::byte*>(mapped) + CullingOffset,
        &upload.cullingCamera, sizeof(upload.cullingCamera));
    staging->GetAPIResource().Unmap(0, UploadBytes);

    preparation.Reserve(std::make_shared<CameraUploadLifecycle>(upload.revision));
    basic_telemetry::AddCounter("SARP.Camera.Upload.Prepared");
    basic_telemetry::SetGauge("SARP.Camera.Upload.PreparedRevision",
        static_cast<std::int64_t>(upload.revision));
    return {
        preparation.CaptureResource(staging->GetGlobalResourceID()),
        preparation.CaptureResource(m_cameraDestination->GetGlobalResourceID()),
        preparation.CaptureResource(m_cullingDestination->GetGlobalResourceID()),
        upload.revision, upload.frameNumber };
}

void PrimaryCameraUploadPass::Record(const PreparedPrimaryCameraUpload& data,
    org::PassRecordContext& recording) {
    const auto source = recording.Resolve(data.staging).GetHandle();
    recording.Commands().CopyBufferRegion(
        recording.Resolve(data.cameraDestination).GetHandle(), 0, source, 0, sizeof(CameraInfo));
    recording.Commands().CopyBufferRegion(
        recording.Resolve(data.cullingDestination).GetHandle(), 0, source, CullingOffset,
        sizeof(CullingCameraInfo));
    basic_telemetry::AddCounter("SARP.Camera.Upload.Recorded");
    basic_telemetry::SetGauge("SARP.Camera.Upload.RecordedRevision",
        static_cast<std::int64_t>(data.revision));
}

} // namespace br::render
