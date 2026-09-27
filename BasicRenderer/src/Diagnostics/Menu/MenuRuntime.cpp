#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"

Menu& Menu::GetInstance() {
    static Menu instance;
    return instance;
}

bool Menu::HandleInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return static_cast<bool>(ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam));
}

static inline const char* MajorCategory(rhi::ResourceType t) {
    using RT = rhi::ResourceType;
    switch (t) {
    case RT::Buffer:                return "Buffers";
    case RT::Texture1D:             return "Textures";
    case RT::Texture2D:             return "Textures";
    case RT::Texture3D:             return "Textures";
    case RT::AccelerationStructure: return "AccelStructs";
    default:                        return "Other";
    }
}

struct PerResourceMemInfo {
    uint64_t bytes = 0;
    std::string category; // "Textures/Material", etc.
    std::string name;     // optional
};

using PerResourceMemIndex = std::unordered_map<uint64_t, PerResourceMemInfo>;

static void BuildMemorySnapshotFromRecords(
    org::ui::MemorySnapshot& out,
    const std::vector<org::memory::ResourceMemoryRecord>& records,
    PerResourceMemIndex* outIndex /*= nullptr*/)
{
    out.categories.clear();
    out.resources.clear();
    out.totalBytes = 0;

    std::unordered_map<std::string, uint64_t> minorBuckets;
    minorBuckets.reserve(256);

    if (outIndex) { outIndex->clear(); outIndex->reserve(2048); }

    for (const auto& record : records) {
        const uint64_t bytes = record.bytes;
        out.totalBytes += bytes;

        const char* major = MajorCategory(record.resourceType);

        const char* usage = !record.usage.empty() ? record.usage.c_str()
            : "Unspecified";

        const std::string cat = std::string(major) + "/" + usage;

        minorBuckets[cat] += bytes;

        org::ui::MemoryResourceRow row{};
        row.bytes = bytes;
        row.uid = record.resourceID;

        if (!record.resourceName.empty()) row.name = record.resourceName;
        else if (record.resourceID != 0) row.name = "Resource " + std::to_string((unsigned long long)record.resourceID);
        else row.name = "Unknown resource";

        row.type = cat;
        out.resources.push_back(row);

        if (outIndex && record.resourceID != 0) {
            auto& info = (*outIndex)[record.resourceID];
            info.bytes = bytes;
            info.category = cat;
            if (!record.resourceName.empty()) info.name = record.resourceName;
        }
    }

    out.categories.reserve(minorBuckets.size());
    for (auto& [label, bytes] : minorBuckets) {
        if (bytes) out.categories.push_back({ label, bytes });
    }

    std::sort(out.categories.begin(), out.categories.end(),
        [](auto const& a, auto const& b) { return a.bytes > b.bytes; });
}


void Menu::Initialize(HWND hwnd, rhi::Swapchain swapChain) {
	auto numFramesInFlight = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numFramesInFlight")();

    environmentsDir = std::filesystem::path(GetExePath()) / "textures" / "environment";

	auto device = DeviceManager::GetInstance().GetDevice();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    m_imguiWin32Initialized = true;

    if (DeviceManager::GetInstance().GetBackend() == rhi::Backend::D3D12) {
		auto result = device.CreateDescriptorHeap({ rhi::DescriptorHeapType::CbvSrvUav, kImGuiHeapCapacity, true }, g_pd3dSrvDescHeap);
		if (!rhi::IsOk(result) || !g_pd3dSrvDescHeap) {
			throw std::runtime_error("Menu::Initialize failed to create ImGui descriptor heap for DX12 backend");
		}

        // Cache GPU start and increment size for user-texture descriptor allocation.
        auto* nativeDescriptorHeap = rhi::dx12::get_descriptor_heap(g_pd3dSrvDescHeap.Get());
        imguiHeapGpuStart_ = nativeDescriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr;
        imguiHeapIncrementSize_ = device.GetDescriptorHandleIncrementSize(rhi::DescriptorHeapType::CbvSrvUav);

        ImGui_ImplDX12_InitInfo initInfo{};
        initInfo.Device = rhi::dx12::get_device(device);
        initInfo.CommandQueue = rhi::dx12::get_queue(DeviceManager::GetInstance().GetGraphicsQueue());
        initInfo.NumFramesInFlight = numFramesInFlight;
        initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.UserData = this;
        initInfo.SrvDescriptorHeap = nativeDescriptorHeap;
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
            D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
            D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle) {
            auto* menu = static_cast<Menu*>(info->UserData);
            const uint32_t descriptorIndex = menu->AllocateImGuiDescriptor();
            const uint64_t descriptorOffset =
                static_cast<uint64_t>(descriptorIndex) * menu->imguiHeapIncrementSize_;
            *cpuHandle = info->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
            *gpuHandle = info->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
            cpuHandle->ptr += static_cast<SIZE_T>(descriptorOffset);
            gpuHandle->ptr += descriptorOffset;
        };
        initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info,
            D3D12_CPU_DESCRIPTOR_HANDLE,
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
            auto* menu = static_cast<Menu*>(info->UserData);
            if (gpuHandle.ptr < menu->imguiHeapGpuStart_ || menu->imguiHeapIncrementSize_ == 0) {
                return;
            }
            const uint64_t descriptorOffset = gpuHandle.ptr - menu->imguiHeapGpuStart_;
            if (descriptorOffset % menu->imguiHeapIncrementSize_ != 0) {
                return;
            }
            menu->FreeImGuiDescriptor(static_cast<uint32_t>(
                descriptorOffset / menu->imguiHeapIncrementSize_));
        };

        if (!ImGui_ImplDX12_Init(&initInfo)) {
            throw std::runtime_error("Menu::Initialize failed to initialize ImGui DX12 backend");
        }
        m_imguiBackend = rhi::Backend::D3D12;
    } else if (DeviceManager::GetInstance().GetBackend() == rhi::Backend::Vulkan) {
#if BASICRENDERER_HAS_IMGUI_VULKAN
		// The Vulkan ImGui backend currently faults in its loader/initialization
		// path on some drivers. Keep UI optional so Vulkan rendering and interop
		// can start independently; the renderer already treats a null UI backend
		// as a supported headless state.
		if (std::getenv("BASICRENDERER_ENABLE_VULKAN_IMGUI") == nullptr) {
			spdlog::warn("Menu::Initialize: Vulkan ImGui backend disabled; set BASICRENDERER_ENABLE_VULKAN_IMGUI=1 to opt in");
		}
		else {
        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = rhi::vulkan::get_device_api_version(device);
        initInfo.Instance = rhi::vulkan::get_instance(device);
        initInfo.PhysicalDevice = rhi::vulkan::get_physical_device(device);
        initInfo.Device = rhi::vulkan::get_device(device);
        initInfo.QueueFamily = rhi::vulkan::get_queue_family_index(DeviceManager::GetInstance().GetGraphicsQueue());
        initInfo.Queue = rhi::vulkan::get_queue(DeviceManager::GetInstance().GetGraphicsQueue());
        initInfo.MinImageCount = numFramesInFlight;
        initInfo.ImageCount = numFramesInFlight;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.DescriptorPoolSize = kImGuiHeapCapacity;
        initInfo.UseDynamicRendering = true;
        m_imguiVkColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        m_imguiVkRenderingInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
        m_imguiVkRenderingInfo.colorAttachmentCount = 1;
        m_imguiVkRenderingInfo.pColorAttachmentFormats = &m_imguiVkColorFormat;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = m_imguiVkRenderingInfo;
        initInfo.CheckVkResultFn = [](VkResult result) {
            if (result != VK_SUCCESS) {
                spdlog::error("ImGui Vulkan backend returned VkResult {}", static_cast<int>(result));
            }
        };

        if (!initInfo.Instance || !initInfo.PhysicalDevice || !initInfo.Device || !initInfo.Queue) {
            throw std::runtime_error("Menu::Initialize failed to query Vulkan native handles for ImGui");
        }
        if (!vkGetInstanceProcAddr || !vkGetDeviceProcAddr) {
            throw std::runtime_error("Menu::Initialize cannot initialize ImGui Vulkan backend because Volk has not loaded Vulkan function pointers");
        }

        struct ImGuiVulkanLoaderData {
            VkInstance instance;
            VkDevice device;
        } loaderData{ initInfo.Instance, initInfo.Device };

        if (!ImGui_ImplVulkan_LoadFunctions(initInfo.ApiVersion, [](const char* functionName, void* userData) -> PFN_vkVoidFunction {
            const auto* loaderData = static_cast<const ImGuiVulkanLoaderData*>(userData);
            if (!loaderData) {
                return nullptr;
            }
            if (std::strcmp(functionName, "vkCmdBeginRendering") == 0) {
                return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering);
            }
            if (std::strcmp(functionName, "vkCmdEndRendering") == 0) {
                return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRendering);
            }
            if (std::strcmp(functionName, "vkCmdBeginRenderingKHR") == 0) {
                return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderingKHR);
            }
            if (std::strcmp(functionName, "vkCmdEndRenderingKHR") == 0) {
                return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderingKHR);
            }
            if (vkGetInstanceProcAddr) {
                if (PFN_vkVoidFunction function = vkGetInstanceProcAddr(loaderData->instance, functionName)) {
                    return function;
                }
            }
            return vkGetDeviceProcAddr ? vkGetDeviceProcAddr(loaderData->device, functionName) : nullptr;
        }, &loaderData)) {
            throw std::runtime_error("Menu::Initialize failed to load ImGui Vulkan backend functions through Volk");
        }

        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error("Menu::Initialize failed to initialize ImGui Vulkan backend");
        }

        auto result = device.CreateDescriptorHeap({ rhi::DescriptorHeapType::CbvSrvUav, kImGuiHeapCapacity, true }, g_pd3dSrvDescHeap);
        if (!rhi::IsOk(result) || !g_pd3dSrvDescHeap) {
            throw std::runtime_error("Menu::Initialize failed to create ImGui descriptor heap for Vulkan backend");
        }

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        samplerInfo.maxAnisotropy = 1.0f;
        m_imguiVkDevice = initInfo.Device;
        const VkResult samplerResult = vkCreateSampler(m_imguiVkDevice, &samplerInfo, nullptr, &m_imguiVkPreviewSampler);
        if (samplerResult != VK_SUCCESS) {
            throw std::runtime_error("Menu::Initialize failed to create Vulkan ImGui preview sampler");
        }

        m_imguiBackend = rhi::Backend::Vulkan;
		}
#else
        (void)swapChain;
        spdlog::warn("Menu::Initialize: Vulkan renderer backend was selected, but imgui_impl_vulkan.h is not available in this build environment.");
