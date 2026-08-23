#include "Managers/ReadbackManager.h"

#include <DirectXTex.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <thread>

#include <rhi_conversions_dx12.h>

#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Utilities/Utilities.h"

namespace br {

namespace {

void SaveCubemapReadbackToDds(
    const std::shared_ptr<Resource>& readbackBuffer,
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
    const std::shared_ptr<Resource>& readbackBuffer,
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
    m_readbackPass = std::make_shared<ReadbackPass>(*this);
}

void ReadbackManager::Initialize(rhi::Timeline readbackFence) {
    m_readbackFence = readbackFence;
    if (m_readbackPass) {
        m_readbackPass->Setup();
        m_readbackPass->SetReadbackFence(readbackFence);
    }
}

void ReadbackManager::RequestReadback(std::shared_ptr<PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback, bool cubemap) {
    std::scoped_lock lock(m_mutex);
    m_queuedReadbacks.push_back(ReadbackInfo{
        .cubemap = cubemap,
        .texture = std::move(texture),
        .outputFile = std::move(outputFile),
        .callback = std::move(callback)
        });
}

void ReadbackManager::ClearReadbacks() {
    std::scoped_lock lock(m_mutex);
    m_queuedReadbacks.clear();
}

void ReadbackManager::Cleanup() {
    std::scoped_lock lock(m_mutex);
    m_queuedReadbacks.clear();
    m_readbackRequests.clear();
    m_readbackPass.reset();
}

void ReadbackManager::ReadbackPass::RecordImmediateCommands(ImmediateExecutionContext& context) {
    std::vector<ReadbackInfo> readbacks;
    uint64_t fenceValue = 0;
    {
        std::scoped_lock lock(m_owner.m_mutex);
        if (m_owner.m_queuedReadbacks.empty()) {
            m_hasWork = false;
            m_pendingFenceValue = 0;
            return;
        }

        fenceValue = m_owner.AcquireNextFenceValue();
        m_hasWork = true;
        m_pendingFenceValue = fenceValue;
        readbacks = m_owner.m_queuedReadbacks;
    }

    auto& commandList = context.list;
    for (auto& readback : readbacks) {
        if (!readback.texture) {
            continue;
        }

        if (readback.cubemap) {
            m_owner.SaveCubemapToDDS(context.device, commandList, readback.texture, readback.outputFile, fenceValue);
        }
        else {
            m_owner.SaveTextureToDDS(context.device, commandList, readback.texture.get(), readback.outputFile, fenceValue);
        }
    }
}

PassReturn ReadbackManager::ReadbackPass::Execute(PassExecutionContext& context) {
    (void)context;

    if (!m_hasWork) {
        return { {} };
    }

    m_owner.ClearReadbacks();
    m_hasWork = false;
    const uint64_t fenceValue = m_pendingFenceValue;
    m_pendingFenceValue = 0;
    spdlog::debug(
        "ReadbackManager::ReadbackPass returning external fence timeline(idx={}, gen={}) value={}",
        m_readbackFence.GetHandle().index,
        m_readbackFence.GetHandle().generation,
        fenceValue);
    return { m_readbackFence, fenceValue };
}

void ReadbackManager::SaveCubemapToDDS(
    rhi::Device& device,
    org::imm::ImmediateCommandList& commandList,
    std::shared_ptr<PixelBuffer> cubemap,
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

    auto readbackBuffer = Buffer::CreateShared(rhi::HeapType::Readback, info.totalBytes);
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

    std::scoped_lock lock(m_mutex);
    m_readbackRequests.push_back(std::move(readbackRequest));
}

void ReadbackManager::SaveTextureToDDS(
    rhi::Device& device,
    org::imm::ImmediateCommandList& commandList,
    PixelBuffer* texture,
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

    auto readbackBuffer = Buffer::CreateShared(rhi::HeapType::Readback, info.totalBytes);
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

    std::scoped_lock lock(m_mutex);
    m_readbackRequests.push_back(std::move(readbackRequest));
}

void ReadbackManager::ProcessReadbackRequests() {
    std::scoped_lock lock(m_mutex);

    const auto completedValue = m_readbackFence.GetCompletedValue();

    std::vector<ReadbackRequest> remainingRequests;
    remainingRequests.reserve(m_readbackRequests.size());
    for (auto& request : m_readbackRequests) {
        if (completedValue >= request.fenceValue) {
            if (request.callback) {
                request.callback();
            }
        }
        else {
            remainingRequests.push_back(std::move(request));
        }
    }

    m_readbackRequests = std::move(remainingRequests);
}

} // namespace br
