#include <BasicRenderer/Extensions/RenderDeviceAccess.h>

#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include "Runtime/Device/DeviceManager.h"

namespace br::extensions {

rhi::Device GetRenderDevice()
{
    return DeviceManager::GetInstance().GetDevice();
}

rhi::CommandSignatureHandle GetRawDispatchSignature()
{
    return CommandSignatureManager::GetInstance().GetRawDispatchCommandSignature().GetHandle();
}

rhi::CommandSignatureHandle GetDispatchMeshSignature()
{
    return CommandSignatureManager::GetInstance().GetDispatchMeshCommandSignature().GetHandle();
}

} // namespace br::extensions