#endif
    } else {
        (void)swapChain;
        spdlog::warn("Menu::Initialize: Vulkan renderer backend is not available in this workspace's ImGui integration. UI rendering will stay disabled until a Vulkan ImGui backend is added.");
    }

    ImGui_ImplWin32_EnableDpiAwareness();


    IMGUI_CHECKVERSION();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    // Keep keyboard navigation available without having a focused navigation
    // window claim every key from the renderer. Active text fields and modal
    // widgets still set WantCaptureKeyboard independently.
    io.ConfigNavCaptureKeyboard = false;
    io.FontGlobalScale = 1.2f;

    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

	// Helper to set an observer on a setting which updates local copies of settings
    auto observerSetting = [&](auto& localCopy, const std::string& settingName) {
        m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<std::decay_t<decltype(localCopy)>>(settingName,
            [&localCopy](const std::decay_t<decltype(localCopy)>& newValue) {
                localCopy = newValue;
            }));
		};

	getEnvironmentName = SettingsManager::GetInstance().getSettingGetter<std::string>("environmentName");
	setEnvironment = SettingsManager::GetInstance().getSettingSetter<std::string>("environmentName");

    auto& settingsManager = SettingsManager::GetInstance();
    getImageBasedLightingEnabled = settingsManager.getSettingGetter<bool>("enableImageBasedLighting");
    setImageBasedLightingEnabled = settingsManager.getSettingSetter<bool>("enableImageBasedLighting");
	imageBasedLightingEnabled = getImageBasedLightingEnabled();
	observerSetting(imageBasedLightingEnabled, "enableImageBasedLighting");

	getPunctualLightingEnabled = settingsManager.getSettingGetter<bool>("enablePunctualLighting");
	setPunctualLightingEnabled = settingsManager.getSettingSetter<bool>("enablePunctualLighting");
	punctualLightingEnabled = getPunctualLightingEnabled();
	observerSetting(punctualLightingEnabled, "enablePunctualLighting");

	getShadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows");
	setShadowsEnabled = settingsManager.getSettingSetter<bool>("enableShadows");
	shadowsEnabled = getShadowsEnabled();
	observerSetting(shadowsEnabled, "enableShadows");

    hdrFiles = GetFilesInDirectoryMatchingExtension(environmentsDir.wstring(), L".hdr");
	environmentName = getEnvironmentName();
    settingsManager.addObserver<std::string>("environmentName", [this](const std::string& newValue) {
        environmentName = getEnvironmentName();
        });

	setOutputType = settingsManager.getSettingSetter<unsigned int>("outputType");
	setTonemapType = settingsManager.getSettingSetter<unsigned int>("tonemapType");
	getTonemapType = settingsManager.getSettingGetter<unsigned int>("tonemapType");

    getSceneRoot = settingsManager.getSettingGetter<std::function<flecs::entity()>>("getSceneRoot")();
    queueSceneNodePositionEdit = settingsManager.getSettingGetter<std::function<void(uint64_t, DirectX::XMFLOAT3)>>("queueSceneNodePositionEdit")();
    queueSceneNodeUniformScaleEdit = settingsManager.getSettingGetter<std::function<void(uint64_t, float)>>("queueSceneNodeUniformScaleEdit")();

	setMeshShaderEnabled = settingsManager.getSettingSetter<bool>("enableMeshShader");
	getMeshShaderEnabled = settingsManager.getSettingGetter<bool>("enableMeshShader");
	meshShaderEnabled = getMeshShaderEnabled();
	observerSetting(meshShaderEnabled, "enableMeshShader");

	setIndirectDrawsEnabled = settingsManager.getSettingSetter<bool>("enableIndirectDraws");
	getIndirectDrawsEnabled = settingsManager.getSettingGetter<bool>("enableIndirectDraws");
	indirectDrawsEnabled = getIndirectDrawsEnabled();
	observerSetting(indirectDrawsEnabled, "enableIndirectDraws");

	getOcclusionCullingEnabled = settingsManager.getSettingGetter<bool>("enableOcclusionCulling");
	setOcclusionCullingEnabled = settingsManager.getSettingSetter<bool>("enableOcclusionCulling");
	occlusionCulling = getOcclusionCullingEnabled();
	observerSetting(occlusionCulling, "enableOcclusionCulling");

    getCLodFrustumCulling = settingsManager.getSettingGetter<bool>(CLodFrustumCullingSettingName);
    setCLodFrustumCulling = settingsManager.getSettingSetter<bool>(CLodFrustumCullingSettingName);
    m_clodFrustumCulling = getCLodFrustumCulling();
    observerSetting(m_clodFrustumCulling, CLodFrustumCullingSettingName);

    getCLodCullingBackend = settingsManager.getSettingGetter<CLodCullingBackend>(CLodCullingBackendSettingName);
    setCLodCullingBackend = settingsManager.getSettingSetter<CLodCullingBackend>(CLodCullingBackendSettingName);
    m_clodCullingBackend = getCLodCullingBackend();
    observerSetting(m_clodCullingBackend, CLodCullingBackendSettingName);

    getCLodPureComputePhase2ExpansionFactor =
        settingsManager.getSettingGetter<uint32_t>(CLodPureComputePhase2ExpansionFactorSettingName);
    setCLodPureComputePhase2ExpansionFactor =
        settingsManager.getSettingSetter<uint32_t>(CLodPureComputePhase2ExpansionFactorSettingName);
    m_clodPureComputePhase2ExpansionFactor =
        CLodNormalizePureComputePhase2ExpansionFactor(getCLodPureComputePhase2ExpansionFactor());
    observerSetting(m_clodPureComputePhase2ExpansionFactor, CLodPureComputePhase2ExpansionFactorSettingName);

    getCLodSoftwareRasterMode = settingsManager.getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName);
    setCLodSoftwareRasterMode = settingsManager.getSettingSetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName);
    m_clodSoftwareRasterMode = getCLodSoftwareRasterMode();
    observerSetting(m_clodSoftwareRasterMode, CLodSoftwareRasterModeSettingName);

    getCLodVSMRasterMode = settingsManager.getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName);
    setCLodVSMRasterMode = settingsManager.getSettingSetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName);
    m_clodVSMRasterMode = getCLodVSMRasterMode();
    observerSetting(m_clodVSMRasterMode, CLodVSMRasterModeSettingName);

    getCLodTransparencyMode = settingsManager.getSettingGetter<CLodTransparencyMode>(CLodTransparencyModeSettingName);
    setCLodTransparencyMode = settingsManager.getSettingSetter<CLodTransparencyMode>(CLodTransparencyModeSettingName);
    m_clodTransparencyMode = getCLodTransparencyMode();
    observerSetting(m_clodTransparencyMode, CLodTransparencyModeSettingName);

    getCLodDisableReyesRasterization = settingsManager.getSettingGetter<bool>(CLodDisableReyesRasterizationSettingName);
    setCLodDisableReyesRasterization = settingsManager.getSettingSetter<bool>(CLodDisableReyesRasterizationSettingName);
    m_clodDisableReyesRasterization = getCLodDisableReyesRasterization();
    observerSetting(m_clodDisableReyesRasterization, CLodDisableReyesRasterizationSettingName);

    getCLodReyesGeometricNormal = settingsManager.getSettingGetter<bool>(CLodReyesGeometricNormalSettingName);
    setCLodReyesGeometricNormal = settingsManager.getSettingSetter<bool>(CLodReyesGeometricNormalSettingName);
    m_clodReyesGeometricNormal = getCLodReyesGeometricNormal();
    observerSetting(m_clodReyesGeometricNormal, CLodReyesGeometricNormalSettingName);

    getCLodReyesObjectNormalMapBlend = settingsManager.getSettingGetter<float>(CLodReyesObjectNormalMapBlendSettingName);
    setCLodReyesObjectNormalMapBlend = settingsManager.getSettingSetter<float>(CLodReyesObjectNormalMapBlendSettingName);
    m_clodReyesObjectNormalMapBlend = getCLodReyesObjectNormalMapBlend();
    observerSetting(m_clodReyesObjectNormalMapBlend, CLodReyesObjectNormalMapBlendSettingName);

    getCLodReyesTerrainNormalBlend = settingsManager.getSettingGetter<float>(CLodReyesTerrainNormalBlendSettingName);
    setCLodReyesTerrainNormalBlend = settingsManager.getSettingSetter<float>(CLodReyesTerrainNormalBlendSettingName);
    m_clodReyesTerrainNormalBlend = getCLodReyesTerrainNormalBlend();
    observerSetting(m_clodReyesTerrainNormalBlend, CLodReyesTerrainNormalBlendSettingName);

    getCLodReyesTerrainNormalMipBias = settingsManager.getSettingGetter<uint32_t>(CLodReyesTerrainNormalMipBiasSettingName);
    setCLodReyesTerrainNormalMipBias = settingsManager.getSettingSetter<uint32_t>(CLodReyesTerrainNormalMipBiasSettingName);
    m_clodReyesTerrainNormalMipBias = static_cast<int>(getCLodReyesTerrainNormalMipBias());
    m_settingSubscriptions.push_back(settingsManager.addObserver<uint32_t>(
        CLodReyesTerrainNormalMipBiasSettingName,
        [this](const uint32_t& newValue) {
            m_clodReyesTerrainNormalMipBias = static_cast<int>(newValue);
        }));

    getCLodReyesDiceRatePixels = settingsManager.getSettingGetter<float>(CLodReyesDiceRatePixelsSettingName);
    setCLodReyesDiceRatePixels = settingsManager.getSettingSetter<float>(CLodReyesDiceRatePixelsSettingName);
    m_clodReyesDiceRatePixels = getCLodReyesDiceRatePixels();
    observerSetting(m_clodReyesDiceRatePixels, CLodReyesDiceRatePixelsSettingName);

    getCLodReyesUseAabbOcclusion = settingsManager.getSettingGetter<bool>(CLodReyesUseAabbOcclusionSettingName);
    setCLodReyesUseAabbOcclusion = settingsManager.getSettingSetter<bool>(CLodReyesUseAabbOcclusionSettingName);
    m_clodReyesUseAabbOcclusion = getCLodReyesUseAabbOcclusion();
    observerSetting(m_clodReyesUseAabbOcclusion, CLodReyesUseAabbOcclusionSettingName);

    getCLodDisableVirtualShadowPageCaching = settingsManager.getSettingGetter<bool>(CLodDisableVirtualShadowPageCachingSettingName);
    setCLodDisableVirtualShadowPageCaching = settingsManager.getSettingSetter<bool>(CLodDisableVirtualShadowPageCachingSettingName);
    m_clodDisableVirtualShadowPageCaching = getCLodDisableVirtualShadowPageCaching();
    observerSetting(m_clodDisableVirtualShadowPageCaching, CLodDisableVirtualShadowPageCachingSettingName);

    getCLodEnablePageJobVSM = settingsManager.getSettingGetter<bool>(CLodEnablePageJobVSMSettingName);
    setCLodEnablePageJobVSM = settingsManager.getSettingSetter<bool>(CLodEnablePageJobVSMSettingName);
    m_clodEnablePageJobVSM = getCLodEnablePageJobVSM();
    observerSetting(m_clodEnablePageJobVSM, CLodEnablePageJobVSMSettingName);

    getCLodReyesShadowCoarseTargetPagesPerTriangle = settingsManager.getSettingGetter<float>(CLodReyesShadowCoarseTargetPagesPerTriangleSettingName);
    setCLodReyesShadowCoarseTargetPagesPerTriangle = settingsManager.getSettingSetter<float>(CLodReyesShadowCoarseTargetPagesPerTriangleSettingName);
    m_clodReyesShadowCoarseTargetPagesPerTriangle = getCLodReyesShadowCoarseTargetPagesPerTriangle();
    observerSetting(m_clodReyesShadowCoarseTargetPagesPerTriangle, CLodReyesShadowCoarseTargetPagesPerTriangleSettingName);

    getCLodPageJobDiameterThreshold = settingsManager.getSettingGetter<uint32_t>(CLodPageJobDiameterThresholdSettingName);
    setCLodPageJobDiameterThreshold = settingsManager.getSettingSetter<uint32_t>(CLodPageJobDiameterThresholdSettingName);
    m_clodPageJobDiameterThreshold = getCLodPageJobDiameterThreshold();
    observerSetting(m_clodPageJobDiameterThreshold, CLodPageJobDiameterThresholdSettingName);

    getCLodPageJobSparseRatio = settingsManager.getSettingGetter<float>(CLodPageJobSparseRatioSettingName);
    setCLodPageJobSparseRatio = settingsManager.getSettingSetter<float>(CLodPageJobSparseRatioSettingName);
    m_clodPageJobSparseRatio = getCLodPageJobSparseRatio();
    observerSetting(m_clodPageJobSparseRatio, CLodPageJobSparseRatioSettingName);

    getCLodPageJobMaxPagesPerCluster = settingsManager.getSettingGetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName);
    setCLodPageJobMaxPagesPerCluster = settingsManager.getSettingSetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName);
    m_clodPageJobMaxPagesPerCluster = getCLodPageJobMaxPagesPerCluster();
    observerSetting(m_clodPageJobMaxPagesPerCluster, CLodPageJobMaxPagesPerClusterSettingName);

    getCLodPageJobRecordCapacity = settingsManager.getSettingGetter<uint32_t>(CLodPageJobRecordCapacitySettingName);
    setCLodPageJobRecordCapacity = settingsManager.getSettingSetter<uint32_t>(CLodPageJobRecordCapacitySettingName);
    m_clodPageJobRecordCapacity = getCLodPageJobRecordCapacity();
    observerSetting(m_clodPageJobRecordCapacity, CLodPageJobRecordCapacitySettingName);

    getCLodPageJobForceAll = settingsManager.getSettingGetter<bool>(CLodPageJobForceAllSettingName);
    setCLodPageJobForceAll = settingsManager.getSettingSetter<bool>(CLodPageJobForceAllSettingName);
    m_clodPageJobForceAll = getCLodPageJobForceAll();
    observerSetting(m_clodPageJobForceAll, CLodPageJobForceAllSettingName);

    getCLodForceTraversalDepthRoot = settingsManager.getSettingGetter<uint32_t>(CLodForceTraversalDepthRootSettingName);
    setCLodForceTraversalDepthRoot = settingsManager.getSettingSetter<uint32_t>(CLodForceTraversalDepthRootSettingName);
    m_clodForceTraversalDepthRoot = getCLodForceTraversalDepthRoot();
    observerSetting(m_clodForceTraversalDepthRoot, CLodForceTraversalDepthRootSettingName);

    getCLodVisibleClusterCapacity = settingsManager.getSettingGetter<uint32_t>(CLodVisibleClusterCapacitySettingName);
    setCLodVisibleClusterCapacity = settingsManager.getSettingSetter<uint32_t>(CLodVisibleClusterCapacitySettingName);
    m_clodVisibleClusterCapacity = getCLodVisibleClusterCapacity();
    observerSetting(m_clodVisibleClusterCapacity, CLodVisibleClusterCapacitySettingName);

    getCLodDirectionalVirtualShadowMaxBackingResolution = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName);
    setCLodDirectionalVirtualShadowMaxBackingResolution = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName);
    m_clodDirectionalVirtualShadowMaxBackingResolution = getCLodDirectionalVirtualShadowMaxBackingResolution();
    observerSetting(m_clodDirectionalVirtualShadowMaxBackingResolution, CLodDirectionalVirtualShadowMaxBackingResolutionSettingName);

    getCLodDirectionalVirtualShadowMaxPhysicalPages = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName);
    setCLodDirectionalVirtualShadowMaxPhysicalPages = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName);
    m_clodDirectionalVirtualShadowMaxPhysicalPages = getCLodDirectionalVirtualShadowMaxPhysicalPages();
    observerSetting(m_clodDirectionalVirtualShadowMaxPhysicalPages, CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName);

    getCLodDirectionalVirtualShadowLodBias = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowLodBiasSettingName);
    setCLodDirectionalVirtualShadowLodBias = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowLodBiasSettingName);
    m_clodDirectionalVirtualShadowLodBias = getCLodDirectionalVirtualShadowLodBias();
    observerSetting(m_clodDirectionalVirtualShadowLodBias, CLodDirectionalVirtualShadowLodBiasSettingName);

    getCLodDirectionalVirtualShadowAutoLodBias = settingsManager.getSettingGetter<bool>(CLodDirectionalVirtualShadowAutoLodBiasSettingName);
    setCLodDirectionalVirtualShadowAutoLodBias = settingsManager.getSettingSetter<bool>(CLodDirectionalVirtualShadowAutoLodBiasSettingName);
    m_clodDirectionalVirtualShadowAutoLodBias = getCLodDirectionalVirtualShadowAutoLodBias();
    observerSetting(m_clodDirectionalVirtualShadowAutoLodBias, CLodDirectionalVirtualShadowAutoLodBiasSettingName);

    getCLodDirectionalVirtualShadowAutoLodBiasScale = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName);
    setCLodDirectionalVirtualShadowAutoLodBiasScale = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName);
    m_clodDirectionalVirtualShadowAutoLodBiasScale = getCLodDirectionalVirtualShadowAutoLodBiasScale();
    observerSetting(m_clodDirectionalVirtualShadowAutoLodBiasScale, CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName);

    getCLodDirectionalVirtualShadowPredictiveLodInvalidation = settingsManager.getSettingGetter<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName);
    setCLodDirectionalVirtualShadowPredictiveLodInvalidation = settingsManager.getSettingSetter<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName);
    m_clodDirectionalVirtualShadowPredictiveLodInvalidation = getCLodDirectionalVirtualShadowPredictiveLodInvalidation();
    observerSetting(m_clodDirectionalVirtualShadowPredictiveLodInvalidation, CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName);

    getCLodDirectionalVirtualShadowSourceAngleDegrees = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowSourceAngleDegreesSettingName);
    setCLodDirectionalVirtualShadowSourceAngleDegrees = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowSourceAngleDegreesSettingName);
    m_clodDirectionalVirtualShadowSourceAngleDegrees = getCLodDirectionalVirtualShadowSourceAngleDegrees();
    observerSetting(m_clodDirectionalVirtualShadowSourceAngleDegrees, CLodDirectionalVirtualShadowSourceAngleDegreesSettingName);

    getCLodDirectionalVirtualShadowSmrtRayCountDirectional = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName);
    setCLodDirectionalVirtualShadowSmrtRayCountDirectional = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName);
    m_clodDirectionalVirtualShadowSmrtRayCountDirectional = getCLodDirectionalVirtualShadowSmrtRayCountDirectional();
    observerSetting(m_clodDirectionalVirtualShadowSmrtRayCountDirectional, CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName);

    getCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName);
    setCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName);
    m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = getCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional();
    observerSetting(m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional, CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName);

    getCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName);
    setCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName);
    m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees = getCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees();
    observerSetting(m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees, CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName);

    getCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName);
    setCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName);
    m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional = getCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional();
    observerSetting(m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional, CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName);

    getCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName);
    setCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName);
    m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld = getCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld();
    observerSetting(m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld, CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName);

    getCLodDirectionalVirtualShadowReceiverTraceEnabled = settingsManager.getSettingGetter<bool>(CLodDirectionalVirtualShadowReceiverTraceEnabledSettingName);
    setCLodDirectionalVirtualShadowReceiverTraceEnabled = settingsManager.getSettingSetter<bool>(CLodDirectionalVirtualShadowReceiverTraceEnabledSettingName);
    m_clodDirectionalVirtualShadowReceiverTraceEnabled = getCLodDirectionalVirtualShadowReceiverTraceEnabled();
    observerSetting(m_clodDirectionalVirtualShadowReceiverTraceEnabled, CLodDirectionalVirtualShadowReceiverTraceEnabledSettingName);

    getCLodDirectionalVirtualShadowReceiverTraceSampleCount = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowReceiverTraceSampleCountSettingName);
    setCLodDirectionalVirtualShadowReceiverTraceSampleCount = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowReceiverTraceSampleCountSettingName);
    m_clodDirectionalVirtualShadowReceiverTraceSampleCount = getCLodDirectionalVirtualShadowReceiverTraceSampleCount();
    observerSetting(m_clodDirectionalVirtualShadowReceiverTraceSampleCount, CLodDirectionalVirtualShadowReceiverTraceSampleCountSettingName);

    getCLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorldSettingName);
    setCLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorldSettingName);
    m_clodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld = getCLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld();
    observerSetting(m_clodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld, CLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorldSettingName);

    getCLodDirectionalVirtualShadowReceiverTraceUncertaintyScale = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowReceiverTraceUncertaintyScaleSettingName);
    setCLodDirectionalVirtualShadowReceiverTraceUncertaintyScale = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowReceiverTraceUncertaintyScaleSettingName);
    m_clodDirectionalVirtualShadowReceiverTraceUncertaintyScale = getCLodDirectionalVirtualShadowReceiverTraceUncertaintyScale();
    observerSetting(m_clodDirectionalVirtualShadowReceiverTraceUncertaintyScale, CLodDirectionalVirtualShadowReceiverTraceUncertaintyScaleSettingName);

    getCLodDirectionalVirtualShadowReceiverTraceDepthSafetyScale = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowReceiverTraceDepthSafetyScaleSettingName);
    setCLodDirectionalVirtualShadowReceiverTraceDepthSafetyScale = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowReceiverTraceDepthSafetyScaleSettingName);
    m_clodDirectionalVirtualShadowReceiverTraceDepthSafetyScale = getCLodDirectionalVirtualShadowReceiverTraceDepthSafetyScale();
    observerSetting(m_clodDirectionalVirtualShadowReceiverTraceDepthSafetyScale, CLodDirectionalVirtualShadowReceiverTraceDepthSafetyScaleSettingName);

    getNumDirectionalLightCascades = settingsManager.getSettingGetter<uint8_t>("numDirectionalLightCascades");
    setNumDirectionalLightCascades = settingsManager.getSettingSetter<uint8_t>("numDirectionalLightCascades");
    m_numDirectionalLightCascades = getNumDirectionalLightCascades();
    observerSetting(m_numDirectionalLightCascades, "numDirectionalLightCascades");

    getDirectionalShadowDistanceLowerBound = settingsManager.getSettingGetter<float>("directionalShadowDistanceLowerBound");
    setDirectionalShadowDistanceLowerBound = settingsManager.getSettingSetter<float>("directionalShadowDistanceLowerBound");
    m_directionalShadowDistanceLowerBound = getDirectionalShadowDistanceLowerBound();
    observerSetting(m_directionalShadowDistanceLowerBound, "directionalShadowDistanceLowerBound");

	setWireframeEnabled = settingsManager.getSettingSetter<bool>("enableWireframe");
	getWireframeEnabled = settingsManager.getSettingGetter<bool>("enableWireframe");
	wireframeEnabled = getWireframeEnabled();
	observerSetting(wireframeEnabled, "enableWireframe");

	setAllowTearing = settingsManager.getSettingSetter<bool>("allowTearing");
	getAllowTearing = settingsManager.getSettingGetter<bool>("allowTearing");
	allowTearing = getAllowTearing();
	observerSetting(allowTearing, "allowTearing");

	setDrawBoundingSpheres = settingsManager.getSettingSetter<bool>("drawBoundingSpheres");
	getDrawBoundingSpheres = settingsManager.getSettingGetter<bool>("drawBoundingSpheres");
	drawBoundingSpheres = getDrawBoundingSpheres();
	observerSetting(drawBoundingSpheres, "drawBoundingSpheres");

	setClusteredLightingEnabled = settingsManager.getSettingSetter<bool>("enableClusteredLighting");
	getClusteredLightingEnabled = settingsManager.getSettingGetter<bool>("enableClusteredLighting");
	clusteredLighting = getClusteredLightingEnabled();
	observerSetting(clusteredLighting, "enableClusteredLighting");

	setVisibilityRenderingEnabled = settingsManager.getSettingSetter<bool>("enableVisibilityRendering");
	getVisibilityRenderingEnabled = settingsManager.getSettingGetter<bool>("enableVisibilityRendering");
	m_visibilityRenderingEnabled = getVisibilityRenderingEnabled();
	observerSetting(m_visibilityRenderingEnabled, "enableVisibilityRendering");
    setTerrainRegionMaterialEvaluationEnabled = settingsManager.getSettingSetter<bool>("enableTerrainRegionMaterialEvaluation");
    getTerrainRegionMaterialEvaluationEnabled = settingsManager.getSettingGetter<bool>("enableTerrainRegionMaterialEvaluation");
    m_terrainRegionMaterialEvaluationEnabled = getTerrainRegionMaterialEvaluationEnabled();
    observerSetting(m_terrainRegionMaterialEvaluationEnabled, "enableTerrainRegionMaterialEvaluation");
    setTerrainRvtEnabled = settingsManager.getSettingSetter<bool>("enableTerrainRvt");
    getTerrainRvtEnabled = settingsManager.getSettingGetter<bool>("enableTerrainRvt");
    m_terrainRvtEnabled = getTerrainRvtEnabled();
    observerSetting(m_terrainRvtEnabled, "enableTerrainRvt");
    setForceDirectTerrainRvtFallback = settingsManager.getSettingSetter<bool>("forceDirectTerrainRvtFallback");
    getForceDirectTerrainRvtFallback = settingsManager.getSettingGetter<bool>("forceDirectTerrainRvtFallback");
    m_forceDirectTerrainRvtFallback = getForceDirectTerrainRvtFallback();
    observerSetting(m_forceDirectTerrainRvtFallback, "forceDirectTerrainRvtFallback");
    setTerrainRvtTelemetryDebug = settingsManager.getSettingSetter<bool>("terrainRvtTelemetryDebug");
    getTerrainRvtTelemetryDebug = settingsManager.getSettingGetter<bool>("terrainRvtTelemetryDebug");
    m_terrainRvtTelemetryDebug = getTerrainRvtTelemetryDebug();
    observerSetting(m_terrainRvtTelemetryDebug, "terrainRvtTelemetryDebug");
    setTerrainRvtDebugView = settingsManager.getSettingSetter<uint32_t>("terrainRvtDebugView");
    getTerrainRvtDebugView = settingsManager.getSettingGetter<uint32_t>("terrainRvtDebugView");
    m_terrainRvtDebugView = static_cast<int>(getTerrainRvtDebugView());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtDebugView",
        [this](const uint32_t& newValue) {
            m_terrainRvtDebugView = static_cast<int>(newValue);
        }));
    setTerrainRvtPageSize = settingsManager.getSettingSetter<uint32_t>("terrainRvtPageSize");
    getTerrainRvtPageSize = settingsManager.getSettingGetter<uint32_t>("terrainRvtPageSize");
    m_terrainRvtPageSize = static_cast<int>(getTerrainRvtPageSize());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtPageSize",
        [this](const uint32_t& newValue) {
            m_terrainRvtPageSize = static_cast<int>(newValue);
        }));
    setTerrainRvtBorderTexels = settingsManager.getSettingSetter<uint32_t>("terrainRvtBorderTexels");
    getTerrainRvtBorderTexels = settingsManager.getSettingGetter<uint32_t>("terrainRvtBorderTexels");
    m_terrainRvtBorderTexels = static_cast<int>(getTerrainRvtBorderTexels());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtBorderTexels",
        [this](const uint32_t& newValue) {
            m_terrainRvtBorderTexels = static_cast<int>(newValue);
        }));
    setTerrainRvtMipCount = settingsManager.getSettingSetter<uint32_t>("terrainRvtMipCount");
    getTerrainRvtMipCount = settingsManager.getSettingGetter<uint32_t>("terrainRvtMipCount");
    m_terrainRvtMipCount = static_cast<int>(getTerrainRvtMipCount());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtMipCount",
        [this](const uint32_t& newValue) {
            m_terrainRvtMipCount = static_cast<int>(newValue);
        }));
    setTerrainRvtMipOffset = settingsManager.getSettingSetter<float>("terrainRvtMipOffset");
    getTerrainRvtMipOffset = settingsManager.getSettingGetter<float>("terrainRvtMipOffset");
    m_terrainRvtMipOffset = getTerrainRvtMipOffset();
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<float>(
        "terrainRvtMipOffset",
        [this](const float& newValue) {
            m_terrainRvtMipOffset = newValue;
        }));
    setTerrainRvtSourceTexelsPerWorld = settingsManager.getSettingSetter<float>("terrainRvtSourceTexelsPerWorld");
    getTerrainRvtSourceTexelsPerWorld = settingsManager.getSettingGetter<float>("terrainRvtSourceTexelsPerWorld");
    m_terrainRvtSourceTexelsPerWorld = getTerrainRvtSourceTexelsPerWorld();
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<float>(
        "terrainRvtSourceTexelsPerWorld",
        [this](const float& newValue) {
            m_terrainRvtSourceTexelsPerWorld = newValue;
        }));
    setTerrainRvtPhysicalAtlasPagesWide = settingsManager.getSettingSetter<uint32_t>("terrainRvtPhysicalAtlasPagesWide");
    getTerrainRvtPhysicalAtlasPagesWide = settingsManager.getSettingGetter<uint32_t>("terrainRvtPhysicalAtlasPagesWide");
    m_terrainRvtPhysicalAtlasPagesWide = static_cast<int>(getTerrainRvtPhysicalAtlasPagesWide());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtPhysicalAtlasPagesWide",
        [this](const uint32_t& newValue) {
            m_terrainRvtPhysicalAtlasPagesWide = static_cast<int>(newValue);
        }));
    setTerrainRvtPhysicalAtlasPagesHigh = settingsManager.getSettingSetter<uint32_t>("terrainRvtPhysicalAtlasPagesHigh");
    getTerrainRvtPhysicalAtlasPagesHigh = settingsManager.getSettingGetter<uint32_t>("terrainRvtPhysicalAtlasPagesHigh");
    m_terrainRvtPhysicalAtlasPagesHigh = static_cast<int>(getTerrainRvtPhysicalAtlasPagesHigh());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtPhysicalAtlasPagesHigh",
        [this](const uint32_t& newValue) {
            m_terrainRvtPhysicalAtlasPagesHigh = static_cast<int>(newValue);
        }));
    setTerrainRvtPhysicalAtlasPoolCount = settingsManager.getSettingSetter<uint32_t>("terrainRvtPhysicalAtlasPoolCount");
    getTerrainRvtPhysicalAtlasPoolCount = settingsManager.getSettingGetter<uint32_t>("terrainRvtPhysicalAtlasPoolCount");
    m_terrainRvtPhysicalAtlasPoolCount = static_cast<int>(getTerrainRvtPhysicalAtlasPoolCount());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtPhysicalAtlasPoolCount",
        [this](const uint32_t& newValue) {
            m_terrainRvtPhysicalAtlasPoolCount = static_cast<int>(newValue);
        }));

    setTerrainStochasticSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainStochasticSampling");
    getTerrainStochasticSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainStochasticSampling");
    m_terrainStochasticSamplingEnabled = getTerrainStochasticSamplingEnabled();
    observerSetting(m_terrainStochasticSamplingEnabled, "enableTerrainStochasticSampling");
    setTerrainStochasticDiffuseSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainStochasticDiffuseSampling");
    getTerrainStochasticDiffuseSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainStochasticDiffuseSampling");
    m_terrainStochasticDiffuseSamplingEnabled = getTerrainStochasticDiffuseSamplingEnabled();
    observerSetting(m_terrainStochasticDiffuseSamplingEnabled, "enableTerrainStochasticDiffuseSampling");
    setTerrainStochasticNormalSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainStochasticNormalSampling");
    getTerrainStochasticNormalSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainStochasticNormalSampling");
    m_terrainStochasticNormalSamplingEnabled = getTerrainStochasticNormalSamplingEnabled();
    observerSetting(m_terrainStochasticNormalSamplingEnabled, "enableTerrainStochasticNormalSampling");
    setTerrainStochasticDerivativeNormalSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainStochasticDerivativeNormalSampling");
    getTerrainStochasticDerivativeNormalSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainStochasticDerivativeNormalSampling");
    m_terrainStochasticDerivativeNormalSamplingEnabled = getTerrainStochasticDerivativeNormalSamplingEnabled();
    observerSetting(m_terrainStochasticDerivativeNormalSamplingEnabled, "enableTerrainStochasticDerivativeNormalSampling");
    setTerrainStochasticBlendCurve = settingsManager.getSettingSetter<float>("terrainStochasticBlendCurve");
    getTerrainStochasticBlendCurve = settingsManager.getSettingGetter<float>("terrainStochasticBlendCurve");
    m_terrainStochasticBlendCurve = getTerrainStochasticBlendCurve();
    observerSetting(m_terrainStochasticBlendCurve, "terrainStochasticBlendCurve");
    setTerrainGaussianStochasticSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainGaussianStochasticSampling");
    getTerrainGaussianStochasticSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainGaussianStochasticSampling");
    m_terrainGaussianStochasticSamplingEnabled = getTerrainGaussianStochasticSamplingEnabled();
    observerSetting(m_terrainGaussianStochasticSamplingEnabled, "enableTerrainGaussianStochasticSampling");
    setParallaxOcclusionMappingEnabled = settingsManager.getSettingSetter<bool>("enableParallaxOcclusionMapping");
    getParallaxOcclusionMappingEnabled = settingsManager.getSettingGetter<bool>("enableParallaxOcclusionMapping");
    m_parallaxOcclusionMappingEnabled = getParallaxOcclusionMappingEnabled();
    observerSetting(m_parallaxOcclusionMappingEnabled, "enableParallaxOcclusionMapping");
    setTerrainParallaxOcclusionMappingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainParallaxOcclusionMapping");
    getTerrainParallaxOcclusionMappingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainParallaxOcclusionMapping");
    m_terrainParallaxOcclusionMappingEnabled = getTerrainParallaxOcclusionMappingEnabled();
    observerSetting(m_terrainParallaxOcclusionMappingEnabled, "enableTerrainParallaxOcclusionMapping");
    setTerrainReyesDisplacementEnabled = settingsManager.getSettingSetter<bool>("enableTerrainReyesDisplacement");
    getTerrainReyesDisplacementEnabled = settingsManager.getSettingGetter<bool>("enableTerrainReyesDisplacement");
    m_terrainReyesDisplacementEnabled = getTerrainReyesDisplacementEnabled();
    observerSetting(m_terrainReyesDisplacementEnabled, "enableTerrainReyesDisplacement");
    setTerrainReyesDisplacementScale = settingsManager.getSettingSetter<float>("terrainReyesDisplacementGlobalScale");
    getTerrainReyesDisplacementScale = settingsManager.getSettingGetter<float>("terrainReyesDisplacementGlobalScale");
    m_terrainReyesDisplacementScale = getTerrainReyesDisplacementScale();
    observerSetting(m_terrainReyesDisplacementScale, "terrainReyesDisplacementGlobalScale");
    setObjectReyesDisplacementScale = settingsManager.getSettingSetter<float>("objectReyesDisplacementScale");
    getObjectReyesDisplacementScale = settingsManager.getSettingGetter<float>("objectReyesDisplacementScale");
    m_objectReyesDisplacementScale = getObjectReyesDisplacementScale();
    observerSetting(m_objectReyesDisplacementScale, "objectReyesDisplacementScale");
    setTerrainParallaxHeightScale = settingsManager.getSettingSetter<float>("terrainParallaxHeightScale");
    getTerrainParallaxHeightScale = settingsManager.getSettingGetter<float>("terrainParallaxHeightScale");
    m_terrainParallaxHeightScale = getTerrainParallaxHeightScale();
    observerSetting(m_terrainParallaxHeightScale, "terrainParallaxHeightScale");
    setObjectParallaxHeightScale = settingsManager.getSettingSetter<float>("objectParallaxHeightScale");
    getObjectParallaxHeightScale = settingsManager.getSettingGetter<float>("objectParallaxHeightScale");
    m_objectParallaxHeightScale = getObjectParallaxHeightScale();
    observerSetting(m_objectParallaxHeightScale, "objectParallaxHeightScale");
    setProceduralWindDisplacementScale = settingsManager.getSettingSetter<float>(ProceduralWindDisplacementScaleSettingName);
    getProceduralWindDisplacementScale = settingsManager.getSettingGetter<float>(ProceduralWindDisplacementScaleSettingName);
    m_proceduralWindDisplacementScale = getProceduralWindDisplacementScale();
    observerSetting(m_proceduralWindDisplacementScale, ProceduralWindDisplacementScaleSettingName);
    setProceduralWindGrassDisplacementScale = settingsManager.getSettingSetter<float>(ProceduralWindGrassDisplacementScaleSettingName);
    getProceduralWindGrassDisplacementScale = settingsManager.getSettingGetter<float>(ProceduralWindGrassDisplacementScaleSettingName);
    m_proceduralWindGrassDisplacementScale = getProceduralWindGrassDisplacementScale();
    observerSetting(m_proceduralWindGrassDisplacementScale, ProceduralWindGrassDisplacementScaleSettingName);
    setProceduralWindGrassOscillationScale = settingsManager.getSettingSetter<float>(ProceduralWindGrassOscillationScaleSettingName);
    getProceduralWindGrassOscillationScale = settingsManager.getSettingGetter<float>(ProceduralWindGrassOscillationScaleSettingName);
    m_proceduralWindGrassOscillationScale = getProceduralWindGrassOscillationScale();
    observerSetting(m_proceduralWindGrassOscillationScale, ProceduralWindGrassOscillationScaleSettingName);
    setProceduralWindGrassFlutterFrequency = settingsManager.getSettingSetter<float>(ProceduralWindGrassFlutterFrequencySettingName);
    getProceduralWindGrassFlutterFrequency = settingsManager.getSettingGetter<float>(ProceduralWindGrassFlutterFrequencySettingName);
    m_proceduralWindGrassFlutterFrequency = getProceduralWindGrassFlutterFrequency();
    observerSetting(m_proceduralWindGrassFlutterFrequency, ProceduralWindGrassFlutterFrequencySettingName);
    setProceduralWindEffectDistance = settingsManager.getSettingSetter<float>(ProceduralWindOuterRadiusSettingName);
    setProceduralWindInnerRadius = settingsManager.getSettingSetter<float>(ProceduralWindInnerRadiusSettingName);
    m_proceduralWindEffectDistance = settingsManager.getSettingGetter<float>(ProceduralWindOuterRadiusSettingName)();
    observerSetting(m_proceduralWindEffectDistance, ProceduralWindOuterRadiusSettingName);
    setTerrainParallaxMaxSteps = settingsManager.getSettingSetter<uint32_t>("terrainParallaxMaxSteps");
    getTerrainParallaxMaxSteps = settingsManager.getSettingGetter<uint32_t>("terrainParallaxMaxSteps");
    m_terrainParallaxMaxSteps = getTerrainParallaxMaxSteps();
    observerSetting(m_terrainParallaxMaxSteps, "terrainParallaxMaxSteps");
    setTerrainParallaxFadeStartDistance = settingsManager.getSettingSetter<float>("terrainParallaxFadeStartDistance");
    getTerrainParallaxFadeStartDistance = settingsManager.getSettingGetter<float>("terrainParallaxFadeStartDistance");
    m_terrainParallaxFadeStartDistance = getTerrainParallaxFadeStartDistance();
    observerSetting(m_terrainParallaxFadeStartDistance, "terrainParallaxFadeStartDistance");
    setTerrainParallaxFadeEndDistance = settingsManager.getSettingSetter<float>("terrainParallaxFadeEndDistance");
    getTerrainParallaxFadeEndDistance = settingsManager.getSettingGetter<float>("terrainParallaxFadeEndDistance");
    m_terrainParallaxFadeEndDistance = getTerrainParallaxFadeEndDistance();
    observerSetting(m_terrainParallaxFadeEndDistance, "terrainParallaxFadeEndDistance");

	getGTAOEnabled = settingsManager.getSettingGetter<bool>("enableGTAO");
	setGTAOEnabled = settingsManager.getSettingSetter<bool>("enableGTAO");
	m_gtaoEnabled = getGTAOEnabled();
	observerSetting(m_gtaoEnabled, "enableGTAO");

	setBloomEnabled = settingsManager.getSettingSetter<bool>("enableBloom");
	getBloomEnabled = settingsManager.getSettingGetter<bool>("enableBloom");
	m_bloomEnabled = getBloomEnabled();
	observerSetting(m_bloomEnabled, "enableBloom");

	setScreenSpaceReflectionsEnabled = settingsManager.getSettingSetter<bool>("enableScreenSpaceReflections");
	getScreenSpaceReflectionsEnabled = settingsManager.getSettingGetter<bool>("enableScreenSpaceReflections");
	m_screenSpaceReflectionsEnabled = getScreenSpaceReflectionsEnabled();
	observerSetting(m_screenSpaceReflectionsEnabled, "enableScreenSpaceReflections");

    setRayTracedReflectionsEnabled = settingsManager.getSettingSetter<bool>("enableRayTracedReflections");
    getRayTracedReflectionsEnabled = settingsManager.getSettingGetter<bool>("enableRayTracedReflections");
    m_rayTracedReflectionsEnabled = getRayTracedReflectionsEnabled();
    observerSetting(m_rayTracedReflectionsEnabled, "enableRayTracedReflections");

    setJitterEnabled = settingsManager.getSettingSetter<bool>("enableJitter");
    getJitterEnabled = settingsManager.getSettingGetter<bool>("enableJitter");
    m_jitterEnabled = getJitterEnabled();
	observerSetting(m_jitterEnabled, "enableJitter");

    setRememberCameraPose = settingsManager.getSettingSetter<bool>("rememberCameraPose");
    getRememberCameraPose = settingsManager.getSettingGetter<bool>("rememberCameraPose");
    m_rememberCameraPose = getRememberCameraPose();
    observerSetting(m_rememberCameraPose, "rememberCameraPose");

    getCollectPassStatistics = settingsManager.getSettingGetter<bool>("collectPassStatistics");
    setCollectPassStatistics = settingsManager.getSettingSetter<bool>("collectPassStatistics");
    m_collectPassStatistics = getCollectPassStatistics();
    observerSetting(m_collectPassStatistics, "collectPassStatistics");

	getCollectPipelineStatistics = settingsManager.getSettingGetter<bool>("collectPipelineStatistics");
	setCollectPipelineStatistics = settingsManager.getSettingSetter<bool>("collectPipelineStatistics");
	m_collectPipelineStatistics = getCollectPipelineStatistics();

    getUpscalingMode = settingsManager.getSettingGetter<UpscalingMode>("upscalingMode");
    setUpscalingMode = settingsManager.getSettingSetter<UpscalingMode>("upscalingMode");
    m_currentUpscalingMode = getUpscalingMode();
	observerSetting(m_currentUpscalingMode, "upscalingMode");

	getDilatedMotionVectorsEnabled = settingsManager.getSettingGetter<bool>("enableDilatedMotionVectors");
	setDilatedMotionVectorsEnabled = settingsManager.getSettingSetter<bool>("enableDilatedMotionVectors");
	m_dilatedMotionVectorsEnabled = getDilatedMotionVectorsEnabled();
	observerSetting(m_dilatedMotionVectorsEnabled, "enableDilatedMotionVectors");

	getUpscalingQualityMode = settingsManager.getSettingGetter<UpscaleQualityMode>("upscalingQualityMode");
    setUpscalingQualityMode = settingsManager.getSettingSetter<UpscaleQualityMode>("upscalingQualityMode");
    m_currentUpscalingQualityMode = getUpscalingQualityMode();
	observerSetting(m_currentUpscalingQualityMode, "upscalingQualityMode");

    getWindowResolutionPreset = settingsManager.getSettingGetter<WindowResolutionPreset>(WindowResolutionPresetSettingName);
    setWindowResolutionPreset = settingsManager.getSettingSetter<WindowResolutionPreset>(WindowResolutionPresetSettingName);
    m_currentWindowResolutionPreset = getWindowResolutionPreset();
    observerSetting(m_currentWindowResolutionPreset, WindowResolutionPresetSettingName);

    getCLodLodHeightMode = settingsManager.getSettingGetter<CLodLodHeightMode>(CLodLodHeightModeSettingName);
    setCLodLodHeightMode = settingsManager.getSettingSetter<CLodLodHeightMode>(CLodLodHeightModeSettingName);
    m_currentCLodLodHeightMode = getCLodLodHeightMode();
    observerSetting(m_currentCLodLodHeightMode, CLodLodHeightModeSettingName);

	getUseAsyncCompute = settingsManager.getSettingGetter<bool>("useAsyncCompute");
    setUseAsyncCompute = settingsManager.getSettingSetter<bool>("useAsyncCompute");
    m_useAsyncCompute = getUseAsyncCompute();
	observerSetting(m_useAsyncCompute, "useAsyncCompute");

	getHeavyDebug = settingsManager.getSettingGetter<bool>("heavyDebug");
	setHeavyDebug = settingsManager.getSettingSetter<bool>("heavyDebug");
	m_heavyDebug = getHeavyDebug();
	observerSetting(m_heavyDebug, "heavyDebug");

    getRenderGraphBatchTraceEnabled = settingsManager.getSettingGetter<bool>("renderGraphBatchTraceEnabled");
    setRenderGraphBatchTraceEnabled = settingsManager.getSettingSetter<bool>("renderGraphBatchTraceEnabled");
    m_renderGraphBatchTraceEnabled = getRenderGraphBatchTraceEnabled();
    observerSetting(m_renderGraphBatchTraceEnabled, "renderGraphBatchTraceEnabled");

	getRenderGraphLightweightCompileSummaryEnabled = settingsManager.getSettingGetter<bool>("renderGraphLightweightCompileSummaryEnabled");
	setRenderGraphLightweightCompileSummaryEnabled = settingsManager.getSettingSetter<bool>("renderGraphLightweightCompileSummaryEnabled");
	m_renderGraphLightweightCompileSummaryEnabled = getRenderGraphLightweightCompileSummaryEnabled();
	observerSetting(m_renderGraphLightweightCompileSummaryEnabled, "renderGraphLightweightCompileSummaryEnabled");

    getReshapeTexelAddressing = settingsManager.getSettingGetter<bool>("reshapeTexelAddressing");
    setReshapeTexelAddressing = settingsManager.getSettingSetter<bool>("reshapeTexelAddressing");
    m_reshapeTexelAddressing = getReshapeTexelAddressing();
    observerSetting(m_reshapeTexelAddressing, "reshapeTexelAddressing");

    getAutoAliasMode = settingsManager.getSettingGetter<org::AutoAliasMode>("autoAliasMode");
    setAutoAliasMode = settingsManager.getSettingSetter<org::AutoAliasMode>("autoAliasMode");
    m_autoAliasMode = getAutoAliasMode();
    observerSetting(m_autoAliasMode, "autoAliasMode");

    getAutoAliasPackingStrategy = settingsManager.getSettingGetter<org::AutoAliasPackingStrategy>("autoAliasPackingStrategy");
    setAutoAliasPackingStrategy = settingsManager.getSettingSetter<org::AutoAliasPackingStrategy>("autoAliasPackingStrategy");
    m_autoAliasPackingStrategy = getAutoAliasPackingStrategy();
    observerSetting(m_autoAliasPackingStrategy, "autoAliasPackingStrategy");

    getAutoAliasLogExclusionReasons = settingsManager.getSettingGetter<bool>("autoAliasLogExclusionReasons");
    setAutoAliasLogExclusionReasons = settingsManager.getSettingSetter<bool>("autoAliasLogExclusionReasons");
    m_autoAliasLogExclusionReasons = getAutoAliasLogExclusionReasons();
    observerSetting(m_autoAliasLogExclusionReasons, "autoAliasLogExclusionReasons");

    setAutoAliasBuildDebugData = settingsManager.getSettingSetter<bool>("autoAliasBuildDebugData");

    getAutoAliasPoolRetireIdleFrames = settingsManager.getSettingGetter<uint32_t>("autoAliasPoolRetireIdleFrames");
    setAutoAliasPoolRetireIdleFrames = settingsManager.getSettingSetter<uint32_t>("autoAliasPoolRetireIdleFrames");
    m_autoAliasPoolRetireIdleFrames = getAutoAliasPoolRetireIdleFrames();
    observerSetting(m_autoAliasPoolRetireIdleFrames, "autoAliasPoolRetireIdleFrames");

    getCLodStreamingCpuUploadBudgetRequests = settingsManager.getSettingGetter<uint32_t>(CLodStreamingCpuUploadBudgetSettingName);
    setCLodStreamingCpuUploadBudgetRequests = settingsManager.getSettingSetter<uint32_t>(CLodStreamingCpuUploadBudgetSettingName);
    m_clodStreamingCpuUploadBudgetRequests = getCLodStreamingCpuUploadBudgetRequests();
    observerSetting(m_clodStreamingCpuUploadBudgetRequests, CLodStreamingCpuUploadBudgetSettingName);

    getCLodStreamingEnableDirectStorage = settingsManager.getSettingGetter<bool>(CLodStreamingEnableDirectStorageSettingName);
    setCLodStreamingEnableDirectStorage = settingsManager.getSettingSetter<bool>(CLodStreamingEnableDirectStorageSettingName);
    m_clodStreamingEnableDirectStorage = getCLodStreamingEnableDirectStorage();
    observerSetting(m_clodStreamingEnableDirectStorage, CLodStreamingEnableDirectStorageSettingName);

    getAutoAliasPoolGrowthHeadroom = settingsManager.getSettingGetter<float>("autoAliasPoolGrowthHeadroom");
    setAutoAliasPoolGrowthHeadroom = settingsManager.getSettingSetter<float>("autoAliasPoolGrowthHeadroom");
    m_autoAliasPoolGrowthHeadroom = getAutoAliasPoolGrowthHeadroom();
    observerSetting(m_autoAliasPoolGrowthHeadroom, "autoAliasPoolGrowthHeadroom");

	appendScene = settingsManager.getSettingGetter<std::function<std::shared_ptr<Scene>(std::shared_ptr<Scene>)>>("appendScene")();

    m_meshShadersSupported = DeviceManager::GetInstance().GetMeshShadersSupported();

    // CLod queries
    const auto visBufferTag = RendererECSManager::GetInstance().GetWorld().component<CLodExtensionVisibilityBufferTag>();
    m_telemetryQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodWorkGraphTelemetryBufferTag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    const auto shadowTag = RendererECSManager::GetInstance().GetWorld().component<CLodExtensionShadowTag>();
    m_shadowTelemetryQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodWorkGraphTelemetryBufferTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_reyesTelemetryPhase1Query = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase1Tag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    m_reyesTelemetryPhase2Query = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase2Tag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    m_shadowReyesTelemetryPhase1Query = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase1Tag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_visibleClustersQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<VisibleClustersBufferTag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    m_visibleCounterQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<VisibleClustersCounterTag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    m_shadowVisibleClustersQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<VisibleClustersBufferTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_shadowVisibleCounterQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<VisibleClustersCounterTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_shadowVirtualShadowStatsQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodVirtualShadowStatsTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_shadowVirtualShadowRuntimeStateQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodVirtualShadowRuntimeStateTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();

    const auto alphaTag = RendererECSManager::GetInstance().GetWorld().component<CLodExtensionAlphaBlendTag>();
    m_alphaDeepVisibilityCounterQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodDeepVisibilityCounterTag>()
        .with<CLodExtensionTypeTag>(alphaTag)
        .build();
    m_alphaDeepVisibilityOverflowQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodDeepVisibilityOverflowCounterTag>()
        .with<CLodExtensionTypeTag>(alphaTag)
        .build();
    m_alphaDeepVisibilityStatsQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodDeepVisibilityStatsTag>()
        .with<CLodExtensionTypeTag>(alphaTag)
        .build();
}

