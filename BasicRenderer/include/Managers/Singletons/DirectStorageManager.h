#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <rhi.h>
#include <string>
#include <vector>

struct ID3D12Fence;

namespace br {

enum class DirectStorageQueueKind : uint8_t {
    SystemMemory = 0,
    Gpu,
};

struct DirectStorageCapabilities {
    bool sdkAvailable = false;
    bool initialized = false;
    bool enabled = false;
    bool runtimeDisabled = false;
    bool systemMemoryQueueAvailable = false;
    bool gpuQueueAvailable = false;
    bool supportsQueue2 = false;
    bool supportsQueue3 = false;
    uint32_t stagingBufferSizeBytes = 0;
};

struct DirectStorageOpenFileResult {
    bool success = false;
    bool cached = false;
    std::string message;
};

struct DirectStorageTextureRegionCopy {
    uint64_t sourceOffset = 0;
    uint32_t sourceSizeBytes = 0;
    uint32_t uncompressedSizeBytes = 0;
    uint32_t subresourceIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
};

struct DirectStorageTextureSubresourceRangeCopy {
    uint64_t sourceOffset = 0;
    uint32_t sourceSizeBytes = 0;
    uint32_t uncompressedSizeBytes = 0;
    uint32_t firstSubresource = 0;
    uint32_t subresourceCount = 0;
};

struct DirectStorageBufferRegionCopy {
    uint64_t sourceOffset = 0;
    uint32_t sourceSizeBytes = 0;
    uint32_t uncompressedSizeBytes = 0;
    rhi::Resource destinationResource{};
    uint64_t destinationOffset = 0;
};

struct DirectStorageFencePoint {
    rhi::Timeline timeline;
    uint64_t value = 0;
};

enum class DirectStorageFenceWaitMode : uint8_t {
    BeforeDestinationWrite,
    BeforeGpuWork,
    BeforeSourceAccess,
};

enum class DirectStorageAsyncRequestState : uint8_t {
    Invalid = 0,
    Pending,
    Ready,
    Failed,
};

struct DirectStorageAsyncRequestStatus {
    DirectStorageAsyncRequestState state = DirectStorageAsyncRequestState::Invalid;
    ID3D12Fence* fence = nullptr;
    uint64_t fenceValue = 0;
    uint32_t hresult = 0;
    DirectStorageQueueKind queueKind = DirectStorageQueueKind::Gpu;
    std::string debugLabel;
    std::string message;
};

class DirectStorageAsyncRequestHandle {
public:
    DirectStorageAsyncRequestHandle() = default;

    bool IsValid() const noexcept;

private:
    struct State;

    explicit DirectStorageAsyncRequestHandle(std::shared_ptr<State> state);

    std::shared_ptr<State> m_state;

    friend class DirectStorageManager;
};


class DirectStorageManager {
public:
    static DirectStorageManager& GetInstance();

    void Initialize();
    void Cleanup();

    bool IsInitialized() const {
        return m_initialized;
    }

    bool IsAvailable() const {
        return m_available;
    }

    bool IsEnabled() const {
        return m_enabled;
    }

    bool IsRuntimeDisabled() const {
        return m_runtimeDisabled;
    }

    uint32_t GetStagingBufferSizeBytes() const {
        return m_stagingBufferSizeBytes;
    }

    const std::string& GetStatusMessage() const {
        return m_statusMessage;
    }

    DirectStorageCapabilities GetCapabilities() const;
    bool CanServiceQueue(DirectStorageQueueKind queueKind) const;

