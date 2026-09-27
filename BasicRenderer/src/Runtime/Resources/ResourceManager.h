#pragma once

#include <wrl.h>
#include <memory>
#include <vector>

#include <rhi.h>

#include "spdlog/spdlog.h"
#include "Utilities/Utilities.h"
#include "Runtime/Device/DeviceManager.h"

using namespace Microsoft::WRL;

namespace org { class BufferView; }
class SortedUnsignedIntBuffer;
namespace org { class Buffer; }
namespace org::runtime { class IUploadService; }

class ResourceManager {
public:

    static ResourceManager& GetInstance() {
        static ResourceManager instance;
        return instance;
    }

    void Initialize(std::shared_ptr<org::runtime::IUploadService> uploadService);
    void SetUploadService(std::shared_ptr<org::runtime::IUploadService> uploadService);
    void Cleanup();

    void UpdatePerFrameBuffer(UINT cameraIndex, UINT numLights, DirectX::XMUINT2 screenRes, DirectX::XMUINT3 clusterSizes, unsigned int frameIndex);
    
    std::shared_ptr<org::Buffer>& GetPerFrameBuffer() {
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

    std::shared_ptr<org::Buffer> m_perFrameBuffer;
    UINT8* pPerFrameConstantBuffer;
    PerFrameCB perFrameCBData;
    UINT currentFrameIndex;

    rhi::ResourcePtr m_uavCounterReset;
    std::shared_ptr<org::runtime::IUploadService> m_uploadService;

	int defaultShadowSamplerIndex = -1;

};
