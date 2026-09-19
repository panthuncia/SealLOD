#include "Managers/ReadbackManager.h"

#include <DirectXTex.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <thread>

#include <rhi_conversions_dx12.h>

#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Utilities/Utilities.h"

namespace br {

namespace {

void SaveCubemapReadbackToDds(
    const std::shared_ptr<org::Resource>& readbackBuffer,
    const std::vector<rhi::CopyableFootprint>& fps,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    uint32_t numMipLevels,
    const std::wstring& outputFile)
{
    void* mappedData = nullptr;
    readbackBuffer->GetAPIResource().Map(&mappedData);

    DirectX::ScratchImage scratchImage;
    scratchImage.InitializeCube(format, width, height, 1, numMipLevels);

    constexpr uint32_t faces = 6;
    for (uint32_t mipLevel = 0; mipLevel < numMipLevels; ++mipLevel) {
        for (uint32_t faceIndex = 0; faceIndex < faces; ++faceIndex) {
            const uint32_t subresourceIndex = CalcSubresource(mipLevel, faceIndex, 0, numMipLevels, faces);

            DirectX::Image image;
            image.width = fps[subresourceIndex].width;
            image.height = fps[subresourceIndex].height;
            image.format = format;
            image.rowPitch = static_cast<size_t>(fps[subresourceIndex].rowPitch);
            image.slicePitch = image.rowPitch * image.height;
            image.pixels = static_cast<uint8_t*>(mappedData) + fps[subresourceIndex].offset;

            const DirectX::Image* destImage = scratchImage.GetImage(mipLevel, faceIndex, 0);

            const size_t destRowPitch = destImage->rowPitch;
            const size_t srcRowPitch = image.rowPitch;
            const size_t rowCount = image.height;

            uint8_t* destPixels = destImage->pixels;
            uint8_t* srcPixels = image.pixels;

            for (size_t row = 0; row < rowCount; ++row) {
                std::memcpy(destPixels + row * destRowPitch, srcPixels + row * srcRowPitch, (std::min)(destRowPitch, srcRowPitch));
            }
        }
    }

    readbackBuffer->GetAPIResource().Unmap(0, 0);

    auto hr = DirectX::SaveToDDSFile(
        scratchImage.GetImages(),
        scratchImage.GetImageCount(),
        scratchImage.GetMetadata(),
        DirectX::DDS_FLAGS_NONE,
        outputFile.c_str());
    if (FAILED(hr)) {
        spdlog::error("Failed to save the cubemap to a .dds file!");
    }
}

void SaveTextureReadbackToDds(
    const std::shared_ptr<org::Resource>& readbackBuffer,
    const std::vector<rhi::CopyableFootprint>& fps,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT dxgiFmt,
    uint32_t numMipLevels,
    const std::wstring& outputFile)
{
    void* mappedData = nullptr;
    readbackBuffer->GetAPIResource().Map(&mappedData);

    DirectX::ScratchImage scratchImage;
    scratchImage.Initialize2D(
        dxgiFmt,
        width,
        height,
        1,
        numMipLevels);

    for (uint32_t mipLevel = 0; mipLevel < numMipLevels; ++mipLevel) {
        const uint32_t subresourceIndex = CalcSubresource(mipLevel, 0, 0, numMipLevels, 1);

        DirectX::Image src{};
        src.width = fps[subresourceIndex].width;
        src.height = fps[subresourceIndex].height;
        src.format = dxgiFmt;
        src.rowPitch = static_cast<size_t>(fps[subresourceIndex].rowPitch);
        src.slicePitch = src.rowPitch * src.height;
        src.pixels = static_cast<uint8_t*>(mappedData) + fps[subresourceIndex].offset;

        const DirectX::Image* dst = scratchImage.GetImage(mipLevel, 0, 0);

        const size_t dstRowPitch = dst->rowPitch;
        const size_t srcRowPitch = src.rowPitch;
        const size_t rows = src.height;

        uint8_t* dstPixels = dst->pixels;
        const uint8_t* srcPixels = src.pixels;

        for (size_t row = 0; row < rows; ++row) {
            std::memcpy(
                dstPixels + row * dstRowPitch,
                srcPixels + row * srcRowPitch,
                (std::min)(dstRowPitch, srcRowPitch));
        }
    }

    readbackBuffer->GetAPIResource().Unmap(0, 0);

    const auto hr = DirectX::SaveToDDSFile(
        scratchImage.GetImages(),
        scratchImage.GetImageCount(),
        scratchImage.GetMetadata(),
        DirectX::DDS_FLAGS_NONE,
        outputFile.c_str());

    if (FAILED(hr)) {
        spdlog::error("Failed to save the texture to a .dds file!");
    }
}

}

ReadbackManager::ReadbackManager() {
    m_state = std::make_shared<State>();
    m_readbackPass = std::make_shared<ReadbackPass>(m_state);
}

void ReadbackManager::Initialize(rhi::Timeline readbackFence) {
    m_readbackFence = readbackFence;
    if (m_readbackPass) {
        m_readbackPass->Setup();
        m_readbackPass->SetReadbackFence(readbackFence);
    }
}

void ReadbackManager::RequestReadback(std::shared_ptr<org::PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback, bool cubemap) {
    std::scoped_lock lock(m_state->mutex);
    if (!m_state->accepting) return;
    m_state->queuedReadbacks.push_back(ReadbackInfo{
        .cubemap = cubemap,
        .texture = std::move(texture),
        .outputFile = std::move(outputFile),
        .callback = std::move(callback)
        });
}

void ReadbackManager::ClearReadbacks() {
    std::scoped_lock lock(m_state->mutex);
    m_state->queuedReadbacks.clear();
}

void ReadbackManager::Cleanup() {
    std::scoped_lock lock(m_state->mutex);
    m_state->accepting = false;
    m_state->queuedReadbacks.clear();
    m_state->readbackRequests.clear();
    m_readbackPass.reset();
}

bool ReadbackManager::ReadbackPass::DeclaredResourcesChanged() const
{
    std::scoped_lock lock(m_state->mutex);
    return m_state->accepting && !m_state->queuedReadbacks.empty();
}

void ReadbackManager::ReadbackPass::Declare(org::PassBuilder& builder)
{
    std::scoped_lock lock(m_state->mutex);
    for (const auto& readback : m_state->queuedReadbacks)
        if (readback.texture) builder.WithCopySource(readback.texture);
    builder.PreferQueue(org::QueueKind::Graphics);
}

ReadbackManager::ReadbackFrameData ReadbackManager::ReadbackPass::Prepare(
    const org::PassPrepareContext& preparation)
{
    ReadbackFrameData frame;
    std::vector<ReadbackInfo> inputs;
    {
        std::scoped_lock lock(m_state->mutex);
        if (!m_state->accepting) return frame;
        inputs = std::move(m_state->queuedReadbacks);
        m_state->queuedReadbacks.clear();
    }
    if (inputs.empty()) return frame;

    const uint64_t fenceValue =
        m_state->nextFenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
    std::vector<ReadbackRequest> requests;
    auto device = DeviceManager::GetInstance().GetDevice();
    for (const auto& input : inputs) {
        if (!input.texture) continue;
        const uint32_t mipCount = input.texture->GetMipLevels();
        const uint32_t slices = input.cubemap ? 6u : 1u;
        std::vector<rhi::CopyableFootprint> footprints(mipCount * slices);
        rhi::FootprintRangeDesc range{};
        range.texture = input.texture->GetAPIResource().GetHandle();
        range.mipCount = mipCount;
        range.arraySize = slices;
        range.planeCount = 1;
        const auto info = device.GetCopyableFootprints(
            range, footprints.data(), static_cast<uint32_t>(footprints.size()));
        auto buffer = org::Buffer::CreateShared(rhi::HeapType::Readback, info.totalBytes);
        buffer->SetName("Readback");
        preparation.Retain(buffer);
        const auto source = preparation.CaptureResource(input.texture->GetGlobalResourceID());
        for (uint32_t slice = 0; slice < slices; ++slice) {
            for (uint32_t mip = 0; mip < mipCount; ++mip) {
                const uint32_t index = CalcSubresource(mip, slice, 0, mipCount, slices);
                frame.copies.push_back({source, buffer->GetAPIResource().GetHandle(),
                    footprints[index], mip, slice});
            }
        }
        const auto width = input.texture->GetWidth();
        const auto height = input.texture->GetHeight();
        const auto format = rhi::ToDxgi(input.texture->GetFormat());
        const auto output = input.outputFile;
        const auto callback = input.callback;
        ReadbackRequest request{};
        request.readbackBuffer = buffer;
        request.layouts = footprints;
        request.totalSize = info.totalBytes;
        request.outputFile = output;
        request.fenceValue = fenceValue;
        request.callback = [buffer, footprints, width, height, format, mipCount,
            output, callback, cubemap = input.cubemap]() {
            TaskSchedulerManager::GetInstance().Submit(TaskLane::Background,
                TaskDomain::Cleanup, "ReadbackManager::SaveDDS",
                [buffer, footprints, width, height, format, mipCount, output,
                    callback, cubemap]() {
                    if (cubemap) SaveCubemapReadbackToDds(buffer, footprints,
                        width, height, format, mipCount, output);
                    else SaveTextureReadbackToDds(buffer, footprints,
                        width, height, format, mipCount, output);
                    if (callback) callback();
                });
        };
        requests.push_back(std::move(request));
    }

    struct Reservation final : org::PreparedLifecycleEffect {
        std::shared_ptr<State> state;
        std::vector<ReadbackInfo> inputs;
        mutable std::vector<ReadbackRequest> requests;
        org::ExternalTimelinePoint signal{};
        mutable std::atomic<bool> resolved{false};
        std::span<const org::ExternalTimelinePoint> SignalsAfterCompletion() const override {
            return {&signal, 1u};
        }
        void Submitted(org::SubmissionContext) const override {
            if (resolved.exchange(true)) return;
            std::scoped_lock lock(state->mutex);
            for (auto& request : requests)
                state->readbackRequests.push_back(std::move(request));
        }
        void Abandoned(org::AbandonReason) const override {
            if (resolved.exchange(true)) return;
            std::scoped_lock lock(state->mutex);
            if (!state->accepting) return;
            for (auto& input : inputs)
                state->queuedReadbacks.push_back(std::move(input));
        }
    };
    auto reservation = std::make_shared<Reservation>();
    reservation->state = m_state;
    reservation->inputs = std::move(inputs);
    reservation->requests = std::move(requests);
    reservation->signal = {m_readbackFence, fenceValue};
    preparation.Reserve(std::move(reservation));
    return frame;
}

void ReadbackManager::ReadbackPass::Record(
    const ReadbackFrameData& frame, org::PassRecordContext& recording)
{
    for (const auto& copy : frame.copies) {
        rhi::BufferTextureCopyFootprint region{};
        region.texture = recording.Resolve(copy.source).GetHandle();
        region.buffer = copy.destination;
        region.mip = copy.mip;
        region.arraySlice = copy.slice;
        region.footprint = copy.footprint;
        recording.Commands().CopyTextureToBuffer(region);
    }
}

void ReadbackManager::SaveCubemapToDDS(
    rhi::Device& device,
    org::imm::ImmediateCommandList& commandList,
    std::shared_ptr<org::PixelBuffer> cubemap,
    const std::wstring& outputFile,
    uint64_t fenceValue)
{
    const uint32_t numMipLevels = cubemap->GetMipLevels();
    constexpr uint32_t faces = 6;
    const uint32_t numSubresources = faces * numMipLevels;

    std::vector<rhi::CopyableFootprint> fps;
    fps.resize(numSubresources);

    rhi::FootprintRangeDesc fr{};
    fr.texture = cubemap->GetAPIResource().GetHandle();
    fr.firstMip = 0;
    fr.mipCount = numMipLevels;
    fr.firstArraySlice = 0;
    fr.arraySize = faces;
    fr.firstPlane = 0;
    fr.planeCount = 1;
    fr.baseOffset = 0;

    auto info = device.GetCopyableFootprints(fr, fps.data(), static_cast<uint32_t>(fps.size()));
    assert(info.count == numSubresources);

    auto readbackBuffer = org::Buffer::CreateShared(rhi::HeapType::Readback, info.totalBytes);
    readbackBuffer->SetName("Readback");

    for (uint32_t mipLevel = 0; mipLevel < numMipLevels; ++mipLevel) {
        for (uint32_t faceIndex = 0; faceIndex < faces; ++faceIndex) {
            const uint32_t subresourceIndex = CalcSubresource(mipLevel, faceIndex, 0, numMipLevels, faces);

            rhi::CopyableFootprint footprint;
            footprint.offset = fps[subresourceIndex].offset;
            footprint.rowPitch = fps[subresourceIndex].rowPitch;
            footprint.width = fps[subresourceIndex].width;
            footprint.height = fps[subresourceIndex].height;
            footprint.depth = fps[subresourceIndex].depth;

            commandList.CopyTextureToBuffer(
                cubemap.get(),
                mipLevel,
                faceIndex,
                readbackBuffer.get(),
                footprint,
                0,
                0,
                0);
        }
    }

    const auto width = cubemap->GetWidth();
    const auto height = cubemap->GetHeight();
    const auto format = rhi::ToDxgi(cubemap->GetFormat());

    ReadbackRequest readbackRequest;
    readbackRequest.readbackBuffer = readbackBuffer;
    readbackRequest.layouts = fps;
    readbackRequest.totalSize = info.totalBytes;
    readbackRequest.outputFile = outputFile;
    readbackRequest.fenceValue = fenceValue;
    readbackRequest.callback = [=]() {
        TaskSchedulerManager::GetInstance().Submit(TaskLane::Background, TaskDomain::Cleanup, "ReadbackManager::SaveCubemapToDDS", [=]() {
            SaveCubemapReadbackToDds(readbackBuffer, fps, width, height, format, numMipLevels, outputFile);
        });
        };

    std::scoped_lock lock(m_state->mutex);
    m_state->readbackRequests.push_back(std::move(readbackRequest));
}

void ReadbackManager::SaveTextureToDDS(
    rhi::Device& device,
    org::imm::ImmediateCommandList& commandList,
    org::PixelBuffer* texture,
    const std::wstring& outputFile,
    uint64_t fenceValue)
{
    const uint32_t numMipLevels = texture->GetMipLevels();
    constexpr uint32_t faces = 1;
    const uint32_t numSubresources = numMipLevels * faces;

    std::vector<rhi::CopyableFootprint> fps(numSubresources);

    rhi::FootprintRangeDesc fr{};
    fr.texture = texture->GetAPIResource().GetHandle();
    fr.firstMip = 0;
    fr.mipCount = numMipLevels;
    fr.firstArraySlice = 0;
    fr.arraySize = faces;
    fr.firstPlane = 0;
    fr.planeCount = 1;
    fr.baseOffset = 0;

    auto info = device.GetCopyableFootprints(fr, fps.data(), static_cast<uint32_t>(fps.size()));
    assert(info.count == numSubresources);

    const auto width = texture->GetWidth();
    const auto height = texture->GetHeight();
    const auto dxgiFmt = rhi::ToDxgi(texture->GetFormat());

    auto readbackBuffer = org::Buffer::CreateShared(rhi::HeapType::Readback, info.totalBytes);
    readbackBuffer->SetName("Readback");

    for (uint32_t mipLevel = 0; mipLevel < numMipLevels; ++mipLevel) {
        const uint32_t subresourceIndex = CalcSubresource(mipLevel, 0, 0, numMipLevels, 1);

        rhi::CopyableFootprint footprint;
        footprint.offset = fps[subresourceIndex].offset;
        footprint.rowPitch = fps[subresourceIndex].rowPitch;
        footprint.width = fps[subresourceIndex].width;
        footprint.height = fps[subresourceIndex].height;
        footprint.depth = fps[subresourceIndex].depth;

        commandList.CopyTextureToBuffer(
            texture,
            mipLevel,
            0,
            readbackBuffer.get(),
            footprint,
            0,
            0,
            0);
    }

    ReadbackRequest readbackRequest;
    readbackRequest.readbackBuffer = readbackBuffer;
    readbackRequest.layouts = fps;
    readbackRequest.totalSize = info.totalBytes;
    readbackRequest.outputFile = outputFile;
    readbackRequest.fenceValue = fenceValue;
    readbackRequest.callback = [=]() {
        TaskSchedulerManager::GetInstance().Submit(TaskLane::Background, TaskDomain::Cleanup, "ReadbackManager::SaveTextureToDDS", [=]() {
            SaveTextureReadbackToDds(readbackBuffer, fps, width, height, dxgiFmt, numMipLevels, outputFile);
        });
        };

    std::scoped_lock lock(m_state->mutex);
    m_state->readbackRequests.push_back(std::move(readbackRequest));
}

void ReadbackManager::ProcessReadbackRequests() {
    const auto completedValue = m_readbackFence.GetCompletedValue();
    std::vector<std::function<void()>> completedCallbacks;
    std::vector<ReadbackRequest> remainingRequests;
    {
        std::scoped_lock lock(m_state->mutex);
        remainingRequests.reserve(m_state->readbackRequests.size());
        for (auto& request : m_state->readbackRequests) {
            if (completedValue >= request.fenceValue) {
                if (request.callback)
                    completedCallbacks.push_back(std::move(request.callback));
            }
            else {
                remainingRequests.push_back(std::move(request));
            }
        }
        m_state->readbackRequests = std::move(remainingRequests);
    }
    // User callbacks may request another readback; never invoke them while the
    // service-state mutex is held.
    for (auto& callback : completedCallbacks) callback();
}

} // namespace br
