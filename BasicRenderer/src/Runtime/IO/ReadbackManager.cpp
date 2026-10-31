#include "Runtime/IO/ReadbackManager.h"
#include "Resources/Buffers/Buffer.h"
#include "OpenRenderGraph/ExternalClient.h"

#include <DirectXTex.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <thread>

#include <rhi_conversions_dx12.h>

#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "Runtime/Device/DeviceManager.h"
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
}

void ReadbackManager::Initialize(rhi::Timeline readbackFence) {
    m_readbackFence = readbackFence;
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

void ReadbackManager::Cleanup() {
    std::scoped_lock lock(m_state->mutex);
    m_state->accepting = false;
    m_state->queuedReadbacks.clear();
    m_state->readbackRequests.clear();
}

void ReadbackManager::Submit(org::ExternalClient& client, uint32_t queueSlot)
{
    std::vector<ReadbackInfo> inputs;
    {
        std::scoped_lock lock(m_state->mutex);
        if (!m_state->accepting || m_state->queuedReadbacks.empty()) return;
        inputs = std::move(m_state->queuedReadbacks);
        m_state->queuedReadbacks.clear();
    }

    struct Copy {
        rhi::ResourceHandle source{};
        rhi::ResourceHandle destination{};
        rhi::CopyableFootprint footprint{};
        uint32_t mip = 0, slice = 0;
    };
    std::vector<Copy> copies;
    std::vector<org::ExternalClient::Access> accesses;
    std::vector<ReadbackRequest> requests;
    const uint64_t fenceValue = m_state->nextFenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
    const org::ResourceState copySource{rhi::ResourceAccessType::CopySource, rhi::ResourceLayout::CopySource,
        rhi::ResourceSyncState::Copy};
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
        accesses.push_back({input.texture.get(), {}, copySource});
        for (uint32_t slice = 0; slice < slices; ++slice) {
            for (uint32_t mip = 0; mip < mipCount; ++mip) {
                const uint32_t index = CalcSubresource(mip, slice, 0, mipCount, slices);
                copies.push_back({input.texture->GetAPIResource().GetHandle(), buffer->GetAPIResource().GetHandle(),
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
    if (requests.empty()) return;

    // The textures stay alive through the copy because the requests and inputs hold them until it completes.
    const rhi::TimelinePoint signal{m_readbackFence.GetHandle(), fenceValue};
    try {
        client.Submit(queueSlot, accesses, [&](rhi::CommandList& list) {
            for (const auto& copy : copies) {
                rhi::BufferTextureCopyFootprint region{};
                region.texture = copy.source;
                region.buffer = copy.destination;
                region.mip = copy.mip;
                region.arraySlice = copy.slice;
                region.footprint = copy.footprint;
                list.CopyTextureToBuffer(region);
            }
        }, {&signal, 1}, std::make_shared<std::vector<ReadbackInfo>>(inputs));
    } catch (const std::exception& error) {
        // Nothing was submitted. Queue the requests again for the next frame.
        spdlog::error("ReadbackManager: readback submission failed: {}", error.what());
        std::scoped_lock lock(m_state->mutex);
        if (m_state->accepting)
            for (auto& input : inputs) m_state->queuedReadbacks.push_back(std::move(input));
        return;
    }
    std::scoped_lock lock(m_state->mutex);
    for (auto& request : requests)
        m_state->readbackRequests.push_back(std::move(request));
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
