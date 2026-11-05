#include <BasicRenderer/Extensions/RenderDeviceAccess.h>

#include "Runtime/Device/DeviceManager.h"

namespace br::extensions {

rhi::Device GetRenderDevice()
{
    return DeviceManager::GetInstance().GetDevice();
}

} // namespace br::extensions
