#include "FidelityFX/FfxBackendAdapters.h"

#include <cstdlib>
#include <cstring>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "Resources/PixelBuffer.h"
#include "ThirdParty/FFX/dx12/ffx_api_dx12.hpp"
#include "ThirdParty/FFX/host/backends/dx12/ffx_dx12.h"
#include "rhi_interop_dx12.h"

namespace {
struct Dx12BackendFunctions {
    decltype(&ffxGetScratchMemorySizeDX12) GetScratchMemorySize = nullptr;
    decltype(&ffxGetDeviceDX12) GetDevice = nullptr;
    decltype(&ffxGetInterfaceDX12) GetInterface = nullptr;
    decltype(&ffxGetResourceDX12) GetResource = nullptr;
    decltype(&ffxGetResourceDescriptionDX12) GetResourceDescription = nullptr;
};

HMODULE g_dx12BackendModule = nullptr;
Dx12BackendFunctions g_dx12BackendFunctions{};

template<typename Function>
bool LoadFunction(HMODULE module, Function& target, const char* name) {
    target = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (target == nullptr) {
        spdlog::error("FidelityFX DX12 backend module is missing export {}", name);
        return false;
    }
    return true;
}

bool LoadDx12Backend() {
    if (g_dx12BackendModule != nullptr) {
        return true;
    }

    g_dx12BackendModule = LoadLibraryW(L"ffx_backend_dx12_x64drel.dll");
    if (g_dx12BackendModule == nullptr) {
        spdlog::error("Failed to load FidelityFX DX12 backend module ffx_backend_dx12_x64drel.dll");
        return false;
    }

    if (!LoadFunction(g_dx12BackendModule, g_dx12BackendFunctions.GetScratchMemorySize, "ffxGetScratchMemorySizeDX12") ||
        !LoadFunction(g_dx12BackendModule, g_dx12BackendFunctions.GetDevice, "ffxGetDeviceDX12") ||
        !LoadFunction(g_dx12BackendModule, g_dx12BackendFunctions.GetInterface, "ffxGetInterfaceDX12") ||
        !LoadFunction(g_dx12BackendModule, g_dx12BackendFunctions.GetResource, "ffxGetResourceDX12") ||
        !LoadFunction(g_dx12BackendModule, g_dx12BackendFunctions.GetResourceDescription, "ffxGetResourceDescriptionDX12")) {
        FreeLibrary(g_dx12BackendModule);
        g_dx12BackendModule = nullptr;
        g_dx12BackendFunctions = {};
        return false;
    }

    return true;
}
}

namespace fidelityfx_backend::detail {
bool CreateUpscaleContextDX12(ffx::Context& context, rhi::Device device, ffx::CreateContextDescUpscale& createUpscaling) {
    ffx::CreateBackendDX12Desc backendDesc{};
    backendDesc.device = rhi::dx12::get_device(device);
    return api::CreateContext(context, nullptr, createUpscaling, backendDesc) == ffx::ReturnCode::Ok;
}

FfxApiResource GetApiResourceDX12(org::PixelBuffer* resource, FfxApiResourceState state) {
    auto* nativeResource = rhi::dx12::get_resource(resource->GetAPIResource());
    return ffxApiGetResourceDX12(nativeResource, state);
}

void* GetApiCommandListDX12(rhi::CommandList& commandList) {
    return rhi::dx12::get_cmd_list(commandList);
}

bool CreateHostBackendInterfaceDX12(FfxInterface& backendInterface, void*& scratchMemory, rhi::Device device, size_t maxContexts) {
    if (!LoadDx12Backend()) {
        return false;
    }

    const FfxDevice ffxDevice = g_dx12BackendFunctions.GetDevice(rhi::dx12::get_device(device));
    const size_t scratchMemorySize = g_dx12BackendFunctions.GetScratchMemorySize(maxContexts);

    scratchMemory = std::malloc(scratchMemorySize);
    if (scratchMemory == nullptr) {
        return false;
    }

    std::memset(scratchMemory, 0, scratchMemorySize);
    if (g_dx12BackendFunctions.GetInterface(&backendInterface, ffxDevice, scratchMemory, scratchMemorySize, maxContexts) != FFX_OK) {
        std::free(scratchMemory);
        scratchMemory = nullptr;
        return false;
    }

    return true;
}

FfxResource GetHostResourceDX12(org::PixelBuffer* resource, const wchar_t* name, FfxResourceStates state) {
    if (!LoadDx12Backend()) {
        return {};
    }

    auto* nativeResource = rhi::dx12::get_resource(resource->GetAPIResource());
    const FfxResourceDescription desc = g_dx12BackendFunctions.GetResourceDescription(nativeResource, FFX_RESOURCE_USAGE_READ_ONLY);
    return g_dx12BackendFunctions.GetResource(nativeResource, desc, name, state);
}

void* GetHostCommandListDX12(rhi::CommandList& commandList) {
    return rhi::dx12::get_cmd_list(commandList);
}
}
