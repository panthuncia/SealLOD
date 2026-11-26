#pragma once

#include <dxgi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <atomic>
#include <memory>
#include <vector>
#include <DirectXMath.h>
#include <functional>
#include <mutex>
#include <rhi.h>

#include <bit> // FFX headers need this, not sure why it's not included by default

#include "ThirdParty/FFX/host/ffx_sssr.h"

#include "BasicRenderer/Scene/Components.h"


namespace org { class PixelBuffer; }
struct RenderContext;
namespace org { class Buffer; }

class FFXManager {
public:
    // An FFX SSSR context for one render resolution (M5 step 3). The graphs that use SSSR hold it through their pass's service, so
    // it lives as long as they do, and live graphs of the same render resolution share it (and its history). It is made by the
    // build that first needs it, on the building thread; the last graph's retirement releases it (CollectRetired).
    class SssrContext {
    public:
        ~SssrContext();
        SssrContext(const SssrContext&) = delete;
        SssrContext& operator=(const SssrContext&) = delete;
        DirectX::XMUINT2 RenderSize() const noexcept { return m_renderSize; }

    private:
        friend class FFXManager;
        SssrContext() = default;
        // FFX keeps pointers into its context: it stays where it was made until it is destroyed.
        struct Native {
            FfxInterface backendInterface{};
            FfxSssrContext context{};
            void* scratchMemory = nullptr;
        };
        DirectX::XMUINT2 m_renderSize{};
        std::unique_ptr<Native> m_native;
    };

    static FFXManager& GetInstance();
    // A live context for the render resolution, or a new one. Any thread; null if FFX could not make it.
    std::shared_ptr<SssrContext> AcquireSssrContext(DirectX::XMUINT2 renderSize);
    void EvaluateSSSR(SssrContext& sssr,
        rhi::CommandList& commandList,
        const Components::Camera* currentCamera,
        org::PixelBuffer* pHDRTarget,
        org::PixelBuffer* pDepthTexture,
        org::PixelBuffer* pNormals,
        org::PixelBuffer* pMetallicRoughness,
        org::PixelBuffer* pMotionVectors,
        org::PixelBuffer* pEnvironmentCubemap,
        org::PixelBuffer* pBRDFLUT,
        org::PixelBuffer* pReflectionsTarget);
    // Destroys the contexts whose last graph retired. Render thread, once a frame.
    void CollectRetired();
    void Shutdown();

private:
    FFXManager() = default;
    void Retire(std::unique_ptr<SssrContext::Native> native);
    static void Destroy(SssrContext::Native& native);
    std::mutex m_contextsMutex;
    std::vector<std::weak_ptr<SssrContext>> m_contexts;
    std::vector<std::unique_ptr<SssrContext::Native>> m_retired;
    std::atomic<bool> m_shutDown = false;
    std::mutex m_evaluateMutex;
};

inline FFXManager& FFXManager::GetInstance() {
    static FFXManager instance;
    return instance;
}
