#pragma once

#include <rhi.h>
#include <rhi_allocator.h>

namespace br {
class DeviceManager {
public:
    static DeviceManager& GetInstance();

    void Initialize();
    void Cleanup();

    rhi::Device GetDevice() const {
        return m_device.Get();
    }

    rhi::Queue GetGraphicsQueue() const {
        return m_graphicsQueue;
    }

    rhi::Queue GetComputeQueue() const {
        return m_computeQueue;
    }

    rhi::Queue GetCopyQueue() const {
        return m_copyQueue;
    }

    rhi::Backend GetBackend() const {
        return m_backend;
    }

    bool IsMultiRHIEnabled() const { return static_cast<bool>(m_peerDevice); }
    rhi::Backend GetPeerBackend() const { return m_peerBackend; }
    rhi::Device GetPeerDevice() const { return m_peerDevice.Get(); }
    rhi::Device GetDevice(rhi::Backend backend) const {
        if (backend == m_backend) return m_device.Get();
        if (backend == m_peerBackend) return m_peerDevice.Get();
        return {};
    }
    rhi::Queue GetPeerQueue(rhi::QueueKind kind) {
        return m_peerDevice ? m_peerDevice->GetQueue(kind) : rhi::Queue{};
    }
    bool GetMeshShadersSupported() const {
        return m_meshShadersSupported;
    }

    const RayTracingFeatureInfo& GetRayTracingFeatures() const {
        return m_rayTracingFeatures;
    }

    bool GetRayTracingSupported() const {
        return m_rayTracingSupported;
    }

    bool GetCLodRayTracingSupported() const {
        return m_clodRayTracingSupported;
    }

private:
    DeviceManager() = default;

    void CheckGPUFeatures();

    rhi::DevicePtr m_device;
    rhi::Queue m_graphicsQueue;
    rhi::Queue m_computeQueue;
    rhi::Queue m_copyQueue;
    rhi::Backend m_backend = rhi::Backend::Null;
    rhi::DevicePtr m_peerDevice;
    rhi::Backend m_peerBackend = rhi::Backend::Null;
    bool m_meshShadersSupported = false;
    RayTracingFeatureInfo m_rayTracingFeatures{};
    bool m_rayTracingSupported = false;
    bool m_clodRayTracingSupported = false;
};
}

using DeviceManager = br::DeviceManager;