static bool PassUsesResourceAdapter(const void* passAndRes, uint64_t resourceId, int passKind) {
    auto checkRequirements = [&](const auto& resources) {
        bool found = false;
        ForEachFrameRequirement(resources, [&](const auto& req) {
            if (req.resourceHandleAndRange.resource.GetGlobalResourceID() == resourceId) {
                found = true;
            }
        });
        return found;
    };
    switch (passKind) {
    case 1: { // Compute
        auto& pr = *reinterpret_cast<const org::RenderGraph::ComputePassAndResources*>(passAndRes);
        return checkRequirements(pr.resources);
    }
    case 2: { // Copy
        auto& pr = *reinterpret_cast<const org::RenderGraph::CopyPassAndResources*>(passAndRes);
        return checkRequirements(pr.resources);
    }
    default: { // Render (0)
        auto& pr = *reinterpret_cast<const org::RenderGraph::RenderPassAndResources*>(passAndRes);
        return checkRequirements(pr.resources);
    }
    }
}

void Menu::Render(const RenderContext& context, rhi::CommandList commandList) {
    m_sceneOverlapStatus = context.sceneOverlapStatus;
	// A platform window/context may exist while the renderer backend is
	// deliberately disabled (currently the default for Vulkan). ImGui 1.92's
	// font atlas update requires a live renderer backend, so do not start a UI
	// frame in the null-backend mode.
	if (m_imguiBackend == rhi::Backend::Null) {
		return;
	}

    if (m_imguiBackend == rhi::Backend::D3D12) {
        ImGui_ImplDX12_NewFrame();
    }
#if BASICRENDERER_HAS_IMGUI_VULKAN
    else if (m_imguiBackend == rhi::Backend::Vulkan) {
        ImGui_ImplVulkan_NewFrame();
    }
#endif
	ImGui_ImplWin32_NewFrame();

	ImGui::NewFrame();
    static bool showRG = false;
    static bool showMemoryIntrospection = false;
    static bool showCLodTelemetry = false;
    static bool showFrameTaskGraph = false;
    static bool showAutoAliasPlanner = false;
    static bool showGpuInstrumentation = false;
    static bool showMaterialTextureStreaming = false;

    const auto formatBytes = [](uint64_t bytes) {
        const double value = static_cast<double>(bytes);
        const double KiB = 1024.0;
        const double MiB = KiB * 1024.0;
        const double GiB = MiB * 1024.0;
        const auto [divisor, suffix] =
            (value >= GiB) ? std::pair{ GiB, "GB" } :
            (value >= MiB) ? std::pair{ MiB, "MB" } :
            (value >= KiB) ? std::pair{ KiB, "KB" } :
            std::pair{ 1.0, "B" };
        return std::format("{:.2f} {}", value / divisor, suffix);
    };
    std::optional<MaterialTextureStreamingStats> materialTextureStreamingStats;
    if (showMaterialTextureStreaming) {
        materialTextureStreamingStats.emplace(context.materialTextureStreamingStats);
    }

    const float fps = ImGui::GetIO().Framerate;
    const float msPerFrame = fps > 0.0f ? (1000.0f / fps) : 0.0f;

    SetCLodWorkGraphTelemetryEnabled(
        (m_menuEnabled && showCLodTelemetry) ||
        m_clodTelemetry.capturePending ||
        m_clodTelemetry.captureStatsPending ||
        m_shadowClodTelemetry.capturePending ||
        m_shadowClodTelemetry.captureStatsPending);

    if (!m_menuEnabled) {
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Menu", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoCollapse);
        ImGui::Checkbox("Enable Menu", &m_menuEnabled);
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", msPerFrame, fps);
        ImGui::End();

		ImGui::Render();
		if (!commandList) return;

        if (m_imguiBackend == rhi::Backend::Null) {
            return;
        }

        if (m_imguiBackend == rhi::Backend::D3D12) {
            if (!g_pd3dSrvDescHeap) {
                return;
            }
            commandList.SetDescriptorHeaps(g_pd3dSrvDescHeap->GetHandle(), std::nullopt);
        }

		rhi::PassBeginInfo beginInfo{};
		rhi::ColorAttachment attchment{};
        attchment.loadOp = rhi::LoadOp::Load;
		attchment.rtv = { context.rtvHeap.GetHandle() , context.frameIndex }; // Index into the swapchain RTV heap
		beginInfo.colors = { &attchment };
        beginInfo.height = context.outputResolution.y;
        beginInfo.width = context.outputResolution.x;

		commandList.BeginPass(beginInfo);

        if (m_imguiBackend == rhi::Backend::D3D12) {
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), rhi::dx12::get_cmd_list(commandList));
        }