    DirectStorageOpenFileResult PrimeFileHandle(const std::wstring& path);
    void ReleaseFileHandle(const std::wstring& path);
    bool HasPrimedFileHandle(const std::wstring& path) const;
    bool ReadFileRegionToMemory(
        const std::wstring& path,
        uint64_t sourceOffset,
        uint32_t sourceSizeBytes,
        std::vector<std::byte>& outData,
        std::string* outMessage = nullptr);
    bool ReadFileToMemory(const std::wstring& path, std::vector<std::byte>& outData, std::string* outMessage = nullptr);
    DirectStorageAsyncRequestHandle EnqueueUploadBufferRegionsFromFile(
        const std::wstring& path,
        const std::vector<DirectStorageBufferRegionCopy>& regions,
        std::string* outMessage = nullptr);
    DirectStorageAsyncRequestHandle EnqueueUploadBufferRegionsFromFileAfterFence(
        const std::wstring& path,
        const std::vector<DirectStorageBufferRegionCopy>& regions,
        DirectStorageFencePoint waitPoint,
        DirectStorageFenceWaitMode waitMode,
        DirectStorageFencePoint completionPoint,
        std::string* outMessage = nullptr);
    bool UploadBufferRegionsFromFile(
        const std::wstring& path,
        const std::vector<DirectStorageBufferRegionCopy>& regions,
        std::string* outMessage = nullptr);
    DirectStorageAsyncRequestHandle EnqueueUploadTextureSubresourcesFromFile(
        const std::wstring& path,
        rhi::Resource destinationResource,
        uint64_t sourceOffset,
        uint32_t sourceSizeBytes,
        std::string* outMessage = nullptr);
    DirectStorageAsyncRequestHandle EnqueueUploadTextureSubresourceRangeFromFile(
        const std::wstring& path,
        rhi::Resource destinationResource,
        const DirectStorageTextureSubresourceRangeCopy& range,
        std::string* outMessage = nullptr);
    bool UploadTextureSubresourcesFromFile(
        const std::wstring& path,
        rhi::Resource destinationResource,
        uint64_t sourceOffset,
        uint32_t sourceSizeBytes,
        std::string* outMessage = nullptr);
    bool UploadTextureSubresourceRangeFromFile(
        const std::wstring& path,
        rhi::Resource destinationResource,
        const DirectStorageTextureSubresourceRangeCopy& range,
        std::string* outMessage = nullptr);
    DirectStorageAsyncRequestHandle EnqueueUploadTextureRegionsFromFile(
        const std::wstring& path,
        rhi::Resource destinationResource,
        const std::vector<DirectStorageTextureRegionCopy>& regions,
        std::string* outMessage = nullptr);
    bool UploadTextureRegionsFromFile(
        const std::wstring& path,
        rhi::Resource destinationResource,
        const std::vector<DirectStorageTextureRegionCopy>& regions,
        std::string* outMessage = nullptr);
    DirectStorageAsyncRequestStatus PollRequest(const DirectStorageAsyncRequestHandle& handle) const;
    bool WaitForRequest(const DirectStorageAsyncRequestHandle& handle, std::string* outMessage = nullptr) const;
    bool WaitForRequests(const std::vector<DirectStorageAsyncRequestHandle>& handles, std::string* outMessage = nullptr) const;

private:
    DirectStorageManager() = default;

    struct Impl;
    void RegisterActiveRequest(const DirectStorageAsyncRequestHandle& handle);

    std::unique_ptr<Impl> m_impl;
    bool m_initialized = false;
    bool m_available = false;
    bool m_enabled = false;
    bool m_runtimeDisabled = false;
    uint32_t m_stagingBufferSizeBytes = 0;
    std::string m_statusMessage;
};

}

using DirectStorageManager = br::DirectStorageManager;
using DirectStorageQueueKind = br::DirectStorageQueueKind;
using DirectStorageBufferRegionCopy = br::DirectStorageBufferRegionCopy;
using DirectStorageFencePoint = br::DirectStorageFencePoint;
using DirectStorageFenceWaitMode = br::DirectStorageFenceWaitMode;
using DirectStorageTextureSubresourceRangeCopy = br::DirectStorageTextureSubresourceRangeCopy;
using DirectStorageAsyncRequestHandle = br::DirectStorageAsyncRequestHandle;
using DirectStorageAsyncRequestState = br::DirectStorageAsyncRequestState;
using DirectStorageAsyncRequestStatus = br::DirectStorageAsyncRequestStatus;
