#include "PostProcessing/FidelityFX/Backend/FfxBackendAdapters.h"

#include <windows.h>

#include "ThirdParty/FFX/ffx_api_loader.h"

#include <spdlog/spdlog.h>

namespace fidelityfx_backend::detail {
bool CreateUpscaleContextDX12(ffx::Context& context, rhi::Device device, ffx::CreateContextDescUpscale& createUpscaling);
bool CreateUpscaleContextVulkan(ffx::Context& context, rhi::Device device, ffx::CreateContextDescUpscale& createUpscaling);
FfxApiResource GetApiResourceDX12(org::PixelBuffer* resource, FfxApiResourceState state);
FfxApiResource GetApiResourceVulkan(org::PixelBuffer* resource, FfxApiResourceState state);
void* GetApiCommandListDX12(rhi::CommandList& commandList);
void* GetApiCommandListVulkan(rhi::CommandList& commandList);

bool CreateHostBackendInterfaceDX12(FfxInterface& backendInterface, void*& scratchMemory, rhi::Device device, size_t maxContexts);
bool CreateHostBackendInterfaceVulkan(FfxInterface& backendInterface, void*& scratchMemory, rhi::Device device, size_t maxContexts);
FfxResource GetHostResourceDX12(org::PixelBuffer* resource, const wchar_t* name, FfxResourceStates state);
FfxResource GetHostResourceVulkan(org::PixelBuffer* resource, const wchar_t* name, FfxResourceStates state);
void* GetHostCommandListDX12(rhi::CommandList& commandList);
void* GetHostCommandListVulkan(rhi::CommandList& commandList);
}

namespace {
ffxFunctions g_apiFunctions{};
HMODULE g_apiModule = nullptr;
rhi::Backend g_apiBackend = rhi::Backend::Null;

const char* BackendName(rhi::Backend backend) {
    switch (backend) {
    case rhi::Backend::D3D12:
        return "D3D12";
    case rhi::Backend::Vulkan:
        return "Vulkan";
    default:
        return "Unknown";
    }
}

const wchar_t* ApiModuleName(rhi::Backend backend) {
    switch (backend) {
    case rhi::Backend::D3D12:
        return L"amd_fidelityfx_dx12.dll";
    case rhi::Backend::Vulkan:
        return L"amd_fidelityfx_vk.dll";
    default:
        return nullptr;
    }
}

bool HasRequiredApiFunctions() {
    return g_apiFunctions.CreateContext != nullptr &&
        g_apiFunctions.DestroyContext != nullptr &&
        g_apiFunctions.Configure != nullptr &&
        g_apiFunctions.Query != nullptr &&
        g_apiFunctions.Dispatch != nullptr;
}
}

bool fidelityfx_backend::api::LoadModule(rhi::Backend backend) {
    if (g_apiModule != nullptr && g_apiBackend == backend) {
        return true;
    }

    if (g_apiModule != nullptr) {
        FreeLibrary(g_apiModule);
        g_apiModule = nullptr;
        g_apiFunctions = {};
        g_apiBackend = rhi::Backend::Null;
    }

    const wchar_t* moduleName = ApiModuleName(backend);
    if (moduleName == nullptr) {
        spdlog::warn("FidelityFX API does not support backend {}", static_cast<uint32_t>(backend));
        return false;
    }

    g_apiModule = LoadLibraryW(moduleName);
    if (g_apiModule == nullptr) {
        spdlog::error("Failed to load FidelityFX API module for backend {}", BackendName(backend));
        return false;
    }

    ffxLoadFunctions(&g_apiFunctions, g_apiModule);
    if (!HasRequiredApiFunctions()) {
        spdlog::error("FidelityFX API module for backend {} is missing required exports", BackendName(backend));
        FreeLibrary(g_apiModule);
        g_apiModule = nullptr;
        g_apiFunctions = {};
        return false;
    }

    g_apiBackend = backend;
    return true;
}

void fidelityfx_backend::api::UnloadModule() {
    if (g_apiModule != nullptr) {
        FreeLibrary(g_apiModule);
        g_apiModule = nullptr;
        g_apiFunctions = {};
        g_apiBackend = rhi::Backend::Null;
    }
}

ffx::ReturnCode fidelityfx_backend::api::CreateContext(ffx::Context& context, ffxAllocationCallbacks* memCb, ffxCreateContextDescHeader* header) {
    if (g_apiFunctions.CreateContext == nullptr || header == nullptr) {
        return ffx::ReturnCode::ErrorRuntimeError;
    }
    return static_cast<ffx::ReturnCode>(g_apiFunctions.CreateContext(&context, header, memCb));
}