#if BASICRENDERER_HAS_IMGUI_VULKAN
        else if (m_imguiBackend == rhi::Backend::Vulkan) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), rhi::vulkan::get_cmd_list(commandList));
        }
#endif
		commandList.EndPass();
        return;
    }

	{
		static float f = 0.0f;
		static int counter = 0;

        const ImGuiIO& io = ImGui::GetIO();
        const float margin = 12.0f;
        const float maxWindowWidth = (std::max)(360.0f, io.DisplaySize.x - margin * 2.0f);
        const float maxWindowHeight = (std::max)(240.0f, io.DisplaySize.y - margin * 2.0f);
        ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2((std::min)(520.0f, maxWindowWidth), (std::min)(720.0f, maxWindowHeight)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 240.0f), ImVec2(maxWindowWidth, maxWindowHeight));

		ImGui::Begin("Renderer Configuration", nullptr, ImGuiWindowFlags_NoCollapse);

        if (ImGui::Checkbox("Image-Based Lighting", &imageBasedLightingEnabled)) {
			setImageBasedLightingEnabled(imageBasedLightingEnabled);
        }
		if (ImGui::Checkbox("Punctual Lighting", &punctualLightingEnabled)) {
			setPunctualLightingEnabled(punctualLightingEnabled);
		}
		if (ImGui::Checkbox("Shadows", &shadowsEnabled)) {
			setShadowsEnabled(shadowsEnabled);
		}
        if (ImGui::Checkbox("Remember Camera Pose", &m_rememberCameraPose)) {
            setRememberCameraPose(m_rememberCameraPose);
        }
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Geometry and Culling")) {
        if (m_meshShadersSupported) {
            if (ImGui::Checkbox("Use Mesh Shaders", &meshShaderEnabled)) {
                setMeshShaderEnabled(meshShaderEnabled);
            }
        }
        else {
            ImGui::Text("Your GPU does not support mesh shaders!");
        }
        if (ImGui::Checkbox("Use Indirect Draws", &indirectDrawsEnabled)) {
            setIndirectDrawsEnabled(indirectDrawsEnabled);
        }
        if (ImGui::Checkbox("Occlusion Culling", &occlusionCulling)) {
            setOcclusionCullingEnabled(occlusionCulling);
        }
        if (ImGui::Checkbox("CLod Frustum Culling", &m_clodFrustumCulling)) {
            setCLodFrustumCulling(m_clodFrustumCulling);
        }
        int clodCullingBackendIndex = static_cast<int>(m_clodCullingBackend);
        if (ImGui::Combo("CLod Culling Backend", &clodCullingBackendIndex, CLodCullingBackendNames, CLodCullingBackendCount)) {
            clodCullingBackendIndex = std::clamp(clodCullingBackendIndex, 0, CLodCullingBackendCount - 1);
            m_clodCullingBackend = static_cast<CLodCullingBackend>(clodCullingBackendIndex);
            setCLodCullingBackend(m_clodCullingBackend);
        }
        if (m_clodCullingBackend == CLodCullingBackend::PureCompute) {
            static constexpr uint32_t kPureComputePhase2ExpansionFactors[] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u };
            static constexpr const char* kPureComputePhase2ExpansionFactorLabels[] = { "1", "2", "4", "8", "16", "32", "64" };
            static constexpr int kPureComputePhase2ExpansionFactorCount =
                static_cast<int>(sizeof(kPureComputePhase2ExpansionFactors) / sizeof(kPureComputePhase2ExpansionFactors[0]));
            m_clodPureComputePhase2ExpansionFactor =
                CLodNormalizePureComputePhase2ExpansionFactor(m_clodPureComputePhase2ExpansionFactor);
            int phase2ExpansionIndex = 0;
            for (int i = 0; i < kPureComputePhase2ExpansionFactorCount; ++i) {
                if (kPureComputePhase2ExpansionFactors[i] == m_clodPureComputePhase2ExpansionFactor) {
                    phase2ExpansionIndex = i;
                    break;
                }
            }
            if (ImGui::Combo(
                    "Pure Compute Phase-2 Bucket Size",
                    &phase2ExpansionIndex,
                    kPureComputePhase2ExpansionFactorLabels,
                    kPureComputePhase2ExpansionFactorCount)) {
                phase2ExpansionIndex = std::clamp(
                    phase2ExpansionIndex,
                    0,
                    kPureComputePhase2ExpansionFactorCount - 1);
                m_clodPureComputePhase2ExpansionFactor = kPureComputePhase2ExpansionFactors[phase2ExpansionIndex];
                setCLodPureComputePhase2ExpansionFactor(m_clodPureComputePhase2ExpansionFactor);
            }
        }
        int visibleClusterCapacityM = static_cast<int>((m_clodVisibleClusterCapacity + 999999u) / 1000000u);
        if (ImGui::SliderInt("CLod Visible Cluster Capacity (M)", &visibleClusterCapacityM, 1, 30)) {
            const uint32_t capacity = static_cast<uint32_t>(std::clamp(visibleClusterCapacityM, 1, 30)) * 1000000u;
            m_clodVisibleClusterCapacity = std::clamp(
                capacity,
                CLodMinVisibleClusterCapacity,
                CLodMaxVisibleClusterCapacity);
            setCLodVisibleClusterCapacity(m_clodVisibleClusterCapacity);
        }
        DrawCLodLodHeightModeCombo();
        int clodCpuUploadBudget = static_cast<int>(std::min<uint32_t>(m_clodStreamingCpuUploadBudgetRequests, 4096u));
        if (ImGui::SliderInt("CLod CPU Upload Budget", &clodCpuUploadBudget, 1, 4096)) {
            m_clodStreamingCpuUploadBudgetRequests = static_cast<uint32_t>(std::max(clodCpuUploadBudget, 1));
            setCLodStreamingCpuUploadBudgetRequests(m_clodStreamingCpuUploadBudgetRequests);
        }
        if (ImGui::Checkbox("CLod Streaming DirectStorage", &m_clodStreamingEnableDirectStorage)) {
            setCLodStreamingEnableDirectStorage(m_clodStreamingEnableDirectStorage);
        }
        }

        if (ImGui::CollapsingHeader("CLod Rasterization and Reyes")) {
        int clodSoftwareRasterModeIndex = static_cast<int>(m_clodSoftwareRasterMode);
        if (ImGui::Combo("Visibility/Alpha SW Raster Mode", &clodSoftwareRasterModeIndex, CLodSoftwareRasterModeNames, CLodSoftwareRasterModeCount)) {
            clodSoftwareRasterModeIndex = std::clamp(clodSoftwareRasterModeIndex, 0, CLodSoftwareRasterModeCount - 1);
            m_clodSoftwareRasterMode = static_cast<CLodSoftwareRasterMode>(clodSoftwareRasterModeIndex);
            setCLodSoftwareRasterMode(m_clodSoftwareRasterMode);
        }
        int clodVSMRasterModeIndex = static_cast<int>(m_clodVSMRasterMode);
        if (ImGui::Combo("VSM Raster Mode", &clodVSMRasterModeIndex, CLodVSMRasterModeNames, CLodVSMRasterModeCount)) {
            clodVSMRasterModeIndex = std::clamp(clodVSMRasterModeIndex, 0, CLodVSMRasterModeCount - 1);
            m_clodVSMRasterMode = static_cast<CLodVSMRasterMode>(clodVSMRasterModeIndex);
            setCLodVSMRasterMode(m_clodVSMRasterMode);
        }
        int clodTransparencyModeIndex = static_cast<int>(m_clodTransparencyMode);
        if (ImGui::Combo("Transparency Mode", &clodTransparencyModeIndex, CLodTransparencyModeNames, CLodTransparencyModeCount)) {
            clodTransparencyModeIndex = std::clamp(clodTransparencyModeIndex, 0, CLodTransparencyModeCount - 1);
            m_clodTransparencyMode = static_cast<CLodTransparencyMode>(clodTransparencyModeIndex);
            setCLodTransparencyMode(m_clodTransparencyMode);
        }
        if (ImGui::Checkbox("Disable Reyes Tessellation / VSM Reyes Routing", &m_clodDisableReyesRasterization)) {
            setCLodDisableReyesRasterization(m_clodDisableReyesRasterization);
        }
        if (ImGui::Checkbox("Reyes Geometric Normal", &m_clodReyesGeometricNormal)) {
            setCLodReyesGeometricNormal(m_clodReyesGeometricNormal);
        }
        if (ImGui::SliderFloat("Reyes Object Normal Map Blend", &m_clodReyesObjectNormalMapBlend, 0.0f, 1.0f, "%.2f")) {
            m_clodReyesObjectNormalMapBlend = std::clamp(m_clodReyesObjectNormalMapBlend, 0.0f, 1.0f);
            setCLodReyesObjectNormalMapBlend(m_clodReyesObjectNormalMapBlend);
        }
        if (ImGui::SliderFloat("Reyes Terrain Normal Blend", &m_clodReyesTerrainNormalBlend, 0.0f, 1.0f, "%.2f")) {
            m_clodReyesTerrainNormalBlend = std::clamp(m_clodReyesTerrainNormalBlend, 0.0f, 1.0f);
            setCLodReyesTerrainNormalBlend(m_clodReyesTerrainNormalBlend);
        }
        if (ImGui::SliderInt("Reyes Terrain Normal Mip Bias", &m_clodReyesTerrainNormalMipBias, 0, static_cast<int>(CLodReyesTerrainNormalMipBiasMax))) {
            m_clodReyesTerrainNormalMipBias = std::clamp(
                m_clodReyesTerrainNormalMipBias,
                0,
                static_cast<int>(CLodReyesTerrainNormalMipBiasMax));
            setCLodReyesTerrainNormalMipBias(static_cast<uint32_t>(m_clodReyesTerrainNormalMipBias));
        }
        if (ImGui::SliderFloat("Reyes Dice Rate Pixels", &m_clodReyesDiceRatePixels, CLodReyesDiceRatePixelsMin, CLodReyesDiceRatePixelsMax, "%.3f")) {
            m_clodReyesDiceRatePixels = std::clamp(
                m_clodReyesDiceRatePixels,
                CLodReyesDiceRatePixelsMin,
                CLodReyesDiceRatePixelsMax);
            setCLodReyesDiceRatePixels(m_clodReyesDiceRatePixels);
        }
        if (ImGui::Checkbox("Reyes AABB Occlusion", &m_clodReyesUseAabbOcclusion)) {
            setCLodReyesUseAabbOcclusion(m_clodReyesUseAabbOcclusion);
        }
        bool forceTraversalDepthRoot = m_clodForceTraversalDepthRoot != CLodForceTraversalDepthRootDisabled;
        if (ImGui::Checkbox("Force CLod Traversal Depth Root", &forceTraversalDepthRoot)) {
            m_clodForceTraversalDepthRoot = forceTraversalDepthRoot ? 0u : CLodForceTraversalDepthRootDisabled;
            setCLodForceTraversalDepthRoot(m_clodForceTraversalDepthRoot);
        }
        if (forceTraversalDepthRoot) {
            int forcedDepth = static_cast<int>(std::min<uint32_t>(m_clodForceTraversalDepthRoot, 255u));
            if (ImGui::InputInt("Forced CLod Depth Root", &forcedDepth)) {
                forcedDepth = std::max(forcedDepth, 0);
                m_clodForceTraversalDepthRoot = static_cast<uint32_t>(forcedDepth);
                setCLodForceTraversalDepthRoot(m_clodForceTraversalDepthRoot);
            }
        }
        if (ImGui::SliderFloat(
                "Shadow Reyes Coarse Target Pages/Triangle",
                &m_clodReyesShadowCoarseTargetPagesPerTriangle,
                CLodReyesShadowCoarseTargetPagesPerTriangleMin,
                CLodReyesShadowCoarseTargetPagesPerTriangleMax,
                "%.2f")) {
            m_clodReyesShadowCoarseTargetPagesPerTriangle = std::clamp(
                m_clodReyesShadowCoarseTargetPagesPerTriangle,
                CLodReyesShadowCoarseTargetPagesPerTriangleMin,
                CLodReyesShadowCoarseTargetPagesPerTriangleMax);
            setCLodReyesShadowCoarseTargetPagesPerTriangle(m_clodReyesShadowCoarseTargetPagesPerTriangle);
        }
        if (m_clodVSMRasterMode == CLodVSMRasterMode::PageJob) {
            int diameterThreshold = static_cast<int>(m_clodPageJobDiameterThreshold);
            if (ImGui::SliderInt("Page-Job Diameter Threshold", &diameterThreshold, 1, 255)) {
                m_clodPageJobDiameterThreshold = static_cast<uint32_t>(std::clamp(diameterThreshold, 1, 255));
                setCLodPageJobDiameterThreshold(m_clodPageJobDiameterThreshold);
            }
            if (ImGui::SliderFloat("Page-Job Sparse Ratio", &m_clodPageJobSparseRatio, 0.0f, 1.0f, "%.2f")) {
                setCLodPageJobSparseRatio(m_clodPageJobSparseRatio);
            }
            int maxPages = static_cast<int>(m_clodPageJobMaxPagesPerCluster);
            if (ImGui::SliderInt("Page-Job Max Pages/Cluster", &maxPages, 1, 255)) {
                m_clodPageJobMaxPagesPerCluster = static_cast<uint32_t>(std::clamp(maxPages, 1, 255));
                setCLodPageJobMaxPagesPerCluster(m_clodPageJobMaxPagesPerCluster);
            }
            int pageJobRecordCapacity = static_cast<int>(m_clodPageJobRecordCapacity);
            if (ImGui::SliderInt("Page-Job Record Capacity", &pageJobRecordCapacity, 1, 8 * 1024 * 1024)) {
                m_clodPageJobRecordCapacity = static_cast<uint32_t>(std::clamp(pageJobRecordCapacity, 1, 8 * 1024 * 1024));
                setCLodPageJobRecordCapacity(m_clodPageJobRecordCapacity);
            }
            if (ImGui::Checkbox("Force All Opaque VSM -> Page-Job", &m_clodPageJobForceAll)) {
                setCLodPageJobForceAll(m_clodPageJobForceAll);
            }
        }
        }

        if (ImGui::CollapsingHeader("Virtual Shadows")) {
        if (ImGui::Checkbox("Disable VSM Page Caching", &m_clodDisableVirtualShadowPageCaching)) {
            setCLodDisableVirtualShadowPageCaching(m_clodDisableVirtualShadowPageCaching);
        }
        int directionalLightClipmaps = static_cast<int>(m_numDirectionalLightCascades);
        if (ImGui::SliderInt("Directional VSM Clipmaps", &directionalLightClipmaps, 1, static_cast<int>(CLodVirtualShadowMaxSupportedClipmapCount))) {
            directionalLightClipmaps = std::clamp(directionalLightClipmaps, 1, static_cast<int>(CLodVirtualShadowMaxSupportedClipmapCount));
            m_numDirectionalLightCascades = static_cast<uint8_t>(directionalLightClipmaps);
            setNumDirectionalLightCascades(m_numDirectionalLightCascades);
        }
        static constexpr uint32_t kDirectionalVsmBackingResolutionOptions[] = {
            CLodVirtualShadowMinBackingResolution,
            CLodVirtualShadowMediumBackingResolution,
            CLodVirtualShadowMaxBackingResolution,
        };
        static constexpr const char* kDirectionalVsmBackingResolutionLabels[] = {
            "4K",
            "8K",
            "16K",
        };
        int directionalVsmBackingResolutionIndex = 0;
        for (int optionIndex = 0; optionIndex < static_cast<int>(std::size(kDirectionalVsmBackingResolutionOptions)); ++optionIndex) {
            if (CLodVirtualShadowSanitizeBackingResolution(m_clodDirectionalVirtualShadowMaxBackingResolution) ==
                kDirectionalVsmBackingResolutionOptions[optionIndex]) {
                directionalVsmBackingResolutionIndex = optionIndex;
                break;
            }
        }
        if (ImGui::Combo(
                "Directional VSM Backing Size",
                &directionalVsmBackingResolutionIndex,
                kDirectionalVsmBackingResolutionLabels,
                static_cast<int>(std::size(kDirectionalVsmBackingResolutionLabels)))) {
            directionalVsmBackingResolutionIndex = std::clamp(
                directionalVsmBackingResolutionIndex,
                0,
                static_cast<int>(std::size(kDirectionalVsmBackingResolutionOptions)) - 1);
            m_clodDirectionalVirtualShadowMaxBackingResolution =
                kDirectionalVsmBackingResolutionOptions[directionalVsmBackingResolutionIndex];
            setCLodDirectionalVirtualShadowMaxBackingResolution(m_clodDirectionalVirtualShadowMaxBackingResolution);

            const uint32_t backingMaxPhysicalPages = CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(
                m_clodDirectionalVirtualShadowMaxBackingResolution);
            if (m_clodDirectionalVirtualShadowMaxPhysicalPages > backingMaxPhysicalPages) {
                m_clodDirectionalVirtualShadowMaxPhysicalPages = backingMaxPhysicalPages;
                setCLodDirectionalVirtualShadowMaxPhysicalPages(m_clodDirectionalVirtualShadowMaxPhysicalPages);
            }
        }
        const uint32_t backingMaxPhysicalPages = CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(
            m_clodDirectionalVirtualShadowMaxBackingResolution);
        int maxPhysicalPages = static_cast<int>(m_clodDirectionalVirtualShadowMaxPhysicalPages);
        if (ImGui::SliderInt("Directional VSM Physical Pages", &maxPhysicalPages, 1, static_cast<int>(backingMaxPhysicalPages))) {
            maxPhysicalPages = std::clamp(maxPhysicalPages, 1, static_cast<int>(backingMaxPhysicalPages));
            m_clodDirectionalVirtualShadowMaxPhysicalPages = static_cast<uint32_t>(maxPhysicalPages);
            setCLodDirectionalVirtualShadowMaxPhysicalPages(m_clodDirectionalVirtualShadowMaxPhysicalPages);
        }
        if (ImGui::Checkbox("Auto Directional VSM LOD Bias", &m_clodDirectionalVirtualShadowAutoLodBias)) {
            setCLodDirectionalVirtualShadowAutoLodBias(m_clodDirectionalVirtualShadowAutoLodBias);
        }
        if (ImGui::SliderFloat("Directional VSM Manual LOD Bias", &m_clodDirectionalVirtualShadowLodBias, -4.0f, 4.0f, "%.2f")) {
            setCLodDirectionalVirtualShadowLodBias(m_clodDirectionalVirtualShadowLodBias);
        }
        if (ImGui::SliderFloat("Directional VSM Auto Bias Scale", &m_clodDirectionalVirtualShadowAutoLodBiasScale, 0.0f, 4.0f, "%.2f")) {
            m_clodDirectionalVirtualShadowAutoLodBiasScale = std::max(m_clodDirectionalVirtualShadowAutoLodBiasScale, 0.0f);
            setCLodDirectionalVirtualShadowAutoLodBiasScale(m_clodDirectionalVirtualShadowAutoLodBiasScale);
        }
        if (ImGui::Checkbox("Invalidate shadows on streaming upgrade", &m_clodDirectionalVirtualShadowPredictiveLodInvalidation)) {
            setCLodDirectionalVirtualShadowPredictiveLodInvalidation(m_clodDirectionalVirtualShadowPredictiveLodInvalidation);
        }
        if (ImGui::SliderFloat("Directional VSM Source Angle", &m_clodDirectionalVirtualShadowSourceAngleDegrees, 0.0f, 10.0f, "%.2f deg")) {
            m_clodDirectionalVirtualShadowSourceAngleDegrees = std::max(m_clodDirectionalVirtualShadowSourceAngleDegrees, 0.0f);
            setCLodDirectionalVirtualShadowSourceAngleDegrees(m_clodDirectionalVirtualShadowSourceAngleDegrees);
        }
        int smrtRayCountDirectional = static_cast<int>(m_clodDirectionalVirtualShadowSmrtRayCountDirectional);
        if (ImGui::SliderInt("Directional VSM SMRT Rays", &smrtRayCountDirectional, 0, 32)) {
            m_clodDirectionalVirtualShadowSmrtRayCountDirectional = static_cast<uint32_t>(std::max(smrtRayCountDirectional, 0));
            setCLodDirectionalVirtualShadowSmrtRayCountDirectional(m_clodDirectionalVirtualShadowSmrtRayCountDirectional);
        }
        int smrtSamplesPerRayDirectional = static_cast<int>(m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional);
        if (ImGui::SliderInt("Directional VSM SMRT Samples/Ray", &smrtSamplesPerRayDirectional, 0, 16)) {
            m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = static_cast<uint32_t>(std::max(smrtSamplesPerRayDirectional, 0));
            setCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional(m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional);
        }
        if (ImGui::SliderFloat(
                "Directional VSM SMRT Max Ray Angle",
                &m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees,
                0.0f,
                15.0f,
                "%.2f deg")) {
            m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees =
                std::max(m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees, 0.0f);
            setCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees(
                m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees);
        }
        if (ImGui::InputFloat(
                "Directional VSM SMRT Ray Length Scale",
                &m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional,
                0.0f,
                0.0f,
                "%.9g",
                ImGuiInputTextFlags_CharsScientific)) {
            m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional =
                std::max(m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional, 0.0f);
            setCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional(
                m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional);
        }
        if (ImGui::InputFloat(
                "Directional VSM SMRT Max Trace Distance (world units)",
                &m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld,
                0.0f,
                0.0f,
                "%.9g",
                ImGuiInputTextFlags_CharsScientific)) {
            m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld =
                std::max(m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld, 1.0f);
            setCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld(
                m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld);
        }
        ImGui::SeparatorText("Adaptive Receiver Screen Trace");
        if (ImGui::Checkbox(
                "Enable VSM Receiver Screen Trace",
                &m_clodDirectionalVirtualShadowReceiverTraceEnabled)) {
            setCLodDirectionalVirtualShadowReceiverTraceEnabled(
                m_clodDirectionalVirtualShadowReceiverTraceEnabled);
        }
        int receiverTraceSampleCount =
            static_cast<int>(m_clodDirectionalVirtualShadowReceiverTraceSampleCount);
        if (ImGui::SliderInt(
                "VSM Receiver Trace Samples",
                &receiverTraceSampleCount,
                1,
                32)) {
            m_clodDirectionalVirtualShadowReceiverTraceSampleCount =
                static_cast<uint32_t>(std::clamp(receiverTraceSampleCount, 1, 32));
            setCLodDirectionalVirtualShadowReceiverTraceSampleCount(
                m_clodDirectionalVirtualShadowReceiverTraceSampleCount);
        }
        if (ImGui::InputFloat(
                "VSM Receiver Trace Max Distance (world units)",
                &m_clodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld,
                0.0f,
                0.0f,
                "%.9g",
                ImGuiInputTextFlags_CharsScientific)) {
            m_clodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld =
                std::max(m_clodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld, 1.0f);
            setCLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld(
                m_clodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld);
        }
        if (ImGui::SliderFloat(
                "VSM Receiver Trace Uncertainty",
                &m_clodDirectionalVirtualShadowReceiverTraceUncertaintyScale,
                0.25f,
                8.0f,
                "%.2f pixels")) {
            m_clodDirectionalVirtualShadowReceiverTraceUncertaintyScale =
                std::max(m_clodDirectionalVirtualShadowReceiverTraceUncertaintyScale, 0.0f);
            setCLodDirectionalVirtualShadowReceiverTraceUncertaintyScale(
                m_clodDirectionalVirtualShadowReceiverTraceUncertaintyScale);
        }
        if (ImGui::SliderFloat(
                "VSM Trace Depth Safety",
                &m_clodDirectionalVirtualShadowReceiverTraceDepthSafetyScale,
                0.0f,
                16.0f,
                "%.2f pixels")) {
            m_clodDirectionalVirtualShadowReceiverTraceDepthSafetyScale =
                std::max(m_clodDirectionalVirtualShadowReceiverTraceDepthSafetyScale, 0.0f);
            setCLodDirectionalVirtualShadowReceiverTraceDepthSafetyScale(
                m_clodDirectionalVirtualShadowReceiverTraceDepthSafetyScale);
        }
        ImGui::TextDisabled(
            "Receiver escape is tested independently for every randomized SMRT ray.");
        const CLodVirtualShadowResolutionConfig virtualShadowConfig =
            CLodVirtualShadowBuildRuntimeResolutionConfig();
        const float budgetDirectionalLodBias = m_clodDirectionalVirtualShadowAutoLodBias
            ? CLodVirtualShadowAutomaticDirectionalLodBiasFromBudget(
                virtualShadowConfig.maxPhysicalPages,
                m_clodDirectionalVirtualShadowAutoLodBiasScale)
            : 0.0f;
        ImGui::Text(
            "Directional VSM Virtual Space: %u x %u pages (%u texels/page, fixed 16K)",
            CLodVirtualShadowFixedVirtualPageCountPerAxis,
            CLodVirtualShadowFixedVirtualPageCountPerAxis,
            CLodVirtualShadowPhysicalPageSize);
        ImGui::Text(
            "Directional VSM Backing Cap: %u x %u texels (%u pages max)",
            CLodVirtualShadowSanitizeBackingResolution(m_clodDirectionalVirtualShadowMaxBackingResolution),
            CLodVirtualShadowSanitizeBackingResolution(m_clodDirectionalVirtualShadowMaxBackingResolution),
            backingMaxPhysicalPages);
        ImGui::Text(
            "Directional VSM Physical Atlas: %u x %u pages (%u total)",
            virtualShadowConfig.physicalAtlasPagesWide,
            virtualShadowConfig.physicalAtlasPagesHigh,
            virtualShadowConfig.maxPhysicalPages);
        ImGui::Text(
            "Directional VSM Bias: manual=%.2f budgetBase=%.2f configured=%.2f",
            m_clodDirectionalVirtualShadowLodBias,
            budgetDirectionalLodBias,
            virtualShadowConfig.directionalLodBias);
        ImGui::Text(
            "Directional VSM SMRT: angle=%.2f deg rays=%u samples/ray=%u maxRayAngle=%.2f deg rayLengthScale=%.2f",
            m_clodDirectionalVirtualShadowSourceAngleDegrees,
            m_clodDirectionalVirtualShadowSmrtRayCountDirectional,
            m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional,
            m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees,
            m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional);
        ImGui::Text(
            "Directional VSM SMRT distance: %.1f base, %.1f effective world units",
            m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld,
            m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld *
                m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional);
        ImGui::Text(
            "Receiver trace: %s, %u samples, %.1f world-unit cap",
            m_clodDirectionalVirtualShadowReceiverTraceEnabled ? "enabled" : "disabled",
            m_clodDirectionalVirtualShadowReceiverTraceSampleCount,
            m_clodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld);
        if (ImGui::SliderFloat(
                "Directional Shadow Distance Lower Bound",
                &m_directionalShadowDistanceLowerBound,
                1.0f,
                1000000.0f,
                "%.1f",
                ImGuiSliderFlags_Logarithmic)) {
            m_directionalShadowDistanceLowerBound = std::max(m_directionalShadowDistanceLowerBound, 1.0f);
            setDirectionalShadowDistanceLowerBound(m_directionalShadowDistanceLowerBound);
        }
        }

        if (ImGui::CollapsingHeader("Terrain and Materials")) {
        if (ImGui::Checkbox("Clustered Lighting", &clusteredLighting)) {
			setClusteredLightingEnabled(clusteredLighting);
        }
        if (ImGui::Checkbox("Visibility Rendering", &m_visibilityRenderingEnabled)) {
            setVisibilityRenderingEnabled(m_visibilityRenderingEnabled);
        }
        if (ImGui::Checkbox("Terrain Region Material Evaluation", &m_terrainRegionMaterialEvaluationEnabled)) {
            setTerrainRegionMaterialEvaluationEnabled(m_terrainRegionMaterialEvaluationEnabled);
        }
        if (ImGui::Checkbox("Terrain Runtime Virtual Texture", &m_terrainRvtEnabled)) {
            setTerrainRvtEnabled(m_terrainRvtEnabled);
        }
        if (ImGui::Checkbox("Force Terrain RVT Direct Fallback", &m_forceDirectTerrainRvtFallback)) {
            setForceDirectTerrainRvtFallback(m_forceDirectTerrainRvtFallback);
        }
        if (ImGui::Checkbox("Terrain RVT Telemetry", &m_terrainRvtTelemetryDebug)) {
            setTerrainRvtTelemetryDebug(m_terrainRvtTelemetryDebug);
        }
        if (ImGui::SliderInt("Terrain RVT Debug View", &m_terrainRvtDebugView, 0, 4)) {
            setTerrainRvtDebugView(static_cast<uint32_t>(std::max(0, m_terrainRvtDebugView)));
        }
        if (ImGui::SliderInt("Terrain RVT Page Size", &m_terrainRvtPageSize, 16, 512)) {
            setTerrainRvtPageSize(static_cast<uint32_t>(std::clamp(m_terrainRvtPageSize, 16, 512)));
        }
        if (ImGui::SliderInt("Terrain RVT Border Texels", &m_terrainRvtBorderTexels, 0, 16)) {
            setTerrainRvtBorderTexels(static_cast<uint32_t>(std::clamp(m_terrainRvtBorderTexels, 0, 16)));
        }
        if (ImGui::SliderInt("Terrain RVT Clipmaps", &m_terrainRvtMipCount, 1, 24)) {
            setTerrainRvtMipCount(static_cast<uint32_t>(std::clamp(m_terrainRvtMipCount, 1, 24)));
        }
        if (ImGui::SliderFloat("Terrain RVT Mip Offset", &m_terrainRvtMipOffset, -4.0f, 4.0f, "%.2f")) {
            setTerrainRvtMipOffset(std::clamp(m_terrainRvtMipOffset, -8.0f, 8.0f));
        }
        if (ImGui::SliderFloat("Terrain RVT Source Texels/World", &m_terrainRvtSourceTexelsPerWorld, 1.0f, 128.0f, "%.2f")) {
            setTerrainRvtSourceTexelsPerWorld(std::max(0.001f, m_terrainRvtSourceTexelsPerWorld));
        }
        if (ImGui::SliderInt("Terrain RVT Physical Atlas Pages Wide", &m_terrainRvtPhysicalAtlasPagesWide, 1, 128)) {
            setTerrainRvtPhysicalAtlasPagesWide(static_cast<uint32_t>(std::clamp(m_terrainRvtPhysicalAtlasPagesWide, 1, 128)));
        }
        if (ImGui::SliderInt("Terrain RVT Physical Atlas Pages High", &m_terrainRvtPhysicalAtlasPagesHigh, 1, 128)) {
            setTerrainRvtPhysicalAtlasPagesHigh(static_cast<uint32_t>(std::clamp(m_terrainRvtPhysicalAtlasPagesHigh, 1, 128)));
        }
        if (ImGui::SliderInt("Terrain RVT Physical Atlas Pools", &m_terrainRvtPhysicalAtlasPoolCount, 1, 8)) {
            setTerrainRvtPhysicalAtlasPoolCount(static_cast<uint32_t>(std::clamp(m_terrainRvtPhysicalAtlasPoolCount, 1, 8)));
        }
        if (ImGui::Checkbox("Terrain Stochastic Sampling", &m_terrainStochasticSamplingEnabled)) {
            setTerrainStochasticSamplingEnabled(m_terrainStochasticSamplingEnabled);
        }
        if (ImGui::Checkbox("Terrain Stochastic Diffuse", &m_terrainStochasticDiffuseSamplingEnabled)) {
            setTerrainStochasticDiffuseSamplingEnabled(m_terrainStochasticDiffuseSamplingEnabled);
        }
        if (ImGui::Checkbox("Terrain Stochastic Normals", &m_terrainStochasticNormalSamplingEnabled)) {
            setTerrainStochasticNormalSamplingEnabled(m_terrainStochasticNormalSamplingEnabled);
        }
        if (ImGui::Checkbox("Terrain Derivative Normal Blend", &m_terrainStochasticDerivativeNormalSamplingEnabled)) {
            setTerrainStochasticDerivativeNormalSamplingEnabled(m_terrainStochasticDerivativeNormalSamplingEnabled);
        }
        if (ImGui::SliderFloat("Terrain Stochastic Blend Curve", &m_terrainStochasticBlendCurve, 0.0f, 1.0f, "%.2f")) {
            setTerrainStochasticBlendCurve(m_terrainStochasticBlendCurve);
        }
        if (ImGui::Checkbox("Terrain Gaussian Stochastic Variant", &m_terrainGaussianStochasticSamplingEnabled)) {
            setTerrainGaussianStochasticSamplingEnabled(m_terrainGaussianStochasticSamplingEnabled);
        }
        if (ImGui::Checkbox("Parallax Occlusion Mapping", &m_parallaxOcclusionMappingEnabled)) {
            setParallaxOcclusionMappingEnabled(m_parallaxOcclusionMappingEnabled);
        }
        if (ImGui::Checkbox("Terrain Parallax Occlusion Mapping", &m_terrainParallaxOcclusionMappingEnabled)) {
            setTerrainParallaxOcclusionMappingEnabled(m_terrainParallaxOcclusionMappingEnabled);
        }
        if (ImGui::Checkbox("Terrain Reyes Displacement", &m_terrainReyesDisplacementEnabled)) {
            setTerrainReyesDisplacementEnabled(m_terrainReyesDisplacementEnabled);
        }
        if (ImGui::SliderFloat("Terrain Reyes Displacement Global Scale", &m_terrainReyesDisplacementScale, 0.0f, 16.0f, "%.2f")) {
            m_terrainReyesDisplacementScale = std::max(0.0f, m_terrainReyesDisplacementScale);
            setTerrainReyesDisplacementScale(m_terrainReyesDisplacementScale);
        }
        if (ImGui::SliderFloat("Object Reyes Displacement Global Scale", &m_objectReyesDisplacementScale, 0.0f, 1000.0f, "%.2f")) {
            m_objectReyesDisplacementScale = std::max(0.0f, m_objectReyesDisplacementScale);
            setObjectReyesDisplacementScale(m_objectReyesDisplacementScale);
        }
        if (ImGui::SliderFloat("Terrain Parallax Height Scale", &m_terrainParallaxHeightScale, 0.0f, 0.20f, "%.3f")) {
            setTerrainParallaxHeightScale(m_terrainParallaxHeightScale);
        }
        if (ImGui::SliderFloat("Object Parallax Height Scale", &m_objectParallaxHeightScale, 0.0f, 16.0f, "%.2f")) {
            m_objectParallaxHeightScale = std::max(0.0f, m_objectParallaxHeightScale);
            setObjectParallaxHeightScale(m_objectParallaxHeightScale);
        }
        int terrainParallaxMaxSteps = static_cast<int>(m_terrainParallaxMaxSteps);
        if (ImGui::SliderInt("Terrain Parallax Max Steps", &terrainParallaxMaxSteps, 4, 32)) {
            m_terrainParallaxMaxSteps = static_cast<uint32_t>(std::clamp(terrainParallaxMaxSteps, 4, 32));
            setTerrainParallaxMaxSteps(m_terrainParallaxMaxSteps);
        }
        if (ImGui::SliderFloat("Height Fade Start", &m_terrainParallaxFadeStartDistance, 0.0f, 32768.0f, "%.0f")) {
            m_terrainParallaxFadeStartDistance = std::max(0.0f, m_terrainParallaxFadeStartDistance);
            setTerrainParallaxFadeStartDistance(m_terrainParallaxFadeStartDistance);
        }
        if (ImGui::SliderFloat("Height Fade End", &m_terrainParallaxFadeEndDistance, 0.0f, 65536.0f, "%.0f")) {
            m_terrainParallaxFadeEndDistance = std::max(0.0f, m_terrainParallaxFadeEndDistance);
            setTerrainParallaxFadeEndDistance(m_terrainParallaxFadeEndDistance);
        }
        }

        if (ImGui::CollapsingHeader("Procedural Wind")) {
            if (ImGui::SliderFloat("Wind Effect Distance", &m_proceduralWindEffectDistance, 0.0f, 65536.0f, "%.0f units")) {
                m_proceduralWindEffectDistance = std::clamp(m_proceduralWindEffectDistance, 0.0f, 65536.0f);
                setProceduralWindEffectDistance(m_proceduralWindEffectDistance);
                const float innerRadius = std::min(
                    SettingsManager::GetInstance().getSettingGetter<float>(ProceduralWindInnerRadiusSettingName)(),
                    m_proceduralWindEffectDistance);
                setProceduralWindInnerRadius(innerRadius);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Maximum world-space distance for DynamicWind. VSM skinned cascade count is derived from this distance.");
            }
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputFloat("Tree Wind Scale", &m_proceduralWindDisplacementScale, 0.1f, 1.0f, "%.2f")) {
                m_proceduralWindDisplacementScale = std::clamp(m_proceduralWindDisplacementScale, 0.0f, 100.0f);
                setProceduralWindDisplacementScale(m_proceduralWindDisplacementScale);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scales tree bend and torsion before each profile's maximum-angle clamp.");
            }
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputFloat("Grass Lean Scale", &m_proceduralWindGrassDisplacementScale, 0.1f, 1.0f, "%.2f")) {
                m_proceduralWindGrassDisplacementScale = std::clamp(m_proceduralWindGrassDisplacementScale, 0.0f, 100.0f);
                setProceduralWindGrassDisplacementScale(m_proceduralWindGrassDisplacementScale);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scales the sustained DynamicWind lean applied to grass cards.");
            }
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputFloat("Grass Oscillation Scale", &m_proceduralWindGrassOscillationScale, 0.1f, 1.0f, "%.2f")) {
                m_proceduralWindGrassOscillationScale = std::clamp(m_proceduralWindGrassOscillationScale, 0.0f, 100.0f);
                setProceduralWindGrassOscillationScale(m_proceduralWindGrassOscillationScale);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scales the animated flutter amplitude applied to grass cards.");
            }
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputFloat("Grass Flutter Frequency", &m_proceduralWindGrassFlutterFrequency, 0.1f, 1.0f, "%.2f")) {
                m_proceduralWindGrassFlutterFrequency = std::clamp(m_proceduralWindGrassFlutterFrequency, 0.0f, 100.0f);
                setProceduralWindGrassFlutterFrequency(m_proceduralWindGrassFlutterFrequency);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scales the frequency of multi-octave grass flutter noise.");
            }
        }

        if (ImGui::CollapsingHeader("Post Processing and Ray Tracing")) {
		if (ImGui::Checkbox("Enable GTAO", &m_gtaoEnabled)) {
			setGTAOEnabled(m_gtaoEnabled);
		}
		if (ImGui::Checkbox("Enable Bloom", &m_bloomEnabled)) {
			setBloomEnabled(m_bloomEnabled);
		}
        if (ImGui::Checkbox("Enable Screen Space Reflections", &m_screenSpaceReflectionsEnabled)) {
            setScreenSpaceReflectionsEnabled(m_screenSpaceReflectionsEnabled);
		}
        const bool clodRtSupported = DeviceManager::GetInstance().GetCLodRayTracingSupported();
        if (!clodRtSupported) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Enable Ray Traced Reflections", &m_rayTracedReflectionsEnabled)) {
            setRayTracedReflectionsEnabled(m_rayTracedReflectionsEnabled);
        }
        if (!clodRtSupported) {
            ImGui::EndDisabled();
        }
        const RayTracingFeatureInfo& rtFeatures = DeviceManager::GetInstance().GetRayTracingFeatures();
        ImGui::TextDisabled(
            "RT: pipeline %s, AS %s, GPU RTAS %s, cluster AS %s, max cluster %u verts/%u tris",
            rtFeatures.pipeline ? "yes" : "no",
            rtFeatures.accelerationStructure ? "yes" : "no",
            rtFeatures.gpuRtasOperations ? "yes" : "no",
            rtFeatures.clusterAccelerationStructure ? "yes" : "no",
            rtFeatures.maxClusterVertices,
            rtFeatures.maxClusterTriangles);
        if (context.clodRayTracingSystem) {
            const auto& clodRtStats = context.clodRayTracingSystem->GetStats();
            ImGui::TextDisabled(
                "CLod RT: groups %u pages %u clusters %u, GPU %s, CLAS %s, BLAS %s, TLAS %s, pipeline %s, trace %s",
                clodRtStats.residentGroups,
                clodRtStats.residentPages,
                clodRtStats.buildableClusters,
                clodRtStats.gpuResourcesReady ? "yes" : "no",
                clodRtStats.clasBuildSubmitted ? "yes" : "no",
                clodRtStats.blasBuildSubmitted ? "yes" : "no",
                clodRtStats.tlasBuildSubmitted ? "yes" : "no",
                clodRtStats.rayPipelineReady ? "yes" : "no",
                clodRtStats.traceRaysSubmitted ? "yes" : "no");
        }
        if (ImGui::Checkbox("Enable Jitter", &m_jitterEnabled)) {
            setJitterEnabled(m_jitterEnabled);
        }
        }

        if (ImGui::CollapsingHeader("Display and Assets")) {
        DrawWindowResolutionCombo();
        DrawUpscalingCombo();
        if (ImGui::Checkbox("Dilated Motion Vectors", &m_dilatedMotionVectorsEnabled)) {
            setDilatedMotionVectorsEnabled(m_dilatedMotionVectorsEnabled);
        }
        DrawUpscalingQualityCombo();
        DrawTonemapTypeDropdown();

        DrawEnvironmentsDropdown();
        DrawBrowseButton(environmentsDir.wstring());
		DrawOutputTypeDropdown();
        DrawLoadModelButton();
        }

        if (ImGui::CollapsingHeader("Statistics and Debug Windows")) {
        if (ImGui::Checkbox("Collect Pass Statistics", &m_collectPassStatistics)) {
            setCollectPassStatistics(m_collectPassStatistics);
        }
		if (ImGui::Checkbox("Collect Pipeline Statistics", &m_collectPipelineStatistics)) {
			setCollectPipelineStatistics(m_collectPipelineStatistics);
		}
        ImGui::Checkbox("Render Graph Inspector", &showRG);
        ImGui::Checkbox("Memory introspection", &showMemoryIntrospection);
        ImGui::Checkbox("CLod telemetry", &showCLodTelemetry);
        ImGui::Checkbox("CPU frame task graph", &showFrameTaskGraph);
        ImGui::Checkbox("Auto Alias Planner", &showAutoAliasPlanner);
        if (setAutoAliasBuildDebugData) {
            setAutoAliasBuildDebugData(showAutoAliasPlanner);
        }
        ImGui::Checkbox("GPU instrumentation", &showGpuInstrumentation);
        ImGui::Checkbox("Material texture streaming", &showMaterialTextureStreaming);
        }

        ImGui::Separator();
		if (ImGui::Checkbox("Wireframe", &wireframeEnabled)) {
			setWireframeEnabled(wireframeEnabled);
		}
        if (ImGui::Checkbox("Uncap Framerate", &allowTearing)) {
			setAllowTearing(allowTearing);
        }
		if (ImGui::Checkbox("Draw Bounding Spheres", &drawBoundingSpheres)) {
			setDrawBoundingSpheres(drawBoundingSpheres);
		}
		if (ImGui::Checkbox("Use Async Compute", &m_useAsyncCompute)) {
			setUseAsyncCompute(m_useAsyncCompute);
		}
		if (ImGui::Checkbox("Heavy Debug (1 pass/batch + GPU drain)", &m_heavyDebug)) {
			setHeavyDebug(m_heavyDebug);
		}
        if (ImGui::Checkbox("Render Graph Batch Trace", &m_renderGraphBatchTraceEnabled)) {
            setRenderGraphBatchTraceEnabled(m_renderGraphBatchTraceEnabled);
        }
		if (ImGui::Checkbox("Render Graph Compile Summary", &m_renderGraphLightweightCompileSummaryEnabled)) {
			setRenderGraphLightweightCompileSummaryEnabled(m_renderGraphLightweightCompileSummaryEnabled);
		}
        if (ImGui::Checkbox("ReShape texel addressing (requires recreate)", &m_reshapeTexelAddressing)) {
            setReshapeTexelAddressing(m_reshapeTexelAddressing);
        }
        std::string memoryString = "Memory usage: unavailable";
        const double KiB = 1024.0;
        const double MiB = KiB * 1024.0;
        const double GiB = MiB * 1024.0;
        if (m_renderGraph) {
            if (auto* statisticsService = m_renderGraph->GetStatisticsService()) {
                const auto memoryBudgetStats = statisticsService->GetMemoryBudgetStats();
                if (memoryBudgetStats.valid) {
                    const auto usage = static_cast<double>(memoryBudgetStats.usageBytes);

                    const auto [div, suffix] =
                        (usage >= GiB) ? std::pair{ GiB, "GB" } :
                        (usage >= MiB) ? std::pair{ MiB, "MB" } :
                        (usage >= KiB) ? std::pair{ KiB, "KB" } :
                        std::pair{ 1.0, "B" };

                    memoryString = std::format("Memory usage: {:.2f} {} / {:.2f} GB",
                        usage / div, suffix,
                        static_cast<double>(memoryBudgetStats.budgetBytes) / GiB);
                }
            }
        }

        ImGui::Text(memoryString.c_str());
        ImGui::Text("Render Resolution: %d x %d | Output Resolution: %d x %d", context.renderResolution.x, context.renderResolution.y, context.outputResolution.x, context.outputResolution.y);
        ImGui::Checkbox("Enable Menu", &m_menuEnabled);
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", msPerFrame, fps);
		ImGui::End();
	}
	if (showMemoryIntrospection) {
        static org::ui::MemoryIntrospectionWidget g_memWidget;

        std::vector<org::memory::ResourceMemoryRecord> memoryRecords;
    if (m_renderGraph) {
        m_renderGraph->GetMemorySnapshotProvider().BuildSnapshot(memoryRecords);
    }

        org::ui::MemorySnapshot snap;
        PerResourceMemIndex memIndex;
        BuildMemorySnapshotFromRecords(snap, memoryRecords, &memIndex);

		org::ui::FrameGraphSnapshot fgSnap;
        if (m_renderGraph) {
            m_renderGraph->BuildMemoryIntrospectionFrameGraphSnapshot(fgSnap, memoryRecords);
        }


        ImGui::Begin("Memory Introspection", nullptr);
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsedSeconds = now - m_startTime;
        uint64_t totalBytes = snap.totalBytes;
        g_memWidget.PushFrameSample(elapsedSeconds.count(), totalBytes);
        bool open = true;
        g_memWidget.Draw(&open, &snap, &fgSnap);
		ImGui::End();
	}

    if (m_renderGraph) {
        static const bool logMemoryAccounting = [] {
            char* value = nullptr;
            size_t valueLength = 0;
            const bool result = _dupenv_s(&value, &valueLength, "SARP_MEMORY_INTROSPECTION_LOG") == 0 &&
                value != nullptr && valueLength > 1 && value[0] != '0';
            std::free(value);
            return result;
        }();
        static uint64_t memoryAccountingFrame = 0;
        ++memoryAccountingFrame;
        if (logMemoryAccounting && (memoryAccountingFrame == 1 || memoryAccountingFrame % 120 == 0)) {
            std::vector<org::memory::ResourceMemoryRecord> diagnosticRecords;
            m_renderGraph->GetMemorySnapshotProvider().BuildSnapshot(diagnosticRecords);
            uint64_t introspectedBytes = 0;
            uint64_t zeroSizedRecords = 0;
            uint64_t recordsWithoutType = 0;
            std::unordered_map<std::string, uint64_t> diagnosticCategories;
            struct ResourceAllocationGroup {
                uint64_t bytes = 0;
                uint64_t count = 0;
                org::memory::ResourceMemoryRecord representative;
            };
            std::unordered_map<std::string, ResourceAllocationGroup> allocationGroups;
            for (const auto& record : diagnosticRecords) {
                introspectedBytes += record.bytes;
                zeroSizedRecords += record.bytes == 0 ? 1u : 0u;
                recordsWithoutType += record.resourceType == rhi::ResourceType::Unknown ? 1u : 0u;
                const std::string category = std::string(MajorCategory(record.resourceType)) + "/" +
                    (record.usage.empty() ? "Unspecified" : record.usage);
                diagnosticCategories[category] += record.bytes;
                const std::string groupKey = std::format(
                    "{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}",
                    static_cast<uint32_t>(record.resourceType),
                    record.usage,
                    record.resourceName,
                    record.identifier,
                    record.bytes,
                    record.width,
                    record.height,
                    record.mipLevels,
                    record.arraySize,
                    static_cast<uint32_t>(record.format),
                    record.aliased);
                auto& group = allocationGroups[groupKey];
                group.bytes += record.bytes;
                ++group.count;
                if (group.count == 1u) {
                    group.representative = record;
                }
            }

            const auto budget = m_renderGraph->GetStatisticsService()->GetMemoryBudgetStats();
            spdlog::info(
                "Memory diagnostics summary: frame={} dxgi_usage={} introspected={} dxgi_minus_introspected={} "
                "records={} zero_sized_records={} unknown_type_records={}",
                memoryAccountingFrame,
                budget.usageBytes,
                introspectedBytes,
                budget.usageBytes > introspectedBytes ? budget.usageBytes - introspectedBytes : 0,
                diagnosticRecords.size(),
                zeroSizedRecords,
                recordsWithoutType);

            std::vector<std::pair<std::string, uint64_t>> sortedDiagnosticCategories(
                diagnosticCategories.begin(), diagnosticCategories.end());
            std::sort(sortedDiagnosticCategories.begin(), sortedDiagnosticCategories.end(),
                [](const auto& left, const auto& right) { return left.second > right.second; });
            std::string categorySummary;
            for (const auto& [category, bytes] : sortedDiagnosticCategories) {
                if (!categorySummary.empty()) {
                    categorySummary += "; ";
                }
                categorySummary += std::format("{}={}", category, bytes);
            }
            spdlog::info("Memory diagnostics categories: frame={} {}", memoryAccountingFrame, categorySummary);

            std::vector<ResourceAllocationGroup> sortedAllocationGroups;
            sortedAllocationGroups.reserve(allocationGroups.size());
            for (auto& [_, group] : allocationGroups) {
                sortedAllocationGroups.push_back(std::move(group));
            }
            std::sort(sortedAllocationGroups.begin(), sortedAllocationGroups.end(),
                [](const auto& left, const auto& right) { return left.bytes > right.bytes; });
            for (size_t rank = 0; rank < std::min<size_t>(sortedAllocationGroups.size(), 32u); ++rank) {
                const auto& group = sortedAllocationGroups[rank];
                const auto& record = group.representative;
                spdlog::info(
                    "Memory diagnostics resource_group: frame={} rank={} total_bytes={} allocation_bytes={} count={} "
                    "type={} usage='{}' name='{}' identifier='{}' width={} height={} mips={} array={} format={} aliased={}",
                    memoryAccountingFrame,
                    rank,
                    group.bytes,
                    record.bytes,
                    group.count,
                    static_cast<uint32_t>(record.resourceType),
                    record.usage,
                    record.resourceName,
                    record.identifier,
                    record.width,
                    record.height,
                    record.mipLevels,
                    record.arraySize,
                    static_cast<uint32_t>(record.format),
                    record.aliased);
            }
        }
    }

    if (showMaterialTextureStreaming) {
        ImGui::Begin("Material Texture Streaming", &showMaterialTextureStreaming);
        if (materialTextureStreamingStats.has_value()) {
            const auto& stats = *materialTextureStreamingStats;
            ImGui::Text("Active material textures: %u", stats.uniqueMaterialTextureCount);
            ImGui::Text(
                "Streamable / enabled: %u / %u",
                stats.uniqueStreamableTextureCount,
                stats.uniqueStreamingEnabledTextureCount);
            ImGui::Text(
                "Full-res resident: %u total, %u streamable",
                stats.fullResolutionResidentTextureCount,
                stats.streamableFullResolutionResidentTextureCount);
            ImGui::Text("Pending reloads: %u", stats.pendingReloadTextureCount);
            ImGui::Text("Resident bytes: %s", formatBytes(stats.totalResidentBytes).c_str());
            ImGui::Text("Streamable resident bytes: %s", formatBytes(stats.streamableResidentBytes).c_str());

            if (!stats.residentTopMipHistogram.empty()) {
                std::vector<float> histogramValues;
                histogramValues.reserve(stats.residentTopMipHistogram.size());
                for (uint32_t count : stats.residentTopMipHistogram) {
                    histogramValues.push_back(static_cast<float>(count));
                }

                ImGui::SeparatorText("Resident top mip histogram");
                ImGui::PlotHistogram(
                    "##MaterialTextureResidentTopMipHistogram",
                    histogramValues.data(),
                    static_cast<int>(histogramValues.size()),
                    0,
                    nullptr,
                    0.0f,
                    *std::max_element(histogramValues.begin(), histogramValues.end()) + 1.0f,
                    ImVec2(420.0f, 180.0f));

                for (size_t mip = 0; mip < stats.residentTopMipHistogram.size(); ++mip) {
                    ImGui::Text("Top mip %zu: %u", mip, stats.residentTopMipHistogram[mip]);
                }
            }
            else {
                ImGui::TextUnformatted("No active material textures tracked.");
            }
        }
        else {
            ImGui::TextUnformatted("MaterialManager unavailable.");
        }
        ImGui::End();
    }

    {
		const bool sceneGraphVisible = ImGui::Begin("Scene Graph", nullptr);
		if (sceneGraphVisible) {
			DisplaySceneGraph();
		}
		ImGui::End();

		DisplaySelectedNode();

        DrawPassTimingWindow();
    }
    
    if (showRG) {
		ImGui::Begin("Render Graph Inspector", nullptr);
		org::RGInspectorOptions opts;
        if ((m_imguiBackend == rhi::Backend::D3D12 || m_imguiBackend == rhi::Backend::Vulkan) && g_pd3dSrvDescHeap) {
            opts.imguiAllocDescriptor = [this]() { return AllocateImGuiDescriptor(); };
            opts.imguiFreeDescriptor = [this](uint32_t idx) { FreeImGuiDescriptor(idx); };
            opts.imguiGpuHandle = [this](uint32_t idx) { return GetImGuiGpuDescriptorHandle(idx); };
            opts.imguiHeapHandle = GetImGuiHeapHandle();
            opts.device = []() { return DeviceManager::GetInstance().GetDevice(); };
            opts.graphicsQueue = []() { return DeviceManager::GetInstance().GetGraphicsQueue(); };
        }
        org::RGInspector::Show(m_renderGraph->GetBatches(),
            m_renderGraph->GetQueueRegistry(),
            PassUsesResourceAdapter,
            [this](uint64_t resourceId) -> std::string {
                if (!m_renderGraph) return {};
                auto resource = m_renderGraph->GetResourceByID(resourceId);
                if (!resource) return {};
                return resource->GetName();
            },
            [this](uint64_t resourceId) -> org::Resource* {
                if (!m_renderGraph) return nullptr;
                auto resource = m_renderGraph->GetResourceByID(resourceId);
                return resource ? resource.get() : nullptr;
            },
            [this](const std::string& passName, org::Resource* resource, const org::RangeSpec& range, org::ReadbackCaptureCallback callback) {
                if (!m_renderGraph) {
                    return;
                }
                if (auto* readbackService = m_renderGraph->GetReadbackService()) {
                    readbackService->RequestReadbackCapture(passName, resource, range, std::move(callback));
                }
            },
            opts);
        ImGui::End();

    }

    if (showCLodTelemetry) {
        DrawCLodTelemetryWindow();
    }

    if (showGpuInstrumentation) {
        static rhi::debug::InstrumentationWidget g_gpuInstrumentationWidget;
        g_gpuInstrumentationWidget.Draw(DeviceManager::GetInstance().GetDevice(), &showGpuInstrumentation);
    }

    if (showFrameTaskGraph) {
        DrawFrameTaskGraphWindow();
    }

    if (showAutoAliasPlanner) {
        DrawAutoAliasPlannerWindow();
    }

	// Rendering
	ImGui::Render();
	if (!commandList) return;

    if (m_imguiBackend == rhi::Backend::Null) {
        return;
    }

    if (m_imguiBackend == rhi::Backend::D3D12) {
        if (!g_pd3dSrvDescHeap) {
            return;
        }
        commandList.SetDescriptorHeaps(g_pd3dSrvDescHeap->GetHandle(), std::nullopt);
    }

	rhi::PassBeginInfo beginInfo{};
	rhi::ColorAttachment attchment{};
    attchment.loadOp = rhi::LoadOp::Load;
	attchment.rtv = { context.rtvHeap.GetHandle() , context.frameIndex }; // Index into the swapchain RTV heap
	beginInfo.colors = { &attchment };
    beginInfo.height = context.outputResolution.y;
    beginInfo.width = context.outputResolution.x;

	commandList.BeginPass(beginInfo);

    if (m_imguiBackend == rhi::Backend::D3D12) {
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), rhi::dx12::get_cmd_list(commandList));
    }
