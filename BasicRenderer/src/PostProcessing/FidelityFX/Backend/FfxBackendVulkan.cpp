#include "PostProcessing/FidelityFX/Backend/FfxBackendAdapters.h"

#include <cstdlib>
#include <cstring>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "Resources/PixelBuffer.h"
#include "rhi_interop_vulkan.h"

#if BASICRHI_HAS_VULKAN_HEADERS
#include "ThirdParty/FFX/host/backends/vk/ffx_vk.h"
#include "ThirdParty/FFX/vk/ffx_api_vk.hpp"

namespace {
struct VulkanBackendFunctions {
    decltype(&ffxGetScratchMemorySizeVK) GetScratchMemorySize = nullptr;
    decltype(&ffxGetDeviceVK) GetDevice = nullptr;
    decltype(&ffxGetInterfaceVK) GetInterface = nullptr;
    decltype(&ffxGetResourceVK) GetResource = nullptr;
    decltype(&ffxGetImageResourceDescriptionVK) GetImageResourceDescription = nullptr;
};

HMODULE g_vulkanBackendModule = nullptr;
VulkanBackendFunctions g_vulkanBackendFunctions{};

template<typename Function>
bool LoadFunction(HMODULE module, Function& target, const char* name) {
    target = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (target == nullptr) {
        spdlog::error("FidelityFX Vulkan backend module is missing export {}", name);
        return false;
    }
    return true;
}

bool LoadVulkanBackend() {
    if (g_vulkanBackendModule != nullptr) {
        return true;
    }

    g_vulkanBackendModule = LoadLibraryW(L"ffx_backend_vk_x64drel.dll");
    if (g_vulkanBackendModule == nullptr) {
        spdlog::error("Failed to load FidelityFX Vulkan backend module ffx_backend_vk_x64drel.dll");
        return false;
    }

    if (!LoadFunction(g_vulkanBackendModule, g_vulkanBackendFunctions.GetScratchMemorySize, "ffxGetScratchMemorySizeVK") ||
        !LoadFunction(g_vulkanBackendModule, g_vulkanBackendFunctions.GetDevice, "ffxGetDeviceVK") ||
        !LoadFunction(g_vulkanBackendModule, g_vulkanBackendFunctions.GetInterface, "ffxGetInterfaceVK") ||
        !LoadFunction(g_vulkanBackendModule, g_vulkanBackendFunctions.GetResource, "ffxGetResourceVK") ||
        !LoadFunction(g_vulkanBackendModule, g_vulkanBackendFunctions.GetImageResourceDescription, "ffxGetImageResourceDescriptionVK")) {
        FreeLibrary(g_vulkanBackendModule);
        g_vulkanBackendModule = nullptr;
        g_vulkanBackendFunctions = {};
        return false;
    }

    return true;
}

VkFormat ToVkFormat(rhi::Format format) {
    switch (format) {
    case rhi::Format::R32G32B32A32_Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case rhi::Format::R16G16B16A16_Float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case rhi::Format::R11G11B10_Float:
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case rhi::Format::R16G16_Float:
        return VK_FORMAT_R16G16_SFLOAT;
    case rhi::Format::R8G8B8A8_UNorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case rhi::Format::R8G8B8A8_UNorm_sRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case rhi::Format::B8G8R8A8_UNorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case rhi::Format::B8G8R8A8_UNorm_sRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case rhi::Format::R32_Float:
        return VK_FORMAT_R32_SFLOAT;
    case rhi::Format::D32_Float:
        return VK_FORMAT_D32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

uint32_t ComputeMipCount(const org::TextureDescription& description) {
    const uint32_t layers = (description.isCubemap ? 6u : 1u) * (std::max)(1u, description.arraySize);
    if (layers == 0u || description.imageDimensions.empty()) {
        return 1u;
    }

    const size_t mipCount = description.imageDimensions.size() / layers;
    return static_cast<uint32_t>((std::max)(size_t(1), mipCount));
}

VkImageUsageFlags BuildVkImageUsage(const org::TextureDescription& description) {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (description.hasUAV || description.hasNonShaderVisibleUAV) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (description.hasRTV) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (description.hasDSV) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    return usage;
}

VkImageCreateInfo BuildVkImageCreateInfo(const org::TextureDescription& description) {
    VkImageCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.format = ToVkFormat(description.format);
    createInfo.extent.width = description.imageDimensions.empty() ? 1u : description.imageDimensions.front().width;
    createInfo.extent.height = description.imageDimensions.empty() ? 1u : description.imageDimensions.front().height;
    createInfo.extent.depth = 1u;
    createInfo.mipLevels = ComputeMipCount(description);
    createInfo.arrayLayers = (description.isCubemap ? 6u : 1u) * (std::max)(1u, description.arraySize);
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.usage = BuildVkImageUsage(description);
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (description.isCubemap) {
        createInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    return createInfo;
}

VkImageCreateInfo BuildVkImageCreateInfo(rhi::Resource resource, const org::TextureDescription& description) {
    rhi::VulkanResourceInfo resourceInfo{};
    if (!rhi::vulkan::get_resource_info(resource, resourceInfo) || resourceInfo.resource == nullptr) {
        return BuildVkImageCreateInfo(description);
    }

    VkImageCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    createInfo.flags = static_cast<VkImageCreateFlags>(resourceInfo.flags);
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.format = static_cast<VkFormat>(resourceInfo.nativeFormat);
    createInfo.extent.width = resourceInfo.width;
    createInfo.extent.height = resourceInfo.height;
    createInfo.extent.depth = 1u;
    createInfo.mipLevels = (std::max)(1u, resourceInfo.mipLevels);
    createInfo.arrayLayers = (std::max)(1u, resourceInfo.arrayLayers);
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.usage = static_cast<VkImageUsageFlags>(resourceInfo.usage);
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (createInfo.format == VK_FORMAT_UNDEFINED || createInfo.usage == 0u || createInfo.extent.width == 0u || createInfo.extent.height == 0u) {
        return BuildVkImageCreateInfo(description);
    }
    return createInfo;
}
}

namespace fidelityfx_backend::detail {
bool CreateUpscaleContextVulkan(ffx::Context& context, rhi::Device device, ffx::CreateContextDescUpscale& createUpscaling) {
    ffx::CreateBackendVKDesc backendDesc{};
    backendDesc.vkDevice = rhi::vulkan::get_device(device);
    backendDesc.vkPhysicalDevice = rhi::vulkan::get_physical_device(device);

    if (backendDesc.vkDevice == VK_NULL_HANDLE || backendDesc.vkPhysicalDevice == VK_NULL_HANDLE) {
        spdlog::error("CreateUpscaleContextVulkan failed to query Vulkan device handles (device={}, physicalDevice={})",
            fmt::ptr(backendDesc.vkDevice),
            fmt::ptr(backendDesc.vkPhysicalDevice));
        return false;
    }

    backendDesc.vkDeviceProcAddr = rhi::vulkan::get_device_proc_addr();
    if (backendDesc.vkDeviceProcAddr == nullptr) {
        spdlog::error("CreateUpscaleContextVulkan failed because Vulkan device proc loader is not initialized");
        return false;
    }
    return api::CreateContext(context, nullptr, createUpscaling, backendDesc) == ffx::ReturnCode::Ok;
}

FfxApiResource GetApiResourceVulkan(org::PixelBuffer* resource, FfxApiResourceState state) {
    const rhi::Resource apiResource = resource->GetAPIResource();
    const VkImage nativeImage = rhi::vulkan::get_resource(apiResource);
    const VkImageCreateInfo createInfo = BuildVkImageCreateInfo(apiResource, resource->GetDescription());
    const FfxApiResourceDescription desc = ffxApiGetImageResourceDescriptionVK(nativeImage, createInfo, 0u);
    return ffxApiGetResourceVK(reinterpret_cast<void*>(nativeImage), desc, state);
}

void* GetApiCommandListVulkan(rhi::CommandList& commandList) {
    return reinterpret_cast<void*>(rhi::vulkan::get_cmd_list(commandList));
}

bool CreateHostBackendInterfaceVulkan(FfxInterface& backendInterface, void*& scratchMemory, rhi::Device device, size_t maxContexts) {
    if (!LoadVulkanBackend()) {
        return false;
    }

    VkDeviceContext deviceContext{};
    deviceContext.vkDevice = rhi::vulkan::get_device(device);
    deviceContext.vkPhysicalDevice = rhi::vulkan::get_physical_device(device);

    if (deviceContext.vkDevice == VK_NULL_HANDLE || deviceContext.vkPhysicalDevice == VK_NULL_HANDLE) {
        spdlog::error("CreateHostBackendInterfaceVulkan failed to query Vulkan device handles (device={}, physicalDevice={})",
            fmt::ptr(deviceContext.vkDevice),
            fmt::ptr(deviceContext.vkPhysicalDevice));
        return false;
    }

    deviceContext.vkDeviceProcAddr = rhi::vulkan::get_device_proc_addr();
    if (deviceContext.vkDeviceProcAddr == nullptr) {
        spdlog::error("CreateHostBackendInterfaceVulkan failed because Vulkan device proc loader is not initialized");
        return false;
    }

    const FfxDevice ffxDevice = g_vulkanBackendFunctions.GetDevice(&deviceContext);
    const size_t scratchMemorySize = g_vulkanBackendFunctions.GetScratchMemorySize(deviceContext.vkPhysicalDevice, maxContexts);

    scratchMemory = std::malloc(scratchMemorySize);
    if (scratchMemory == nullptr) {
        return false;
    }

    std::memset(scratchMemory, 0, scratchMemorySize);
    if (g_vulkanBackendFunctions.GetInterface(&backendInterface, ffxDevice, scratchMemory, scratchMemorySize, maxContexts) != FFX_OK) {
        std::free(scratchMemory);
        scratchMemory = nullptr;
        return false;
    }

    return true;
}

FfxResource GetHostResourceVulkan(org::PixelBuffer* resource, const wchar_t* name, FfxResourceStates state) {
    if (!LoadVulkanBackend()) {
        return {};
    }

    const rhi::Resource apiResource = resource->GetAPIResource();
    const VkImage nativeImage = rhi::vulkan::get_resource(apiResource);
    const VkImageCreateInfo createInfo = BuildVkImageCreateInfo(apiResource, resource->GetDescription());
    const FfxResourceDescription desc = g_vulkanBackendFunctions.GetImageResourceDescription(nativeImage, createInfo, FFX_RESOURCE_USAGE_READ_ONLY);
    return g_vulkanBackendFunctions.GetResource(reinterpret_cast<void*>(nativeImage), desc, name, state);
}

void* GetHostCommandListVulkan(rhi::CommandList& commandList) {
    return reinterpret_cast<void*>(rhi::vulkan::get_cmd_list(commandList));
}
}
#else
namespace fidelityfx_backend::detail {
bool CreateUpscaleContextVulkan(ffx::Context&, rhi::Device, ffx::CreateContextDescUpscale&) {
    spdlog::warn("FidelityFX Vulkan upscaling is unavailable because Vulkan headers are not available in this build.");
    return false;
}

FfxApiResource GetApiResourceVulkan(org::PixelBuffer*, FfxApiResourceState) {
    spdlog::warn("FidelityFX Vulkan upscaling resource access is unavailable because Vulkan headers are not available in this build.");
    return {};
}

void* GetApiCommandListVulkan(rhi::CommandList&) {
    spdlog::warn("FidelityFX Vulkan upscaling command list access is unavailable because Vulkan headers are not available in this build.");
    return nullptr;
}

bool CreateHostBackendInterfaceVulkan(FfxInterface&, void*&, rhi::Device, size_t) {
    spdlog::warn("FidelityFX Vulkan host backend is unavailable because Vulkan headers are not available in this build.");
    return false;
}

FfxResource GetHostResourceVulkan(org::PixelBuffer*, const wchar_t*, FfxResourceStates) {
    spdlog::warn("FidelityFX Vulkan host resource access is unavailable because Vulkan headers are not available in this build.");
    return {};
}

void* GetHostCommandListVulkan(rhi::CommandList&) {
    spdlog::warn("FidelityFX Vulkan host command list access is unavailable because Vulkan headers are not available in this build.");
    return nullptr;
}
}
#endif