ffx::ReturnCode fidelityfx_backend::api::DestroyContext(ffx::Context& context, const ffxAllocationCallbacks* memCb) {
    if (g_apiFunctions.DestroyContext == nullptr || context == nullptr) {
        return ffx::ReturnCode::ErrorRuntimeError;
    }
    return static_cast<ffx::ReturnCode>(g_apiFunctions.DestroyContext(&context, memCb));
}

ffx::ReturnCode fidelityfx_backend::api::Query(ffx::Context& context, ffxQueryDescHeader* header) {
    if (g_apiFunctions.Query == nullptr || context == nullptr || header == nullptr) {
        return ffx::ReturnCode::ErrorRuntimeError;
    }
    return static_cast<ffx::ReturnCode>(g_apiFunctions.Query(&context, header));
}

ffx::ReturnCode fidelityfx_backend::api::Dispatch(ffx::Context& context, ffxDispatchDescHeader* header) {
    if (g_apiFunctions.Dispatch == nullptr || context == nullptr || header == nullptr) {
        return ffx::ReturnCode::ErrorRuntimeError;
    }
    return static_cast<ffx::ReturnCode>(g_apiFunctions.Dispatch(&context, header));
}

bool fidelityfx_backend::api::CreateUpscaleContext(ffx::Context& context, rhi::Backend backend, rhi::Device device, ffx::CreateContextDescUpscale& createUpscaling) {
    if (!LoadModule(backend)) {
        return false;
    }

    switch (backend) {
    case rhi::Backend::D3D12:
        return detail::CreateUpscaleContextDX12(context, device, createUpscaling);
    case rhi::Backend::Vulkan:
        return detail::CreateUpscaleContextVulkan(context, device, createUpscaling);
    default:
        spdlog::warn("FidelityFX upscale does not support backend {}", static_cast<uint32_t>(backend));
        return false;
    }
}

FfxApiResource fidelityfx_backend::api::GetResource(rhi::Backend backend, org::PixelBuffer* resource, const wchar_t* name, FfxApiResourceState state) {
    switch (backend) {
    case rhi::Backend::D3D12:
        return detail::GetApiResourceDX12(resource, state);
    case rhi::Backend::Vulkan:
        return detail::GetApiResourceVulkan(resource, state);
    default:
        spdlog::error("Failed to get FFX API resource '{}' for unsupported backend {}", name ? "<named>" : "<unnamed>", BackendName(backend));
        return {};
    }
}

void* fidelityfx_backend::api::GetCommandList(rhi::Backend backend, rhi::CommandList& commandList) {
    switch (backend) {
    case rhi::Backend::D3D12:
        return detail::GetApiCommandListDX12(commandList);
    case rhi::Backend::Vulkan:
        return detail::GetApiCommandListVulkan(commandList);
    default:
        spdlog::error("Failed to get FFX API command list for unsupported backend {}", BackendName(backend));
        return nullptr;
    }
}

bool fidelityfx_backend::host::CreateBackendInterface(FfxInterface& backendInterface, void*& scratchMemory, rhi::Backend backend, rhi::Device device, size_t maxContexts) {
    switch (backend) {
    case rhi::Backend::D3D12:
        return detail::CreateHostBackendInterfaceDX12(backendInterface, scratchMemory, device, maxContexts);
    case rhi::Backend::Vulkan:
        return detail::CreateHostBackendInterfaceVulkan(backendInterface, scratchMemory, device, maxContexts);
    default:
        spdlog::warn("FidelityFX host backend does not support backend {}", static_cast<uint32_t>(backend));
        return false;
    }
}

FfxResource fidelityfx_backend::host::GetResource(rhi::Backend backend, org::PixelBuffer* resource, const wchar_t* name, FfxResourceStates state) {
    switch (backend) {
    case rhi::Backend::D3D12:
        return detail::GetHostResourceDX12(resource, name, state);
    case rhi::Backend::Vulkan:
        return detail::GetHostResourceVulkan(resource, name, state);
    default:
        spdlog::error("Failed to get FFX host resource '{}' for unsupported backend {}", name ? "<named>" : "<unnamed>", BackendName(backend));
        return {};
    }
}

void* fidelityfx_backend::host::GetCommandList(rhi::Backend backend, rhi::CommandList& commandList) {
    switch (backend) {
    case rhi::Backend::D3D12:
        return detail::GetHostCommandListDX12(commandList);
    case rhi::Backend::Vulkan:
        return detail::GetHostCommandListVulkan(commandList);
    default:
        spdlog::error("Failed to get FFX host command list for unsupported backend {}", BackendName(backend));
        return nullptr;
    }
}
