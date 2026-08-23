#pragma once

#include <wrl.h>
#include <vector>

#include <rhi.h>

#include "spdlog/spdlog.h"
#include "Utilities/Utilities.h"
#include "Managers/Singletons/DeviceManager.h"

using namespace Microsoft::WRL;

namespace org { class BufferView; }
using org::BufferView;
class SortedUnsignedIntBuffer;
namespace org { class Buffer; }
using org::Buffer;

class ResourceManager {
public:

    static ResourceManager& GetInstance() {
        static ResourceManager instance;
        return instance;
    }

    void Initialize();
    void Cleanup();

    void UpdatePerFrameBuffer(UINT cameraIndex, UINT numLights, DirectX::XMUINT2 screenRes, DirectX::XMUINT3 clusterSizes, unsigned int frameIndex);
    
    std::shared_ptr<Buffer>& GetPerFrameBuffer() {
		return m_perFrameBuffer;
    }

	void SetDirectionalCascadeSplits(const std::vector<float>& splits) {
		switch (perFrameCBData.numDirectionalClipmaps) {
        case 1:
            perFrameCBData.shadowCascadeSplits = DirectX::XMVectorSet(splits[0], 0, 0, 0);
            break;
        case 2:
            perFrameCBData.shadowCascadeSplits = DirectX::XMVectorSet(splits[0], splits[1], 0, 0);
            break;
        case 3:
            perFrameCBData.shadowCascadeSplits = DirectX::XMVectorSet(splits[0], splits[1], splits[2], 0);
            break;
        case 4:
            perFrameCBData.shadowCascadeSplits = DirectX::XMVectorSet(splits[0], splits[1], splits[2], splits[3]);
        }
	}

	void SetActiveEnvironmentIndex(unsigned int index) { perFrameCBData.activeEnvironmentIndex = index; }
	void SetOutputType(unsigned int type) { perFrameCBData.outputType = type; }

	rhi::Resource GetUAVCounterReset() { return m_uavCounterReset.Get(); }
    
private:
    ResourceManager(){};

    std::shared_ptr<Buffer> m_perFrameBuffer;
    UINT8* pPerFrameConstantBuffer;
    PerFrameCB perFrameCBData;
    UINT currentFrameIndex;

    rhi::ResourcePtr m_uavCounterReset;

	int defaultShadowSamplerIndex = -1;

};