#if BASICRENDERER_HAS_IMGUI_VULKAN
    else if (m_imguiBackend == rhi::Backend::Vulkan) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), rhi::vulkan::get_cmd_list(commandList));
    }
#endif
	commandList.EndPass();

}


void Menu::Cleanup() {
        if (m_imguiBackend == rhi::Backend::Vulkan) {
#if BASICRENDERER_HAS_IMGUI_VULKAN
            for (auto& entry : imguiVkTextureIds_) {
                ImGui_ImplVulkan_RemoveTexture(entry.second);
            }
            imguiVkTextureIds_.clear();
#endif
        }
        if (m_imguiBackend == rhi::Backend::D3D12) {
            ImGui_ImplDX12_Shutdown();
        }
#if BASICRENDERER_HAS_IMGUI_VULKAN
        if (m_imguiBackend == rhi::Backend::Vulkan) {
            ImGui_ImplVulkan_Shutdown();
            if (m_imguiVkPreviewSampler != VK_NULL_HANDLE && m_imguiVkDevice != VK_NULL_HANDLE) {
                vkDestroySampler(m_imguiVkDevice, m_imguiVkPreviewSampler, nullptr);
            }
        }
#endif
        if (m_imguiWin32Initialized) {
            ImGui_ImplWin32_Shutdown();
        }
        m_imguiBackend = rhi::Backend::Null;
        m_imguiWin32Initialized = false;
        g_pd3dSrvDescHeap.Reset();
        imguiHeapGpuStart_ = 0;
        imguiHeapIncrementSize_ = 0;
        imguiHeapNextSlot_ = 1;
        imguiHeapFreeSlots_ = {};
#if BASICRENDERER_HAS_IMGUI_VULKAN
        imguiVkTextureIds_.clear();
        m_imguiVkPreviewSampler = VK_NULL_HANDLE;
        m_imguiVkDevice = VK_NULL_HANDLE;
#endif
		m_settingSubscriptions.clear();
		m_telemetryQuery = {};
		m_visibleClustersQuery = {};
		m_visibleCounterQuery = {};
        m_alphaDeepVisibilityCounterQuery = {};
        m_alphaDeepVisibilityOverflowQuery = {};
        m_alphaDeepVisibilityStatsQuery = {};
		m_reyesTelemetryPhase1Query = {};
		m_reyesTelemetryPhase2Query = {};
        m_shadowReyesTelemetryPhase1Query = {};
        m_shadowTelemetryQuery = {};
        m_shadowVisibleCounterQuery = {};
        m_shadowVisibleClustersQuery = {};
        m_shadowVirtualShadowStatsQuery = {};
		m_shadowVirtualShadowRuntimeStateQuery = {};
    }


