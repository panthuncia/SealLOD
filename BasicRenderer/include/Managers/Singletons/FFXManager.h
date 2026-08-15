#pragma once

#pragma once

#include <dxgi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <vector>
#include <DirectXMath.h>
#include <functional>
#include <rhi.h>

#include <bit> // FFX headers need this, not sure why it's not included by default

#include "ThirdParty/FFX/host/ffx_sssr.h"

#include "Scene/Components.h"


namespace org { class PixelBuffer; }
using org::PixelBuffer;
struct RenderContext;
namespace org { class Buffer; }
using org::Buffer;

class FFXManager {
public:
    static FFXManager& GetInstance();
    void EvaluateSSSR(rhi::CommandList& commandList, 
        const Components::Camera* currentCamera,
        PixelBuffer* pHDRTarget, 
        PixelBuffer* pDepthTexture, 
        PixelBuffer* pNormals, 
        PixelBuffer* pMetallicRoughness, 
        PixelBuffer* pMotionVectors, 
        PixelBuffer* pEnvironmentCubemap, 
        PixelBuffer* pBRDFLUT, 
        PixelBuffer* pReflectionsTarget);
    void Shutdown();

    bool InitFFX();

private:
    FFXManager() = default;
    uint8_t m_numFramesInFlight = 0;
    std::function<DirectX::XMUINT2()> m_getRenderRes;
    std::function<DirectX::XMUINT2()> m_getOutputRes;
    FfxInterface m_backendInterface{};
	FfxSssrContext m_sssrContext{};
	void* m_pScratchMemory = nullptr;
	bool m_sssrContextCreated = false;
};

inline FFXManager& FFXManager::GetInstance() {
    static FFXManager instance;
    return instance;
}