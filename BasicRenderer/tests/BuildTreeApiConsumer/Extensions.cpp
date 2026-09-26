#include <BasicRenderer/Extensions/RenderDeviceAccess.h>

int main() {
    auto device = br::extensions::GetRenderDevice();
    (void)device;
    return 0;
}