uint32_t Menu::AllocateImGuiDescriptor() {
        std::lock_guard lock(imguiHeapMutex_);
        if (!imguiHeapFreeSlots_.empty()) {
            uint32_t idx = imguiHeapFreeSlots_.front();
            imguiHeapFreeSlots_.pop();
            return idx;
        }
        if (imguiHeapNextSlot_ < kImGuiHeapCapacity) {
            return imguiHeapNextSlot_++;
        }
        throw std::runtime_error("ImGui descriptor heap exhausted");
    }

void Menu::FreeImGuiDescriptor(uint32_t index) {
        if (index == 0) return; // never free the font atlas slot
#if BASICRENDERER_HAS_IMGUI_VULKAN
        if (m_imguiBackend == rhi::Backend::Vulkan) {
            auto textureIt = imguiVkTextureIds_.find(index);
            if (textureIt != imguiVkTextureIds_.end()) {
                ImGui_ImplVulkan_RemoveTexture(textureIt->second);
                imguiVkTextureIds_.erase(textureIt);
            }
        }
#endif
        std::lock_guard lock(imguiHeapMutex_);
        imguiHeapFreeSlots_.push(index);
    }

ImTextureID Menu::GetImGuiGpuDescriptorHandle(uint32_t index) {
        if (m_imguiBackend == rhi::Backend::D3D12) {
        return static_cast<ImTextureID>(imguiHeapGpuStart_ + static_cast<uint64_t>(index) * imguiHeapIncrementSize_);
        }
#if BASICRENDERER_HAS_IMGUI_VULKAN
        if (m_imguiBackend == rhi::Backend::Vulkan && g_pd3dSrvDescHeap && m_imguiVkPreviewSampler != VK_NULL_HANDLE) {
            auto textureIt = imguiVkTextureIds_.find(index);
            if (textureIt != imguiVkTextureIds_.end()) {
                return (ImTextureID)textureIt->second;
            }

            auto device = DeviceManager::GetInstance().GetDevice();
            const rhi::DescriptorSlot slot{ g_pd3dSrvDescHeap->GetHandle(), index };
            const VkImageView imageView = rhi::vulkan::get_image_view(device, slot);
            if (imageView == VK_NULL_HANDLE) {
                return static_cast<ImTextureID>(0);
            }

            VkDescriptorSet textureSet = ImGui_ImplVulkan_AddTexture(
                m_imguiVkPreviewSampler,
                imageView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (textureSet == VK_NULL_HANDLE) {
                return static_cast<ImTextureID>(0);
            }

            imguiVkTextureIds_[index] = textureSet;
            return (ImTextureID)textureSet;
        }
#endif
        return static_cast<ImTextureID>(0);
    }

Menu::Menu() { 
        ImGui::CreateContext();
		ImPlot::CreateContext();
}

