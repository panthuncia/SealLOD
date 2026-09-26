# CMakeList.txt : CMake project for BasicRenderer, include source and define
# project specific logic here.
#

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(BasicRenderer LANGUAGES CXX)
endif()

get_filename_component(BASICRENDERER_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(BASICRENDERER_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(BASICRENDERER_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/BasicRenderer")

set(BASICRENDERER_ASSETS_ROOT "${BASICRENDERER_REPO_ROOT}" CACHE PATH "Root containing shared models/textures assets")
set(BASICRENDERER_USD_ROOT "${BASICRENDERER_REPO_ROOT}/ThirdParty" CACHE PATH "Root containing usd_dbg/usd_rel folders")

# Ensure project include paths take precedence over system paths.
set(CMAKE_INCLUDE_DIRECTORIES_BEFORE ON)


# Add source to this project's executable.
add_definitions(-D_UNICODE -DUNICODE)

include("${BASICRENDERER_REPO_ROOT}/cmake/RendererDependencies.cmake")
# Retain the inner entry point's fallback defaults without overriding host targets.
if(NOT TARGET BasicTelemetry::Core)
    set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
endif()
basicrenderer_resolve_dependencies("${BASICRENDERER_REPO_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_deps")

if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    get_filename_component(_basicrenderer_clang_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_basicrenderer_clang_root_dir "${_basicrenderer_clang_bin_dir}" DIRECTORY)
    set(_basicrenderer_clang_libomp "${_basicrenderer_clang_root_dir}/lib/libomp.lib")
    if(EXISTS "${_basicrenderer_clang_libomp}")
        set(OpenMP_C_FLAGS "/openmp" CACHE STRING "OpenMP C flags for clang-cl" FORCE)
        set(OpenMP_CXX_FLAGS "/openmp" CACHE STRING "OpenMP CXX flags for clang-cl" FORCE)
        set(OpenMP_C_LIB_NAMES "libomp" CACHE STRING "OpenMP C libraries for clang-cl" FORCE)
        set(OpenMP_CXX_LIB_NAMES "libomp" CACHE STRING "OpenMP CXX libraries for clang-cl" FORCE)
        set(OpenMP_libomp_LIBRARY "${_basicrenderer_clang_libomp}" CACHE FILEPATH "LLVM OpenMP runtime for clang-cl" FORCE)
    endif()
endif()

find_package(spdlog CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(Microsoft.GSL CONFIG REQUIRED)
if(TARGET imgui::imgui)
    set_property(TARGET imgui::imgui PROPERTY INTERFACE_LINK_LIBRARIES "")
endif()
#find_package(meshoptimizer REQUIRED)
find_package(DirectX-Headers CONFIG REQUIRED)
find_package(flecs CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(pxr CONFIG REQUIRED)
if(TARGET TBB::tbb AND NOT TARGET TBB::tbbmalloc)
    add_library(TBB::tbbmalloc INTERFACE IMPORTED)
endif()
find_package(embree CONFIG REQUIRED)
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()
find_package(Eigen3 CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS container_hash interprocess)
find_package(Tracy REQUIRED CONFIG)
if(COMMAND basicrenderer_use_release_library_for_debug)
    basicrenderer_use_release_library_for_debug(fmt::fmt)
    basicrenderer_use_release_library_for_debug(polyclipping::polyclipping)
    basicrenderer_use_release_library_for_debug(poly2tri::poly2tri)
    basicrenderer_use_release_library_for_debug(unofficial::minizip::minizip)
    basicrenderer_use_release_library_for_debug(ZLIB::ZLIB)
    basicrenderer_use_release_library_for_debug(pugixml)
    basicrenderer_use_release_library_for_debug(pugixml::static)
    basicrenderer_use_release_library_for_debug(zip::zip)
    basicrenderer_use_release_library_for_debug(Tracy::TracyClient)
endif()
find_package(slang CONFIG REQUIRED)

option(BASICRENDERER_ENABLE_DIRECTSTORAGE "Enable DirectStorage integration when the SDK is available" ON)
option(BASICRENDERER_ENABLE_NVPERF "Enable NVIDIA Nsight Perf SDK startup diagnostics and future profiling hooks" OFF)
option(BASICRENDERER_BUILD_INTEROP_VALIDATION "Build the opt-in SARP interop validation policy module" OFF)
set(BASICRENDERER_DIRECTSTORAGE_SDK_ROOT "" CACHE PATH "Optional DirectStorage SDK root containing include/dstorage.h and a dstorage import library")
set(BASICRENDERER_NVPERF_SDK_ROOT "$ENV{NVPERF_SDK_ROOT}" CACHE PATH "Path to an NVIDIA Nsight Perf SDK installation")

if(WIN32 AND BASICRENDERER_ENABLE_NVPERF AND NOT TARGET NVPerf::Host)
    find_path(NVPERF_INCLUDE_DIR
        NAMES nvperf_host.h
        HINTS "${BASICRENDERER_NVPERF_SDK_ROOT}"
        PATH_SUFFIXES include
    )
    find_path(NVPERF_PLATFORM_INCLUDE_DIR
        NAMES nvperf_host_impl.h
        HINTS "${BASICRENDERER_NVPERF_SDK_ROOT}"
        PATH_SUFFIXES include/windows-desktop-x64
    )
    find_file(NVPERF_GRFX_HOST_DLL
        NAMES nvperf_grfx_host.dll
        HINTS "${BASICRENDERER_NVPERF_SDK_ROOT}"
        PATH_SUFFIXES bin/x64
    )
    find_library(NVPERF_GRFX_HOST_LIBRARY
        NAMES nvperf_grfx_host
        HINTS "${BASICRENDERER_NVPERF_SDK_ROOT}"
        PATH_SUFFIXES bin/x64
    )

    if(NVPERF_INCLUDE_DIR AND NVPERF_PLATFORM_INCLUDE_DIR AND NVPERF_GRFX_HOST_DLL AND NVPERF_GRFX_HOST_LIBRARY)
        add_library(BasicRenderer_NVPerfHost INTERFACE)
        target_include_directories(BasicRenderer_NVPerfHost INTERFACE
            "${NVPERF_INCLUDE_DIR}"
            "${NVPERF_PLATFORM_INCLUDE_DIR}"
        )
        target_link_libraries(BasicRenderer_NVPerfHost INTERFACE "${NVPERF_GRFX_HOST_LIBRARY}")
        add_library(NVPerf::Host ALIAS BasicRenderer_NVPerfHost)
        message(STATUS "BasicRenderer: NVIDIA Nsight Perf SDK enabled (${BASICRENDERER_NVPERF_SDK_ROOT})")
    else()
        message(FATAL_ERROR
            "BASICRENDERER_ENABLE_NVPERF=ON, but the NVIDIA Nsight Perf SDK was not found. "
            "Set BASICRENDERER_NVPERF_SDK_ROOT or NVPERF_SDK_ROOT to the SDK's NvPerf directory."
        )
    endif()
endif()

if(NOT TARGET BasicTelemetry::Core)
    message(FATAL_ERROR "BasicRenderer requires BasicTelemetry::Core. Install BasicTelemetry or provide it as a sibling project.")
endif()

set(BASICRENDERER_HAS_DIRECTSTORAGE FALSE)
unset(BASICRENDERER_DIRECTSTORAGE_INCLUDE_DIR CACHE)
unset(BASICRENDERER_DIRECTSTORAGE_LIBRARY CACHE)
unset(BASICRENDERER_DIRECTSTORAGE_DLL CACHE)
unset(BASICRENDERER_DIRECTSTORAGE_CORE_DLL CACHE)

if(WIN32 AND BASICRENDERER_ENABLE_DIRECTSTORAGE)
    set(_basicrenderer_directstorage_roots)
    set(_basicrenderer_directstorage_repo_root "${BASICRENDERER_SOURCE_ROOT}/ThirdParty/DirectStorage")

    if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
        set(_basicrenderer_directstorage_arch_dir "ARM64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_basicrenderer_directstorage_arch_dir "x64")
    else()
        set(_basicrenderer_directstorage_arch_dir "x86")
    endif()

    if(EXISTS "${BASICRENDERER_SOURCE_ROOT}/include/ThirdParty/DirectStorage/dstorage.h")
        set(BASICRENDERER_DIRECTSTORAGE_INCLUDE_DIR "${BASICRENDERER_SOURCE_ROOT}/include/ThirdParty/DirectStorage")
    endif()

    if(EXISTS "${_basicrenderer_directstorage_repo_root}/lib/${_basicrenderer_directstorage_arch_dir}/dstorage.lib")
        set(BASICRENDERER_DIRECTSTORAGE_LIBRARY "${_basicrenderer_directstorage_repo_root}/lib/${_basicrenderer_directstorage_arch_dir}/dstorage.lib")
    endif()

    if(EXISTS "${_basicrenderer_directstorage_repo_root}/bin/${_basicrenderer_directstorage_arch_dir}/dstorage.dll")
        set(BASICRENDERER_DIRECTSTORAGE_DLL "${_basicrenderer_directstorage_repo_root}/bin/${_basicrenderer_directstorage_arch_dir}/dstorage.dll")
    endif()

    if(EXISTS "${_basicrenderer_directstorage_repo_root}/bin/${_basicrenderer_directstorage_arch_dir}/dstoragecore.dll")
        set(BASICRENDERER_DIRECTSTORAGE_CORE_DLL "${_basicrenderer_directstorage_repo_root}/bin/${_basicrenderer_directstorage_arch_dir}/dstoragecore.dll")
    endif()

    if(BASICRENDERER_DIRECTSTORAGE_SDK_ROOT)
        list(APPEND _basicrenderer_directstorage_roots "${BASICRENDERER_DIRECTSTORAGE_SDK_ROOT}")
    endif()
    if(DEFINED ENV{DIRECTSTORAGE_SDK_ROOT} AND NOT "$ENV{DIRECTSTORAGE_SDK_ROOT}" STREQUAL "")
        list(APPEND _basicrenderer_directstorage_roots "$ENV{DIRECTSTORAGE_SDK_ROOT}")
    endif()

    foreach(_basicrenderer_directstorage_root IN LISTS _basicrenderer_directstorage_roots)
        if(NOT BASICRENDERER_DIRECTSTORAGE_INCLUDE_DIR)
            if(EXISTS "${_basicrenderer_directstorage_root}/include/dstorage.h")
                set(BASICRENDERER_DIRECTSTORAGE_INCLUDE_DIR "${_basicrenderer_directstorage_root}/include")
            elseif(EXISTS "${_basicrenderer_directstorage_root}/dstorage.h")
                set(BASICRENDERER_DIRECTSTORAGE_INCLUDE_DIR "${_basicrenderer_directstorage_root}")
            endif()
        endif()

        if(NOT BASICRENDERER_DIRECTSTORAGE_LIBRARY)
            find_library(BASICRENDERER_DIRECTSTORAGE_LIBRARY
                NAMES dstorage
                PATHS
                    "${_basicrenderer_directstorage_root}/lib/${_basicrenderer_directstorage_arch_dir}"
                    "${_basicrenderer_directstorage_root}/lib/x64"
                    "${_basicrenderer_directstorage_root}/lib"
                NO_DEFAULT_PATH)
        endif()

        if(NOT BASICRENDERER_DIRECTSTORAGE_DLL)
            find_file(BASICRENDERER_DIRECTSTORAGE_DLL
                NAMES dstorage.dll
                PATHS
                    "${_basicrenderer_directstorage_root}/bin/${_basicrenderer_directstorage_arch_dir}"
                    "${_basicrenderer_directstorage_root}/bin/x64"
                    "${_basicrenderer_directstorage_root}/bin"
                NO_DEFAULT_PATH)
        endif()

        if(NOT BASICRENDERER_DIRECTSTORAGE_CORE_DLL)
            find_file(BASICRENDERER_DIRECTSTORAGE_CORE_DLL
                NAMES dstoragecore.dll
                PATHS
                    "${_basicrenderer_directstorage_root}/bin/${_basicrenderer_directstorage_arch_dir}"
                    "${_basicrenderer_directstorage_root}/bin/x64"
                    "${_basicrenderer_directstorage_root}/bin"
                NO_DEFAULT_PATH)
        endif()
    endforeach()

    if(BASICRENDERER_DIRECTSTORAGE_INCLUDE_DIR AND BASICRENDERER_DIRECTSTORAGE_LIBRARY)
        add_library(BasicRenderer_DirectStorage UNKNOWN IMPORTED)
        set_target_properties(BasicRenderer_DirectStorage PROPERTIES
            IMPORTED_LOCATION "${BASICRENDERER_DIRECTSTORAGE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${BASICRENDERER_DIRECTSTORAGE_INCLUDE_DIR}")
        set(BASICRENDERER_HAS_DIRECTSTORAGE TRUE)
        message(STATUS "BasicRenderer: DirectStorage SDK found at ${BASICRENDERER_DIRECTSTORAGE_INCLUDE_DIR}")
    else()
        message(STATUS "BasicRenderer: DirectStorage SDK not found. Runtime support will compile as a fallback-only stub.")
    endif()
endif()

# Build tool for code generation
add_executable(resource_codegen
  "generators/resource_codegen.cpp"
)

target_link_libraries(resource_codegen
  PRIVATE
    nlohmann_json::nlohmann_json
    fmt::fmt
)

set(BASICRENDERER_DEMO_TARGET BasicRendererDemo)

set(SOURCES
    # src
    "src/Scene/Views/DefaultDirection.cpp"
    "src/Runtime/Renderer/RendererGraphConstruction.cpp"
    "src/Runtime/Renderer/RendererLifecycle.cpp"
    "src/Runtime/Renderer/RendererFrameRendering.cpp"
    "src/Runtime/Renderer/RendererFrameUpdate.cpp"
    "src/Runtime/Renderer/RendererPipeline.cpp"
    "src/Runtime/Renderer/RendererResources.cpp"
    "src/Runtime/Renderer/RendererSceneSync.cpp"
    "src/Runtime/Renderer/RendererSceneIngestion.cpp"
    "src/Runtime/Renderer/RendererSceneApi.cpp"
    # src/Animation
    "src/Animation/Playback/AnimationClip.cpp"
    "src/Animation/Playback/AnimationController.cpp"
    "src/Animation/Skeletons/Skeleton.cpp"
    "src/Animation/Skeletons/SkeletonManager.cpp"
    # src/Assets
    "src/Pipeline/ShaderCompilation/ShaderArtifactCompilation.cpp"
    "src/Assets/Textures/Processing/TextureProcessingManager.cpp"
    "src/Assets/Textures/Processing/TextureProcessingRequests.cpp"
    # src/Diagnostics
    "src/Pipeline/Diagnostics/PipelineControl.cpp"
    "src/Diagnostics/Renderer/RendererDiagnostics.cpp"
    "src/Terrain/Diagnostics/TerrainRvtTelemetry.cpp"
    "src/Diagnostics/Renderer/RendererTelemetryRequests.cpp"
    "src/Diagnostics/Renderer/RendererCLodTelemetryRequests.cpp"
    # src/Factories
    "src/Assets/Textures/TextureFactory.cpp"
    "src/Assets/Textures/RenderPasses/MipmappingPass.cpp"
    "src/Assets/Textures/RenderPasses/BC7CompressionPasses.cpp"
    # src/FidelityFX
    "src/PostProcessing/FidelityFX/Backend/FfxBackendAdapters.cpp"
    "src/PostProcessing/FidelityFX/Backend/FfxBackendDX12.cpp"
    "src/PostProcessing/FidelityFX/Backend/FfxBackendVulkan.cpp"
    # src/Import
    "src/Assets/Import/Assimp/AssimpGeometryExtractor.cpp"
    "src/Assets/Import/Assimp/AssimpLoader.cpp"
    "src/Assets/Import/NIF/BRNiflyClient.cpp"
    "src/Assets/Cache/ClusterLOD/CLodCache.cpp"
    "src/Assets/Cache/ClusterLOD/CLodCacheLoader.cpp"
    "src/Assets/GeometryProcessing/ClusterLOD/DefaultCLodSettings.cpp"
    "src/Assets/Import/GlTF/GlTFGeometryExtractor.cpp"
    "src/Assets/Import/GlTF/GlTFLoader.cpp"
    "src/Assets/Import/ModelLoader.cpp"
    "src/Assets/Import/NIF/NifLoader.cpp"
    "src/Assets/GeometryProcessing/Reyes/ObjectReyesAtlasBaker.cpp"
    "src/Assets/Import/RenderableAsset.cpp"
    "src/Assets/Cache/Skeleton/SkeletonArtifactCache.cpp"
    "src/Assets/Cache/Skeleton/SkeletonArtifactValidation.cpp"
    "src/Assets/Import/USD/USDGeometryExtractor.cpp"
    "src/Assets/Import/USD/USDLoader.cpp"
    "src/Assets/Import/USD/USDStageEntryPoints.cpp"
    "src/Assets/Import/USD/USDMaterialConversion.cpp"
    "src/Assets/Import/USD/USDSkeletonConversion.cpp"
    "src/Assets/Import/USD/USDGeometryConversion.cpp"
    "src/Assets/Import/USD/USDAssetAssembly.cpp"
    "src/Assets/Import/USD/USDMaterialCache.cpp"
    # src/Lighting
    "src/Lighting/Environment/EnvironmentManager.cpp"
    "src/Lighting/Lights/LightManager.cpp"
    "src/Lighting/Environment/RenderPasses/brdfIntegrationPass.h"
    "src/Lighting/Environment/RenderPasses/EnvironmentConversionPass.h"
    "src/Lighting/Environment/RenderPasses/EnvironmentFilterPass.h"
    "src/Lighting/Environment/RenderPasses/EnvironmentSHPass.h"
    "src/Lighting/Environment/RenderPasses/SkyboxRenderPass.h"
    # src/Materials
    "src/Materials/Material.cpp"
    "src/Materials/MaterialManager.cpp"
    "src/Materials/TextureStreaming/MaterialTextureStreaming.cpp"
    "src/Materials/TextureStreaming/MaterialTextureTransferService.cpp"
    "src/Materials/TextureStreaming/TextureStreamingManager.cpp"
    # src/Menu
    "src/Diagnostics/Menu/Panels/VirtualGeometryPanel.cpp"
    "src/Diagnostics/Menu/Panels/FrameTaskGraphPanel.cpp"
    "src/Diagnostics/Menu/Panels/PassTimingPanel.cpp"
    "src/Diagnostics/Menu/Panels/AutoAliasPanel.cpp"
    "src/Diagnostics/Menu/Panels/SceneExplorerPanel.cpp"
    "src/Diagnostics/Menu/RenderPasses/MenuDrawData.cpp"
    "src/Diagnostics/Menu/Panels/EnvironmentPanel.cpp"
    "src/Diagnostics/Menu/Panels/OutputPanel.cpp"
    "src/Diagnostics/Menu/Panels/PostProcessingPanel.cpp"
    "src/Diagnostics/Menu/MenuRuntime.cpp"
    # src/Runtime
    "src/Scene/Objects/IndirectCommandBufferManager.cpp"
    "src/Runtime/IO/DirectStorageManager.cpp"
    "src/Runtime/IO/ReadbackManager.cpp"
    "src/Runtime/Device/RenderDeviceAccess.cpp"
    "src/Pipeline/PipelineState/CommandSignatureManager.cpp"
    "src/Pipeline/PipelineState/PipelineAccess.cpp"
    "src/Runtime/Settings/SettingAccess.cpp"
    "src/Runtime/Settings/RendererSettings.cpp"
    # src/PostProcessing
    "src/PostProcessing/FidelityFX/FFXManager.cpp"
    "src/PostProcessing/Reflections/ScreenSpaceReflectionsGenerationService.cpp"
    "src/PostProcessing/Upscaling/UpscalingGenerationService.cpp"
    "src/PostProcessing/Upscaling/UpscalingManager.cpp"
    # src/Terrain
    "src/Terrain/Residency/TerrainManager.cpp"
    # src/Runtime / src/Assets
    "src/Runtime/Device/DeviceManager.cpp"
    "src/Pipeline/PipelineState/PSOManager.cpp"
    "src/Runtime/Resources/ResourceManager.cpp"
    "src/Pipeline/ShaderCompilation/ShaderArtifactCache.cpp"
    "src/Runtime/Scheduling/TaskSchedulerManager.cpp"
    # src/Mesh
    "src/Assets/GeometryProcessing/ClusterLOD/ClusterLOD.cpp"
    "src/Assets/GeometryProcessing/ClusterLOD/ClusterLODUtilities.cpp"
    "src/Assets/GeometryProcessing/ClusterLOD/ClusterLODValidation.cpp"
    "src/Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.cpp"
    "src/Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePacking.cpp"
    "src/Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.cpp"
    "src/Assets/Geometry/Mesh.cpp"
    "src/Assets/Geometry/MeshIngestBuilder.cpp"
    "src/Assets/Geometry/MeshInstance.cpp"
    "src/Assets/Geometry/MeshInstanceFactory.cpp"
    "src/VirtualGeometry/GeometryStorage/MeshManager.cpp"
    "src/Assets/GeometryProcessing/Voxels/VoxelGroupBuilder.cpp"
    # src/Render
    "src/Runtime/StateGraph/AsyncStateGraph.cpp"
    "src/VirtualGeometry/Streaming/Publication/CLodResidencyStorageArtifacts.cpp"
    "src/Runtime/StateGraph/CapacityProvider.cpp"
    "src/Diagnostics/Scene/DebugSceneSnapshotService.cpp"
    "src/VirtualGeometry/GeometryStorage/GeometryBufferStateArtifacts.cpp"
    "src/VirtualGeometry/Streaming/Publication/GeometryResidencyStateArtifacts.cpp"
    "src/Scene/Objects/IndirectStateArtifacts.cpp"
    "src/Lighting/Lights/LightStateArtifacts.cpp"
    "src/Materials/Publication/MaterialStateArtifacts.cpp"
    "src/Scene/Objects/ObjectBufferStateArtifacts.cpp"
    "src/Materials/Evaluation/OpenPBRLookupResources.cpp"
    "src/Runtime/Publication/PersistentRendererPublication.cpp"
    "src/Animation/Pose/PoseInstanceRegistrationService.cpp"
    "src/Animation/Pose/PoseStateArtifacts.cpp"
    "src/Runtime/Publication/PublishedRendererState.cpp"
    "src/Runtime/GraphIntegration/RenderGraphIOService.cpp"
    "src/Runtime/Publication/RendererStateRequestService.cpp"
    "src/Scene/Ingestion/SceneAssetRequestService.cpp"
    "src/Scene/Ingestion/SceneEntityMaterializationService.cpp"
    "src/Scene/Ingestion/SceneRenderBridge.cpp"
    "src/Scene/Ingestion/SceneRenderableResidencyService.cpp"
    "src/Scene/Ingestion/SceneSourceStateStore.cpp"
    "src/Pipeline/ShaderCompilation/ShaderVariantRequestService.cpp"
    "src/VirtualGeometry/GeometryStorage/StaticGeometryRequestService.cpp"
    "src/Materials/Publication/StaticMaterialRequestService.cpp"
    "src/Scene/Objects/StaticObjectRequestService.cpp"
    "src/Runtime/Publication/StaticStateArtifacts.cpp"
    "src/Scene/Objects/StaticWorkloadRequestService.cpp"
    "src/Runtime/Scheduling/TbbTaskService.cpp"
    "src/Terrain/Residency/TerrainStateArtifacts.cpp"
    "src/Materials/TextureStreaming/TextureBindingArtifacts.cpp"
    "src/Materials/TextureStreaming/TextureImageTableArtifacts.cpp"
    "src/Runtime/Resources/VersionedBuffers/VersionedGpuBufferArtifacts.cpp"
    "src/Scene/Views/ViewStateArtifacts.cpp"
    # src/Render/GraphExtensions
    "src/Transparency/GraphIntegration/CLodAlphaVariant.cpp"
    "src/VirtualGeometry/GraphIntegration/CLodExtension.cpp"
    "src/VirtualGeometry/GraphIntegration/CLodExtensionShared.cpp"
    "src/VirtualShadows/GraphIntegration/CLodShadowVariant.cpp"
    "src/VirtualGeometry/GraphIntegration/CLodVisibilityVariant.cpp"
    "src/VirtualShadows/Casters/VirtualShadowCasterProvider.cpp"
    # src/Render/Pipeline
    "src/Pipeline/Recipes/PipelineRecipe.cpp"
    # src/RenderPasses
    "src/Scene/Views/RenderPasses/PrimaryCameraUploadPass.cpp"
    "src/Lighting/Clustered/RenderPasses/ClusterGenerationPass.h"
    "src/Lighting/Clustered/RenderPasses/LightCullingPass.h"
    "src/VirtualGeometry/Rasterization/RenderPasses/ClearVisibilityBufferPass.h"
    "src/Diagnostics/RenderPasses/DebugGridPass.h"
    "src/Lighting/Shading/RenderPasses/DeferredShadingPass.h"
    "src/Diagnostics/RenderPasses/DebugSpheresPass.h"
    "src/Diagnostics/RenderPasses/DebugSkeletonPass.h"
    "src/Runtime/Frame/RenderPasses/PresentPass.h"
    "src/Scene/Views/RenderPasses/PrimaryDepthCopyPass.h"
    "src/Scene/Views/RenderPasses/PrimaryCameraUploadPass.h"
    "src/PostProcessing/Downsampling/RenderPasses/Downsample.h"
    "src/Scene/Views/RenderPasses/LinearDepthHistoryCopyPass.h"
    "src/PostProcessing/AmbientOcclusion/RenderPasses/XeGTAODenoisePass.h"
    "src/PostProcessing/AmbientOcclusion/RenderPasses/XeGTAOFilterPass.h"
    "src/PostProcessing/AmbientOcclusion/RenderPasses/XeGTAOMainPass.h"
    "src/PostProcessing/Bloom/RenderPasses/BloomBlendPass.h"
    "src/PostProcessing/Bloom/RenderPasses/BloomSamplePass.h"
    "src/Diagnostics/RenderPasses/DebugResolvePass.h"
    "src/PostProcessing/Upscaling/RenderPasses/DilateMotionVectorsPass.h"
    "src/PostProcessing/Exposure/RenderPasses/luminanceHistogram.h"
    "src/PostProcessing/Exposure/RenderPasses/luminanceHistogramAverage.h"
    "src/Lighting/Environment/RenderPasses/SpecularIBLPass.h"
    "src/PostProcessing/ToneMapping/RenderPasses/Tonemapping.h"
    # src/Resources
    "src/Runtime/Resources/Sampler.cpp"
    "src/Assets/Textures/Texture.cpp"
    # src/Resources/Buffers
    "src/Runtime/Resources/Buffers/BufferView.cpp"
    "src/Runtime/Resources/Buffers/DynamicBuffer.cpp"
    "src/Runtime/Resources/Buffers/DynamicStructuredBuffer.cpp"
    "src/Runtime/Resources/Buffers/PagePool.cpp"
    "src/Runtime/Resources/Buffers/SortedUnsignedIntBuffer.cpp"
    # src/Scene
    "src/Scene/ECS/ECSManager.cpp"
    "src/Scene/ECS/RendererECSManager.cpp"
    "src/Scene/Environment.cpp"
    "src/Scene/Objects/ObjectManager.cpp"
    "src/Scene/Scene.cpp"
    "src/Scene/Views/ViewManager.cpp"
    # src/Telemetry
    "src/Diagnostics/Telemetry/FrameTaskGraphTelemetry.cpp"
    "src/Diagnostics/Telemetry/NvPerfIntegration.cpp"
    "src/Diagnostics/Telemetry/SamplingControlServer.cpp"
    "src/Diagnostics/Telemetry/StatisticalSampler.cpp"
    # src/Transparency
    "src/Transparency/AVBOIT/RenderPasses/AVBOITAdaptiveFitPass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITAdaptiveFitUpdatePass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITDepthWarpPass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITEarlyDepthBuildPass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITEarlyDepthPass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITIntegratePass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITOccupancyHistogramPass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITOccupancyRemapPass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITResolvePass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITSetupPass.cpp"
    "src/Transparency/AVBOIT/RenderPasses/AVBOITSparseClearPass.cpp"
    # src/Utilities
    "src/Assets/Cache/CachePathUtilities.cpp"
    "src/Utilities/MathUtils.cpp"
    "src/Assets/Textures/Import/StbImage.cpp"
    "src/Utilities/Utilities.cpp"
    # src/VirtualGeometry/Culling
    "src/VirtualGeometry/Culling/RenderPasses/HierarchicalCullingPass.cpp"
    "src/VirtualGeometry/Culling/RenderPasses/HierarchicalDispatchCullingPass.cpp"
    # src/VirtualGeometry/GraphIntegration
    "src/VirtualGeometry/GraphIntegration/CLodViewTables.cpp"
    # src/VirtualGeometry/Rasterization
    "src/VirtualGeometry/Rasterization/RenderPasses/ClearDeepVisibilityPass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/ClusterRasterizationPass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/ClusterSoftwareRasterizationPass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/DeepVisibilityResolvePass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/PerViewLinearDepthCopyPass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/RasterBucketBlockOffsetsPass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/RasterBucketBlockScanPass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/RasterBucketCompactAndArgsPass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/RasterBucketCreateCommandPass.cpp"
    "src/VirtualGeometry/Rasterization/RenderPasses/RasterBucketHistogramPass.cpp"
    # src/VirtualGeometry/RayTracing
    "src/VirtualGeometry/RayTracing/CLodRayTracingSystem.cpp"
    # src/VirtualGeometry/Reyes
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesBuildRasterWorkPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesClassifyPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesCreateDispatchArgsPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesDeepVisibilityRasterizationPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesDicePass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesPatchRasterizationPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesQueueResetPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesRasterWorkCompactAndArgsPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesRasterWorkHistogramPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesReplayMergePass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesSeedPatchesPass.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesSplitPass.cpp"
    "src/VirtualGeometry/Reyes/Tessellation/ReyesTessellationGeneratedReferenceSource.cpp"
    "src/VirtualGeometry/Reyes/Tessellation/ReyesTessellationTable.cpp"
    "src/VirtualGeometry/Reyes/RenderPasses/ReyesTessellationTableUploadPass.cpp"
    # src/VirtualGeometry/Streaming
    "src/VirtualGeometry/Streaming/CLodPageLRU.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingPageWaiters.cpp"
    "src/VirtualGeometry/Streaming/RenderPasses/CLodStreamingBeginFramePass.cpp"
    "src/VirtualGeometry/Streaming/RenderPasses/CLodStreamingFeedbackSortPass.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingAdmission.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingCompletions.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingDomain.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingFeedback.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingPageResidency.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingResidencyCommit.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingSystem.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingShadowNotifications.cpp"
    "src/VirtualGeometry/Streaming/CLodStreamingTrace.cpp"
    "src/VirtualGeometry/Streaming/CLodUploadStream.cpp"
    # src/VirtualGeometry/Voxels
    "src/VirtualGeometry/Voxels/RenderPasses/VoxelSoftwareRasterizationPass.cpp"
    # src/VirtualShadows
    "src/VirtualShadows/Rasterization/RenderPasses/ReyesVirtualShadowHardwareRasterPass.cpp"
    "src/VirtualShadows/Rasterization/RenderPasses/ReyesVirtualShadowRasterizationPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapAdmitPagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapAllocatePagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildActiveBlocksPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildMarkTilesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildPageListsPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapClearDirtyBitsPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapClearPagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapComposePagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapDeduplicatePredictedPagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapDirtyHierarchyPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapExpandPredictedPagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapFreeWrappedPagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapGatherStatsPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapInvalidatePagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapMarkPagesPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapNonRasterableHierarchyPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapResolveMarkedBlocksPass.cpp"
    "src/VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapSetupPass.cpp"
)
set(BASICRENDERER_DEMO_SOURCE "${BASICRENDERER_SOURCE_ROOT}/src/BasicRenderer.cpp")
set(BASICRENDERER_SAMPLING_CONTROL_SOURCE "${BASICRENDERER_SOURCE_ROOT}/src/Diagnostics/Telemetry/StatisticalSamplingControl.cpp")


if(EXISTS "${BASICRENDERER_REPO_ROOT}/xatlas/source/xatlas/xatlas.cpp")
    add_library(xatlas STATIC "${BASICRENDERER_REPO_ROOT}/xatlas/source/xatlas/xatlas.cpp")
    target_include_directories(xatlas PUBLIC "${BASICRENDERER_REPO_ROOT}/xatlas/source/xatlas")
    set_target_properties(xatlas PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
endif()

if(EXISTS "${BASICRENDERER_REPO_ROOT}/geometry-central/CMakeLists.txt" AND NOT TARGET geometry-central)
    set(SUITESPARSE OFF CACHE BOOL "Enable SuiteSparse for geometry-central." FORCE)
    add_subdirectory("${BASICRENDERER_REPO_ROOT}/geometry-central" "${CMAKE_CURRENT_BINARY_DIR}/_deps/geometry-central" EXCLUDE_FROM_ALL)
    set_property(TARGET geometry-central PROPERTY INTERFACE_COMPILE_DEFINITIONS "")
endif()

add_library(${PROJECT_NAME} STATIC ${SOURCES})
add_library(BasicRendererHeaders INTERFACE)
add_library(BasicRenderer::Headers ALIAS BasicRendererHeaders)
target_include_directories(BasicRendererHeaders INTERFACE
    "${BASICRENDERER_SOURCE_ROOT}/include")
target_link_libraries(${PROJECT_NAME} PUBLIC BasicRendererHeaders)
target_sources(${PROJECT_NAME} PRIVATE
    "src/Runtime/Publication/PersistentRendererPublication.h"
    "src/Runtime/Publication/PersistentRendererPublication.cpp"
    "src/VirtualGeometry/Culling/RenderPasses/CapturedHierarchicalDispatchCullingInputs.h"
    "src/VirtualGeometry/Culling/RenderPasses/CapturedCullingCommandSink.h"
    "src/VirtualGeometry/Culling/RenderPasses/PersistentHierarchicalDispatchCullingPass.h"
    "src/Materials/Evaluation/RenderPasses/PersistentMaterialEvaluationPass.h")
add_library(BasicRenderer::BasicRenderer ALIAS ${PROJECT_NAME})
foreach(_basicrenderer_api_category IN ITEMS
    Runtime Scene Assets Streaming Pipeline Extensions Diagnostics)
    add_executable(BasicRendererApi${_basicrenderer_api_category}Smoke
        "tests/BuildTreeApiConsumer/${_basicrenderer_api_category}.cpp")
    target_compile_features(BasicRendererApi${_basicrenderer_api_category}Smoke PRIVATE cxx_std_23)
    target_link_libraries(BasicRendererApi${_basicrenderer_api_category}Smoke
        PRIVATE BasicRenderer::BasicRenderer)
endforeach()
add_library(BasicRendererPublicHeaderSmoke OBJECT
    "tests/PublicHeaders/Assets_ClusterLODShaderTypes.cpp"
    "tests/PublicHeaders/Assets_Import_Filetypes.cpp"
    "tests/PublicHeaders/Assets_MaterialBlendState.cpp"
    "tests/PublicHeaders/Assets_MaterialTextureStreaming.cpp"
    "tests/PublicHeaders/Assets_TechniqueDescriptor.cpp"
    "tests/PublicHeaders/Assets_TextureResidency.cpp"
    "tests/PublicHeaders/Extensions_Input_InputAction.cpp"
    "tests/PublicHeaders/Extensions_Input_InputContext.cpp"
    "tests/PublicHeaders/Extensions_PreparedRenderGraph_PreparedComputeBarrier.cpp"
    "tests/PublicHeaders/Extensions_PreparedRenderGraph_PreparedComputeCommands.cpp"
    "tests/PublicHeaders/Extensions_PreparedRenderGraph_PreparedComputeDispatch.cpp"
    "tests/PublicHeaders/Extensions_PreparedRenderGraph_PreparedEnvironmentDispatch.cpp"
    "tests/PublicHeaders/Extensions_PreparedRenderGraph_PreparedFullscreenDraw.cpp"
    "tests/PublicHeaders/Extensions_PreparedRenderGraph_PreparedLegacyAdmission.cpp"
    "tests/PublicHeaders/Extensions_PreparedRenderGraph_PreparedRenderIndirect.cpp"
    "tests/PublicHeaders/Extensions_PreparedRenderGraph_PreparedResourceClears.cpp"
    "tests/PublicHeaders/Scene_RendererComponents.cpp"
    "tests/PublicHeaders/Pipeline_PipelineKind.cpp"
    "tests/PublicHeaders/Assets_CachePaths.cpp"
    "tests/PublicHeaders/Assets_CLodCacheIdentity.cpp"
    "tests/PublicHeaders/Assets_CLodCacheStore.cpp"
    "tests/PublicHeaders/Scene_SceneFrameSnapshot.cpp"
    "tests/PublicHeaders/Scene_FrustumUtilities.cpp"
    "tests/PublicHeaders/Pipeline_ShaderVariantRequestService.cpp"
    "tests/PublicHeaders/Scene_SceneIngestionServices.cpp"
    "tests/PublicHeaders/Scene_SceneRenderBridge.cpp"
    "tests/PublicHeaders/Extensions_RenderContext.cpp"
    "tests/PublicHeaders/Extensions_DynamicStructuredBuffer.cpp"
    "tests/PublicHeaders/Extensions_Buffers_BufferView.cpp"
    "tests/PublicHeaders/Extensions_Buffers_DynamicBuffer.cpp"
    "tests/PublicHeaders/Extensions_Buffers_LazyDynamicStructuredBuffer.cpp"
    "tests/PublicHeaders/Extensions_Buffers_MemoryBlock.cpp"
    "tests/PublicHeaders/Extensions_Buffers_PagePool.cpp"
    "tests/PublicHeaders/Extensions_Buffers_SortedUnsignedIntBuffer.cpp"
    "tests/PublicHeaders/Extensions_PublishedStateResourceResolver.cpp"
    "tests/PublicHeaders/Extensions_ResourceComponent.cpp"
    "tests/PublicHeaders/Assets_ClusterLODTypes.cpp"
    "tests/PublicHeaders/Assets_MaterialFlags.cpp"
    "tests/PublicHeaders/Assets_ClusterLODUtilities.cpp"
    "tests/PublicHeaders/Assets_SGGX.cpp"
    "tests/PublicHeaders/Assets_DefaultCLodSettings.cpp"
    "tests/PublicHeaders/Assets_MeshData.cpp"
    "tests/PublicHeaders/Assets_MaterialDescription.cpp"
    "tests/PublicHeaders/Assets_DefaultedOptional.cpp"
    "tests/PublicHeaders/Assets_GeometryMesh.cpp"
    "tests/PublicHeaders/Assets_GeometryMeshInstance.cpp"
    "tests/PublicHeaders/Assets_GeometryMeshInstanceFactory.cpp"
    "tests/PublicHeaders/Assets_GeometryVertexLayout.cpp"
    "tests/PublicHeaders/Assets_ImportedAsset.cpp"
    "tests/PublicHeaders/Assets_ImportSettings.cpp"
    "tests/PublicHeaders/Assets_ImportModelLoader.cpp"
    "tests/PublicHeaders/Assets_Import_AssimpGeometryExtractor.cpp"
    "tests/PublicHeaders/Assets_Import_BRNiflyClient.cpp"
    "tests/PublicHeaders/Assets_Import_CLodCache.cpp"
    "tests/PublicHeaders/Assets_Import_GlTFGeometryExtractor.cpp"
    "tests/PublicHeaders/Assets_Import_USDGeometryExtractor.cpp"
    "tests/PublicHeaders/Assets_Import_USDMaterialCache.cpp"
    "tests/PublicHeaders/Assets_Import_MeshPreprocessData.cpp"
    "tests/PublicHeaders/Assets_Import_RenderablePrototypeGeometry.cpp"
    "tests/PublicHeaders/Assets_Import_ObjectReyesAtlasCache.cpp"
    "tests/PublicHeaders/Assets_Import_CLodCacheLoader.cpp"
    "tests/PublicHeaders/Assets_Import_VoxelGroupBuilder.cpp"
    "tests/PublicHeaders/Assets_ImportNifLoader.cpp"
    "tests/PublicHeaders/Assets_USDLoader.cpp"
    "tests/PublicHeaders/Assets_RenderableAsset.cpp"
    "tests/PublicHeaders/Assets_TextureTypes.cpp"
    "tests/PublicHeaders/Assets_Texture.cpp"
    "tests/PublicHeaders/Assets_TextureLoading.cpp"
    "tests/PublicHeaders/Assets_CpuTextureSampler.cpp"
    "tests/PublicHeaders/Assets_Material.cpp"
    "tests/PublicHeaders/Assets_TextureProcessingRequests.cpp"
    "tests/PublicHeaders/Assets_ShaderArtifactCompilation.cpp"
    "tests/PublicHeaders/Diagnostics_SamplingReadinessSnapshot.cpp"
    "tests/PublicHeaders/Diagnostics_MaterialTextureStreamingStats.cpp"
    "tests/PublicHeaders/Diagnostics_OutputTypes.cpp"
    "tests/PublicHeaders/Diagnostics_TerrainRvtTelemetry.cpp"
    "tests/PublicHeaders/Diagnostics_CLodTelemetry.cpp"
    "tests/PublicHeaders/Diagnostics_PipelineControl.cpp"
    "tests/PublicHeaders/Diagnostics_NvPerfIntegration.cpp"
    "tests/PublicHeaders/Diagnostics_StatisticalSampler.cpp"
    "tests/PublicHeaders/Diagnostics_SamplingControlServer.cpp"
    "tests/PublicHeaders/Extensions_RenderGraphExtensionRegistration.cpp"
    "tests/PublicHeaders/Extensions_RenderDeviceAccess.cpp"
    "tests/PublicHeaders/Extensions_PipelineAccess.cpp"
    "tests/PublicHeaders/Extensions_ShaderCompilationTypes.cpp"
    "tests/PublicHeaders/Extensions_SettingAccess.cpp"
    "tests/PublicHeaders/Extensions_InputManager.cpp"
    "tests/PublicHeaders/Extensions_BuiltinResources.cpp"
    "tests/PublicHeaders/Extensions_ShaderBuffers.cpp"
    "tests/PublicHeaders/Extensions_IndirectCommand.cpp"
    "tests/PublicHeaders/Extensions_VirtualShadowCasterProvider.cpp"
    "tests/PublicHeaders/Extensions_VirtualGeometryCLodCommon.cpp"
    "tests/PublicHeaders/Extensions_ProducerPersistentState.cpp"
    "tests/PublicHeaders/Pipeline_PipelineRecipe.cpp"
    "tests/PublicHeaders/Pipeline_PSOFlags.cpp"
    "tests/PublicHeaders/Pipeline_RenderPhase.cpp"
    "tests/PublicHeaders/Pipeline_DrawWorkload.cpp"
    "tests/PublicHeaders/Pipeline_MaterialEvalVariants.cpp"
    "tests/PublicHeaders/Pipeline_MeshDrawWorkload.cpp"
    "tests/PublicHeaders/Pipeline_RendererSettings.cpp"
    "tests/PublicHeaders/Pipeline_CLodOptions.cpp"
    "tests/PublicHeaders/Pipeline_UpscalingTypes.cpp"
    "tests/PublicHeaders/Renderer.cpp"
    "tests/PublicHeaders/Scene_Components.cpp"
    "tests/PublicHeaders/Scene_AnimationAnimation.cpp"
    "tests/PublicHeaders/Scene_AnimationAnimationClip.cpp"
    "tests/PublicHeaders/Scene_AnimationAnimationController.cpp"
    "tests/PublicHeaders/Scene_AnimationDynamicWindMetadata.cpp"
    "tests/PublicHeaders/Scene_AnimationKeyframe.cpp"
    "tests/PublicHeaders/Scene_AnimationSkeleton.cpp"
    "tests/PublicHeaders/Scene_AnimationSkeletonArtifact.cpp"
    "tests/PublicHeaders/Scene_VertexFlags.cpp"
    "tests/PublicHeaders/Scene_Environment.cpp"
    "tests/PublicHeaders/Scene_Scene.cpp"
    "tests/PublicHeaders/Scene_TerrainTypes.cpp"
    "tests/PublicHeaders/Streaming_ArtifactTypes.cpp"
    "tests/PublicHeaders/Streaming_CLodGeometryTypes.cpp"
    "tests/PublicHeaders/Streaming_DirectStorageRequest.cpp"
    "tests/PublicHeaders/Streaming_GeometryRequests.cpp"
    "tests/PublicHeaders/Streaming_MaterialRequests.cpp"
    "tests/PublicHeaders/Streaming_ObjectRequests.cpp"
    "tests/PublicHeaders/Streaming_PublishedTypes.cpp"
    "tests/PublicHeaders/Streaming_PoseState.cpp"
    "tests/PublicHeaders/Streaming_PublishedRendererState.cpp"
    "tests/PublicHeaders/Streaming_StaticObjectRequestService.cpp"
    "tests/PublicHeaders/Streaming_StaticSceneArtifacts.cpp"
    "tests/PublicHeaders/Streaming_RendererStateArtifacts.cpp"
    "tests/PublicHeaders/Streaming_SceneAssetRequestService.cpp"
    "tests/PublicHeaders/Streaming_StaticGeometryRequestService.cpp"
    "tests/PublicHeaders/Streaming_StaticMaterialRequestService.cpp"
    "tests/PublicHeaders/Streaming_StaticWorkloadRequestService.cpp"
    "tests/PublicHeaders/Streaming_WorkloadRequests.cpp"
    "tests/PublicHeaders/Streaming_VersionedGpuBuffer.cpp"
    "tests/PublicHeaders/Streaming_RendererStateRequestService.cpp"
    "tests/PublicHeaders/Streaming_TaskScheduler.cpp"
    "tests/PublicHeaders/Streaming_SerializedTaskPump.cpp"
    "tests/PublicHeaders/Extensions_EnvironmentWorkService.cpp"
    "tests/PublicHeaders/Extensions_WindPaletteService.cpp"
    "tests/PublicHeaders/Pipeline_RasterBucketFlags.cpp"
    "tests/PublicHeaders/Streaming_LightStateArtifacts.cpp"
    "tests/PublicHeaders/Streaming_ViewStateArtifacts.cpp"
    "tests/PublicHeaders/Runtime_Detail_DepthHistoryService.cpp"
    "tests/PublicHeaders/Runtime_Detail_PoseInstanceRegistrationService.cpp"
    "tests/PublicHeaders/Runtime_Detail_SceneEntityMaterializationService.cpp"
    "tests/PublicHeaders/Runtime_Detail_SceneRenderableResidencyService.cpp"
    "tests/PublicHeaders/Runtime_Detail_SceneSourceStateStore.cpp"
    "tests/PublicHeaders/Runtime_Detail_FrameTimer.cpp"
    "tests/PublicHeaders/Runtime_Detail_OpenPBRLookupResources.cpp"
    "tests/PublicHeaders/Runtime_Detail_MaterialEvaluationBuildInputs.cpp"
)
target_link_libraries(BasicRendererPublicHeaderSmoke PRIVATE ${PROJECT_NAME})
add_dependencies(BasicRendererPublicHeaderSmoke Generate_BuiltinResources)
target_compile_features(BasicRendererPublicHeaderSmoke PRIVATE cxx_std_23)
if(BASICRENDERER_BUILD_INTEROP_VALIDATION)
    add_library(SARPInteropValidation STATIC
        "${BASICRENDERER_SOURCE_ROOT}/validation/SARPInteropValidation.cpp")
    target_compile_features(SARPInteropValidation PRIVATE cxx_std_23)
    target_include_directories(SARPInteropValidation PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
    target_link_libraries(SARPInteropValidation PRIVATE OpenRenderGraph::OpenRenderGraph spdlog::spdlog_header_only)
    target_link_libraries(${PROJECT_NAME} PRIVATE SARPInteropValidation)
    target_compile_definitions(${PROJECT_NAME} PUBLIC BASICRENDERER_HAS_INTEROP_VALIDATION=1)
else()
    target_compile_definitions(${PROJECT_NAME} PUBLIC BASICRENDERER_HAS_INTEROP_VALIDATION=0)
endif()
add_executable(${BASICRENDERER_DEMO_TARGET} WIN32 "${BASICRENDERER_DEMO_SOURCE}")
target_include_directories(${BASICRENDERER_DEMO_TARGET} PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
add_executable(StatisticalSamplingControl "${BASICRENDERER_SAMPLING_CONTROL_SOURCE}")
target_compile_features(StatisticalSamplingControl PRIVATE cxx_std_23)
target_link_libraries(StatisticalSamplingControl PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(StatisticalSamplingControl PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(${BASICRENDERER_DEMO_TARGET} PROPERTIES
    OUTPUT_NAME ${PROJECT_NAME}
    ARCHIVE_OUTPUT_NAME ${BASICRENDERER_DEMO_TARGET}
)
add_executable(ShaderPreprocessTests "tests/ShaderPreprocessTests.cpp")
add_executable(SkeletonArtifactCacheTests "tests/SkeletonArtifactCacheTests.cpp")
add_executable(PipelineRecipeTests "tests/PipelineRecipeTests.cpp")
add_executable(MaterialEvalVariantTests "tests/MaterialEvalVariantTests.cpp")
add_executable(MaterialOwnershipGuardTests "tests/MaterialOwnershipGuardTests.cpp")
add_executable(VirtualShadowBudgetTests "tests/VirtualShadowBudgetTests.cpp")
add_executable(BoundedSpscQueueTests "tests/BoundedSpscQueueTests.cpp")
add_executable(TaskSchedulerManagerTests "tests/TaskSchedulerManagerTests.cpp")
target_link_libraries(TaskSchedulerManagerTests PRIVATE ${PROJECT_NAME})
target_compile_features(TaskSchedulerManagerTests PRIVATE cxx_std_23)

add_executable(AsyncStateGraphTests "tests/AsyncStateGraphTests.cpp")
target_link_libraries(AsyncStateGraphTests PRIVATE ${PROJECT_NAME})
target_compile_features(AsyncStateGraphTests PRIVATE cxx_std_23)
target_include_directories(AsyncStateGraphTests PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
target_include_directories(BoundedSpscQueueTests PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
target_compile_features(BoundedSpscQueueTests PRIVATE cxx_std_23)
add_executable(CLodCoordinatorTests "tests/CLodCoordinatorTests.cpp")
add_executable(StatisticalSamplerTests "tests/StatisticalSamplerTests.cpp")
target_compile_features(StatisticalSamplerTests PRIVATE cxx_std_23)
target_compile_definitions(StatisticalSamplerTests PRIVATE
    STATISTICAL_SAMPLER_TEST_CONFIG="${BASICRENDERER_SOURCE_ROOT}/tests/data/statistical_sampler.json")
target_link_libraries(StatisticalSamplerTests PRIVATE ${PROJECT_NAME})
target_include_directories(CLodCoordinatorTests PRIVATE
    "${BASICRENDERER_SOURCE_ROOT}/include"
    "${BASICRENDERER_SOURCE_ROOT}/src"
    "${BASICRENDERER_REPO_ROOT}/OpenRenderGraph/include")
target_compile_features(CLodCoordinatorTests PRIVATE cxx_std_23)
target_link_libraries(CLodCoordinatorTests PRIVATE ${PROJECT_NAME})

set(BASICRENDERER_IMGUI_SOURCE_VERSION "1.91.9" CACHE STRING "ImGui source release used when vcpkg's transient build tree is unavailable.")
set(_basicrenderer_imgui_vulkan_backend_candidates)
if(DEFINED VCPKG_INSTALLED_DIR)
    file(GLOB _basicrenderer_imgui_vulkan_backend_candidates CONFIGURE_DEPENDS
        "${VCPKG_INSTALLED_DIR}/vcpkg/blds/imgui/src/*/backends/imgui_impl_vulkan.cpp")
endif()
if(NOT _basicrenderer_imgui_vulkan_backend_candidates)
    file(GLOB _basicrenderer_imgui_vulkan_backend_candidates CONFIGURE_DEPENDS
        "${CMAKE_BINARY_DIR}/vcpkg_installed/vcpkg/blds/imgui/src/*/backends/imgui_impl_vulkan.cpp")
endif()
if(NOT _basicrenderer_imgui_vulkan_backend_candidates)
    file(GLOB _basicrenderer_imgui_vulkan_backend_candidates CONFIGURE_DEPENDS
        "${BASICRENDERER_REPO_ROOT}/out/build/*/vcpkg_installed/vcpkg/blds/imgui/src/*/backends/imgui_impl_vulkan.cpp")
endif()
if(NOT _basicrenderer_imgui_vulkan_backend_candidates)
    set(_basicrenderer_imgui_archive "${CMAKE_BINARY_DIR}/_deps/ocornut-imgui-v${BASICRENDERER_IMGUI_SOURCE_VERSION}.tar.gz")
    set(_basicrenderer_imgui_source_dir "${CMAKE_BINARY_DIR}/_deps/imgui-${BASICRENDERER_IMGUI_SOURCE_VERSION}")

    set(_basicrenderer_imgui_cached_archives)
    if(DEFINED ENV{VCPKG_DOWNLOADS})
        list(APPEND _basicrenderer_imgui_cached_archives "$ENV{VCPKG_DOWNLOADS}/ocornut-imgui-v${BASICRENDERER_IMGUI_SOURCE_VERSION}.tar.gz")
    endif()
    if(DEFINED ENV{LOCALAPPDATA})
        list(APPEND _basicrenderer_imgui_cached_archives "$ENV{LOCALAPPDATA}/vcpkg/downloads/ocornut-imgui-v${BASICRENDERER_IMGUI_SOURCE_VERSION}.tar.gz")
    endif()

    foreach(_basicrenderer_imgui_cached_archive IN LISTS _basicrenderer_imgui_cached_archives)
        if(EXISTS "${_basicrenderer_imgui_cached_archive}")
            set(_basicrenderer_imgui_archive "${_basicrenderer_imgui_cached_archive}")
            break()
        endif()
    endforeach()

    if(NOT EXISTS "${_basicrenderer_imgui_archive}")
        file(DOWNLOAD
            "https://github.com/ocornut/imgui/archive/refs/tags/v${BASICRENDERER_IMGUI_SOURCE_VERSION}.tar.gz"
            "${_basicrenderer_imgui_archive}"
            EXPECTED_HASH SHA512=c9393bd9f6b49b036ad6ab3ba4d972876c6f60ce7f5c13a7a56ff11b3559ea3211b0caa03eed10b4f4fbe9c371e14f7f24866bd476652f543f3ed3aa878ea930
            TLS_VERIFY ON
            STATUS _basicrenderer_imgui_download_status)
        list(GET _basicrenderer_imgui_download_status 0 _basicrenderer_imgui_download_result)
        if(NOT _basicrenderer_imgui_download_result EQUAL 0)
            list(GET _basicrenderer_imgui_download_status 1 _basicrenderer_imgui_download_message)
            message(FATAL_ERROR "BasicRenderer: failed to download ImGui ${BASICRENDERER_IMGUI_SOURCE_VERSION} source archive: ${_basicrenderer_imgui_download_message}")
        endif()
    endif()

    if(NOT EXISTS "${_basicrenderer_imgui_source_dir}/backends/imgui_impl_vulkan.cpp")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_basicrenderer_imgui_archive}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/_deps"
            RESULT_VARIABLE _basicrenderer_imgui_extract_result)
        if(NOT _basicrenderer_imgui_extract_result EQUAL 0)
            message(FATAL_ERROR "BasicRenderer: failed to extract ImGui source archive '${_basicrenderer_imgui_archive}'.")
        endif()
    endif()

    if(EXISTS "${_basicrenderer_imgui_source_dir}/backends/imgui_impl_vulkan.cpp")
        list(APPEND _basicrenderer_imgui_vulkan_backend_candidates
            "${_basicrenderer_imgui_source_dir}/backends/imgui_impl_vulkan.cpp")
    endif()
endif()
if(_basicrenderer_imgui_vulkan_backend_candidates)
    list(SORT _basicrenderer_imgui_vulkan_backend_candidates)
    list(GET _basicrenderer_imgui_vulkan_backend_candidates 0 _basicrenderer_imgui_vulkan_backend)
    target_sources(${PROJECT_NAME} PRIVATE "${_basicrenderer_imgui_vulkan_backend}")
    set_source_files_properties("${_basicrenderer_imgui_vulkan_backend}" PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_DEFINITIONS "IMGUI_IMPL_VULKAN_NO_PROTOTYPES;VK_USE_PLATFORM_WIN32_KHR"
        COMPILE_OPTIONS "/w")
else()
    message(FATAL_ERROR "BasicRenderer: imgui_impl_vulkan.cpp source was not found. Vulkan ImGui must compile the backend locally with IMGUI_IMPL_VULKAN_NO_PROTOTYPES instead of using the prebuilt imgui library backend.")
endif()

target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h")

if (CMAKE_VERSION VERSION_GREATER 3.14)
  set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 23)
  set_property(TARGET ${BASICRENDERER_DEMO_TARGET} PROPERTY CXX_STANDARD 23)
  set_property(TARGET ShaderPreprocessTests PROPERTY CXX_STANDARD 23)
  set_property(TARGET SkeletonArtifactCacheTests PROPERTY CXX_STANDARD 23)
endif()

# br_generate_header(
#   TARGET <target to depend on>
#   NAME   <BaseName>                      # e.g. BuiltinResources (no extension)
#   JSON   <path/to/Generator.json>
#   OUT_DIR            <dir for .h>        # default: ${CMAKE_SOURCE_DIR}/BasicRenderer/generated
#   SHADER_OUT_DIR     <dir for .hlsli>    # default: ${CMAKE_SOURCE_DIR}/BasicRenderer/shaders/Generated
#   CODEGEN_TARGET     <target name>       # default: resource_codegen
#   NO_HLSL                                # optional: don't generate .hlsli
# )

set_target_properties(resource_codegen PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON )

function(br_generate_header)
  set(options NO_HLSL)
  set(oneValueArgs TARGET NAME JSON OUT_DIR SHADER_OUT_DIR CODEGEN_TARGET)
  cmake_parse_arguments(BRG "${options}" "${oneValueArgs}" "" ${ARGV})

  if(NOT BRG_TARGET) 
  message(FATAL_ERROR "br_generate_header: TARGET is required") 
  endif()
  if(NOT BRG_NAME)   
  message(FATAL_ERROR "br_generate_header: NAME is required")   
  endif()
  if(NOT BRG_JSON)   
  message(FATAL_ERROR "br_generate_header: JSON is required")   
  endif()

  # Defaults (match your existing layout)
  if(NOT BRG_OUT_DIR)
        set(BRG_OUT_DIR "${BASICRENDERER_SOURCE_ROOT}/generated")
  endif()
  if(NOT BRG_SHADER_OUT_DIR)
        set(BRG_SHADER_OUT_DIR "${BASICRENDERER_SOURCE_ROOT}/shaders/Generated")
  endif()
  if(NOT BRG_CODEGEN_TARGET)
    set(BRG_CODEGEN_TARGET resource_codegen)
  endif()

  # Paths
  set(_hdr "${BRG_OUT_DIR}/${BRG_NAME}.h")
  if(NOT BRG_NO_HLSL)
    set(_hlsli "${BRG_SHADER_OUT_DIR}/${BRG_NAME}.hlsli")
  endif()

  # Ensure output dirs exist at configure time (nice when building in a fresh tree)
  file(MAKE_DIRECTORY "${BRG_OUT_DIR}")
  if(_hlsli)
    file(MAKE_DIRECTORY "${BRG_SHADER_OUT_DIR}")
  endif()

  # Build the command and outputs
  set(_outputs "${_hdr}")
  if(_hlsli)
    list(APPEND _outputs "${_hlsli}")
  endif()

  # Use the built codegen executable path; also depend on the target + json
  add_custom_command(
    OUTPUT ${_outputs}
    COMMAND $<TARGET_FILE:${BRG_CODEGEN_TARGET}>
            "${BRG_JSON}"
            "${_hdr}"
            "$<IF:$<BOOL:${_hlsli}>,${_hlsli},>"
    DEPENDS ${BRG_CODEGEN_TARGET} "${BRG_JSON}"
    COMMENT "Generating ${BRG_NAME} from ${BRG_JSON}"
    VERBATIM
    USES_TERMINAL
  )

  # A named custom target to group the generation step
  set(_gen_target "Generate_${BRG_NAME}")
  add_custom_target(${_gen_target} DEPENDS ${_outputs})

  # Hook into the consuming target and include path
  add_dependencies(${BRG_TARGET} ${_gen_target})
  target_include_directories(${BRG_TARGET} PRIVATE "${BRG_OUT_DIR}")

endfunction()

br_generate_header(
  TARGET ${PROJECT_NAME}
  NAME   BuiltinResources
    JSON   "${BASICRENDERER_SOURCE_ROOT}/Generators/Templates/BuiltinResources.h.json"
    OUT_DIR        "${BASICRENDERER_SOURCE_ROOT}/generated"
    SHADER_OUT_DIR "${BASICRENDERER_SOURCE_ROOT}/shaders/Generated"
  NO_HLSL
)

br_generate_header(
  TARGET ${PROJECT_NAME}
  NAME   BuiltinRenderPasses
    JSON   "${BASICRENDERER_SOURCE_ROOT}/Generators/Templates/BuiltinRenderPasses.h.json"
    OUT_DIR        "${BASICRENDERER_SOURCE_ROOT}/generated"
    SHADER_OUT_DIR "${BASICRENDERER_SOURCE_ROOT}/shaders/Generated"
  NO_HLSL
)

#set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi") # For MSVC
#set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

function(add_symlink_or_copy_per_config source destination IS_DIRECTORY target_name)
    cmake_parse_arguments(_args "" "" "IS_DIRECTORY;TARGET_NAME" ${ARGN})

    if (NOT target_name)
        get_filename_component(target_name "${destination}" NAME)
    endif()

    if (IS_DIRECTORY)
        add_custom_target("${target_name}_sync"
            COMMENT "Syncing directory ${destination} <- ${source}"
            VERBATIM
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${destination}"
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy_directory,create_symlink> "${source}" "${destination}"
        )
    else()
        add_custom_target("${target_name}_sync"
            COMMENT "Syncing file ${destination} <- ${source}"
            VERBATIM
            COMMAND ${CMAKE_COMMAND} -E remove "${destination}"
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy,create_symlink> "${source}" "${destination}"
        )
    endif()
    add_dependencies(${PROJECT_NAME} "${target_name}_sync")
    add_dependencies(${BASICRENDERER_DEMO_TARGET} "${target_name}_sync")
endfunction()

set(SHADERS_SOURCE_DIR "${BASICRENDERER_SOURCE_ROOT}/shaders")
set(SHADERS_DEST_DIR "${BASICRENDERER_OUTPUT_ROOT}/shaders")

add_symlink_or_copy_per_config(
    "${SHADERS_SOURCE_DIR}"
    "${SHADERS_DEST_DIR}"
    TRUE  # IS_DIRECTORY
    "shaders"
)

set(MODELS_SRC_DIR "${BASICRENDERER_ASSETS_ROOT}/models")
set(MODELS_DEST_PATH "${BASICRENDERER_OUTPUT_ROOT}/models")

add_symlink_or_copy_per_config(
    "${MODELS_SRC_DIR}"
    "${MODELS_DEST_PATH}"
    TRUE  # IS_DIRECTORY
    "models"
)

set(TEXTURES_SRC_DIR "${BASICRENDERER_ASSETS_ROOT}/textures")
set(TEXTURES_DEST_PATH "${BASICRENDERER_OUTPUT_ROOT}/textures")

add_symlink_or_copy_per_config(
    "${TEXTURES_SRC_DIR}"
    "${TEXTURES_DEST_PATH}"
    TRUE  # IS_DIRECTORY
    "textures"
)

set(BUILTIN_TEXTURES_SRC_DIR "${BASICRENDERER_SOURCE_ROOT}/BuiltinTextures")
set(BUILTIN_TEXTURES_DEST_PATH "${BASICRENDERER_OUTPUT_ROOT}/BuiltinTextures")

add_symlink_or_copy_per_config(
    "${BUILTIN_TEXTURES_SRC_DIR}"
    "${BUILTIN_TEXTURES_DEST_PATH}"
    TRUE  # IS_DIRECTORY
    "builtin_textures"
)


set(tex_dir "${BASICRENDERER_OUTPUT_ROOT}/Textures")
set(tex_link "${BASICRENDERER_OUTPUT_ROOT}/textures")

INSTALL(CODE "execute_process( \
    COMMAND ${CMAKE_COMMAND} -E create_symlink \
    ${tex_dir} \
    ${tex_link}   \
    )"
)

set(COMPILER_BINARY_DIR "${BASICRENDERER_SOURCE_ROOT}/dxcompiler.dll")
set(COMPILER_DEST_FILE "${BASICRENDERER_OUTPUT_ROOT}/dxcompiler.dll")

add_symlink_or_copy_per_config(
    "${COMPILER_BINARY_DIR}"
    "${COMPILER_DEST_FILE}"
    FALSE  # IS_DIRECTORY
    "dxcompiler_dll"
)

set(SIGNING_BINARY_DIR "${BASICRENDERER_SOURCE_ROOT}/dxil.dll")
set(SIGNING_DEST_FILE "${BASICRENDERER_OUTPUT_ROOT}/dxil.dll")

add_symlink_or_copy_per_config(
    "${SIGNING_BINARY_DIR}"
    "${SIGNING_DEST_FILE}"
    FALSE  # IS_DIRECTORY
    "dxil_dll"
)

set(PIX_BINARY_DIR "${BASICRENDERER_SOURCE_ROOT}/WinPixEventRuntime.dll")
set(PIX_DEST_FILE "${BASICRENDERER_OUTPUT_ROOT}/WinPixEventRuntime.dll")

add_symlink_or_copy_per_config(
    "${PIX_BINARY_DIR}"
    "${PIX_DEST_FILE}"
    FALSE  # IS_DIRECTORY
    "winpix_dll"
)

set(D3D_SRC_DIR "${BASICRENDERER_SOURCE_ROOT}/D3D")
set(D3D_DEST_PATH "${BASICRENDERER_OUTPUT_ROOT}/D3D")

add_symlink_or_copy_per_config(
    "${D3D_SRC_DIR}"
    "${D3D_DEST_PATH}"
    TRUE  # IS_DIRECTORY
    "d3d"
)

set(SL_BINARY_DIR "${BASICRENDERER_SOURCE_ROOT}/sl.interposer.dll")
set(SL_DEST_FILE "${BASICRENDERER_OUTPUT_ROOT}/sl.interposer.dll")

add_symlink_or_copy_per_config(
    "${SL_BINARY_DIR}"
    "${SL_DEST_FILE}"
    FALSE  # IS_DIRECTORY
    "sl_interposer_dll"
)

set(NVSL_DIR "${BASICRENDERER_SOURCE_ROOT}/NVSL")
set(NVSL_FILE "${BASICRENDERER_OUTPUT_ROOT}/NVSL")

add_symlink_or_copy_per_config(
    "${NVSL_DIR}"
    "${NVSL_FILE}"
    TRUE  # IS_DIRECTORY
    "NVSL_DIR"
)

set(FFX_BIN_DIR "${BASICRENDERER_SOURCE_ROOT}/FFX/amd_fidelityfx_dx12.dll")
set(FFX_BIN_FILE "${BASICRENDERER_OUTPUT_ROOT}/amd_fidelityfx_dx12.dll")

add_symlink_or_copy_per_config(
    "${FFX_BIN_DIR}"
    "${FFX_BIN_FILE}"
    FALSE  # IS_DIRECTORY
    "FFX_dx12"
)

set(FFX_VK_BIN_DIR "${BASICRENDERER_SOURCE_ROOT}/FFX/amd_fidelityfx_vk.dll")
set(FFX_VK_BIN_FILE "${BASICRENDERER_OUTPUT_ROOT}/amd_fidelityfx_vk.dll")

add_symlink_or_copy_per_config(
    "${FFX_VK_BIN_DIR}"
    "${FFX_VK_BIN_FILE}"
    FALSE  # IS_DIRECTORY
    "FFX_vk"
)

set(FFX_BACKEND_DIR "${BASICRENDERER_SOURCE_ROOT}/FFX/ffx_backend_dx12_x64drel.dll")
set(FFX_BACKEND_FILE "${BASICRENDERER_OUTPUT_ROOT}/ffx_backend_dx12_x64drel.dll")

add_symlink_or_copy_per_config(
    "${FFX_BACKEND_DIR}"
    "${FFX_BACKEND_FILE}"
    FALSE  # IS_DIRECTORY
    "FFX_backend_dx12"
)

set(FFX_BACKEND_VK_DIR "${BASICRENDERER_SOURCE_ROOT}/FFX/ffx_backend_vk_x64drel.dll")
set(FFX_BACKEND_VK_FILE "${BASICRENDERER_OUTPUT_ROOT}/ffx_backend_vk_x64drel.dll")

add_symlink_or_copy_per_config(
    "${FFX_BACKEND_VK_DIR}"
    "${FFX_BACKEND_VK_FILE}"
    FALSE  # IS_DIRECTORY
    "FFX_backend_vk"
)

set(FFX_SSSR_DIR "${BASICRENDERER_SOURCE_ROOT}/FFX/ffx_sssr_x64drel.dll")
set(FFX_SSSR_FILE "${BASICRENDERER_OUTPUT_ROOT}/ffx_sssr_x64drel.dll")

add_symlink_or_copy_per_config(
    "${FFX_SSSR_DIR}"
    "${FFX_SSSR_FILE}"
    FALSE  # IS_DIRECTORY
    "FFX_sssr_dx12"
)

set(FFX_DENOISER_DIR "${BASICRENDERER_SOURCE_ROOT}/FFX/ffx_denoiser_x64drel.dll")
set(FFX_DENOISER_FILE "${BASICRENDERER_OUTPUT_ROOT}/ffx_denoiser_x64drel.dll")

add_symlink_or_copy_per_config(
    "${FFX_DENOISER_DIR}"
    "${FFX_DENOISER_FILE}"
    FALSE  # IS_DIRECTORY
    "FFX_denoiser_dx12"
)

set(HLSL_PARSER_DIR "${BASICRENDERER_SOURCE_ROOT}/hlsl.dll")
set(HLSL_PARSER_FILE "${BASICRENDERER_OUTPUT_ROOT}/hlsl.dll")

add_symlink_or_copy_per_config(
    "${HLSL_PARSER_DIR}"
    "${HLSL_PARSER_FILE}"
    FALSE  # IS_DIRECTORY
    "HLSL_Parser"
)

set(USD_DIR "${BASICRENDERER_USD_ROOT}/usd_$<IF:$<CONFIG:Debug>,dbg,rel>")
 
add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_usd.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_usd.dll"
    FALSE  # IS_DIRECTORY
    "USD_USD"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_sdf.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_sdf.dll"
    FALSE  # IS_DIRECTORY
    "USD_SDF"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_tf.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_tf.dll"
    FALSE  # IS_DIRECTORY
    "USD_TF"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_kind.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_kind.dll"
    FALSE  # IS_DIRECTORY
    "USD_KIND"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_pcp.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_pcp.dll"
    FALSE  # IS_DIRECTORY
    "USD_PCP"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_ar.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_ar.dll"
    FALSE  # IS_DIRECTORY
    "USD_AR"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_plug.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_plug.dll"
    FALSE  # IS_DIRECTORY
    "USD_PLUG"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_vt.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_vt.dll"
    FALSE  # IS_DIRECTORY
    "USD_VT"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_ts.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_ts.dll"
    FALSE  # IS_DIRECTORY
    "USD_TS"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_work.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_work.dll"
    FALSE  # IS_DIRECTORY
    "USD_WORK"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_gf.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_gf.dll"
    FALSE  # IS_DIRECTORY
    "USD_GF"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_trace.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_trace.dll"
    FALSE  # IS_DIRECTORY
    "USD_TRACE"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_arch.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_arch.dll"
    FALSE  # IS_DIRECTORY
    "USD_ARCH"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_js.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_js.dll"
    FALSE  # IS_DIRECTORY
    "USD_JS"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_usdShade.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_usdShade.dll"
    FALSE  # IS_DIRECTORY
    "USD_USDSHADE"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_usdGeom.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_usdGeom.dll"
    FALSE  # IS_DIRECTORY
    "USD_USDGEOM"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_usdSkel.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_usdSkel.dll"
    FALSE  # IS_DIRECTORY
    "USD_USDSKEL"
)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd_sdr.dll"
    "${BASICRENDERER_OUTPUT_ROOT}/usd_sdr.dll"
    FALSE  # IS_DIRECTORY
    "USD_SDR"
)

set(_tbb_config_sync_stamp "${CMAKE_CURRENT_BINARY_DIR}/runtime_sync/$<CONFIG>/tbb_config.stamp")
add_custom_command(
    OUTPUT "${_tbb_config_sync_stamp}"
    COMMENT "Syncing config-specific TBB runtime"
    VERBATIM
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/runtime_sync/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${BASICRENDERER_USD_ROOT}/usd_$<IF:$<CONFIG:Debug>,dbg,rel>/bin"
        "-DDESTINATION_DIR=${BASICRENDERER_OUTPUT_ROOT}"
        "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_LIST_DIR}/cmake/SyncTbbRuntime.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${_tbb_config_sync_stamp}"
    DEPENDS "${BASICRENDERER_USD_ROOT}/usd_$<IF:$<CONFIG:Debug>,dbg,rel>/bin/tbb.dll"
)
add_custom_target(tbb_config_sync DEPENDS "${_tbb_config_sync_stamp}")
add_dependencies(${PROJECT_NAME} tbb_config_sync)
add_dependencies(${BASICRENDERER_DEMO_TARGET} tbb_config_sync)

add_symlink_or_copy_per_config(
    "${USD_DIR}/lib/usd"
    "${BASICRENDERER_OUTPUT_ROOT}/usd"
    TRUE  # IS_DIRECTORY
    "USD_PLUGINS"
)

if (MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor)
  target_compile_options(${BASICRENDERER_DEMO_TARGET} PRIVATE /Zc:preprocessor)
  target_compile_options(ShaderPreprocessTests PRIVATE /Zc:preprocessor)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE /WX)
    target_compile_options(${BASICRENDERER_DEMO_TARGET} PRIVATE /WX)
    target_compile_options(ShaderPreprocessTests PRIVATE /WX)
  endif()
  target_compile_options(${PROJECT_NAME} PRIVATE 
      /wd4828 
      /wd4514 
      /wd5038 
      /wd4100 
      /wd5219 
      /wd4625 
      /wd4626 
      /wd4820 
      /wd4365 
      /wd4464 
      /wd4062 
      /wd5026 
      /wd5027 
      /wd5246
      /wd4191
      /wd4061
      /wd4623
      /wd4710
      /wd4324
      /wd5267
      /wd5045
  )
  target_compile_options(${BASICRENDERER_DEMO_TARGET} PRIVATE 
      /wd4828 
      /wd4514 
      /wd5038 
      /wd4100 
      /wd5219 
      /wd4625 
      /wd4626 
      /wd4820 
      /wd4365 
      /wd4464 
      /wd4062 
      /wd5026 
      /wd5027 
      /wd5246
      /wd4191
      /wd4061
      /wd4623
      /wd4710
      /wd4324
      /wd5267
      /wd5045
  )
  #target_compile_options(${PROJECT_NAME} PRIVATE /Wall)
  target_compile_options(${PROJECT_NAME} PRIVATE /external:anglebrackets /external:W0) # Disable warnings in external headers
  target_compile_options(${BASICRENDERER_DEMO_TARGET} PRIVATE /external:anglebrackets /external:W0)
  # Clang (via clang-cl) warns about MSVC-specific flags it ignores; suppress those warnings-as-errors
  target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:Clang>:
      -Wno-unused-command-line-argument
      -Wno-unused-function
      -Wno-error=switch
      -Wno-error=reorder-ctor
      -Wno-error=unused-variable
      -Wno-error=unused-but-set-variable
      -Wno-error=unused-private-field
      -Wno-error=misleading-indentation
      -Wno-error=nonportable-include-path
      -Wno-error=inconsistent-missing-override
      -Wno-error=format
      -Wno-error=implicit-float-conversion
      -Wno-error=missing-braces
      -Wno-error=braced-scalar-init
      -Wno-error=pessimizing-move
      -Wno-error=self-assign-field
      -Wno-error=unused-lambda-capture
  >)
  target_compile_options(${BASICRENDERER_DEMO_TARGET} PRIVATE $<$<CXX_COMPILER_ID:Clang>:
      -Wno-unused-command-line-argument
      -Wno-unused-function
      -Wno-error=switch
      -Wno-error=reorder-ctor
      -Wno-error=unused-variable
      -Wno-error=unused-but-set-variable
      -Wno-error=unused-private-field
      -Wno-error=misleading-indentation
      -Wno-error=nonportable-include-path
      -Wno-error=inconsistent-missing-override
      -Wno-error=format
      -Wno-error=implicit-float-conversion
      -Wno-error=missing-braces
      -Wno-error=braced-scalar-init
      -Wno-error=pessimizing-move
      -Wno-error=self-assign-field
      -Wno-error=unused-lambda-capture
  >)
  target_compile_options(ShaderPreprocessTests PRIVATE
      /wd4828
      /wd4514
      /wd5038
      /wd4100
      /wd5219
      /wd4625
      /wd4626
      /wd4820
      /wd4365
      /wd4464
      /wd4062
      /wd5026
      /wd5027
      /wd5246
      /wd4191
      /wd4061
      /wd4623
      /wd4710
      /wd4324
      /wd5267
      /wd5045
  )
  target_compile_options(ShaderPreprocessTests PRIVATE /external:anglebrackets /external:W0)
  target_compile_options(ShaderPreprocessTests PRIVATE $<$<CXX_COMPILER_ID:Clang>:
      -Wno-unused-command-line-argument
      -Wno-unused-function
      -Wno-error=switch
      -Wno-error=reorder-ctor
      -Wno-error=unused-variable
      -Wno-error=unused-but-set-variable
      -Wno-error=unused-private-field
      -Wno-error=misleading-indentation
      -Wno-error=nonportable-include-path
      -Wno-error=inconsistent-missing-override
      -Wno-error=format
      -Wno-error=implicit-float-conversion
      -Wno-error=missing-braces
      -Wno-error=braced-scalar-init
      -Wno-error=pessimizing-move
      -Wno-error=self-assign-field
      -Wno-error=unused-lambda-capture
  >)
endif()

target_include_directories(${PROJECT_NAME} BEFORE PUBLIC ${DIRECTX_HEADERS_INCLUDE_DIR} generated/)
target_include_directories(${PROJECT_NAME} PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
if(BASICRENDERER_DETECTED_VULKAN_INCLUDE_DIR)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${BASICRENDERER_DETECTED_VULKAN_INCLUDE_DIR}")
endif()
target_include_directories(${PROJECT_NAME} BEFORE PUBLIC "${BASICRENDERER_USD_ROOT}/usd_$<IF:$<CONFIG:Debug>,dbg,rel>/include/")
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${BASICRENDERER_REPO_ROOT}/ThirdParty")
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${BASICRENDERER_REPO_ROOT}/volk")
target_include_directories(${BASICRENDERER_DEMO_TARGET} BEFORE PRIVATE "${BASICRENDERER_REPO_ROOT}/ThirdParty")
target_include_directories(${BASICRENDERER_DEMO_TARGET} BEFORE PRIVATE "${BASICRENDERER_REPO_ROOT}/volk")
target_include_directories(ShaderPreprocessTests BEFORE PRIVATE "${BASICRENDERER_SOURCE_ROOT}" "${BASICRENDERER_SOURCE_ROOT}/src" include/)
# Heck off Microsoft
add_compile_definitions(NOMINMAX)
target_compile_definitions(ShaderPreprocessTests PRIVATE NOMINMAX)

# Disable warnings for signed/unsigned mismatch
add_compile_options("/wd4267")

# Link-only: /DEBUG on Debug/RelWithDebInfo.
set(BASICRENDERER_MSVC_PDB_LINK_OPTIONS
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:/DEBUG>
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND NOT CMAKE_CROSSCOMPILING)
    # Use larger PDB pages for native MSVC to avoid "limit exceeded for program
    # database" when debug symbols grow large.
    list(APPEND BASICRENDERER_MSVC_PDB_LINK_OPTIONS
        $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:/PDBPAGESIZE:16384>
    )
endif()

target_link_options(${PROJECT_NAME} PRIVATE
    ${BASICRENDERER_MSVC_PDB_LINK_OPTIONS}
)
target_link_options(${BASICRENDERER_DEMO_TARGET} PRIVATE
    ${BASICRENDERER_MSVC_PDB_LINK_OPTIONS}
)
target_link_options(ShaderPreprocessTests PRIVATE
    ${BASICRENDERER_MSVC_PDB_LINK_OPTIONS}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  # first, define integer codes for each build flavor
  BUILD_TYPE_DEBUG=0
  BUILD_TYPE_RELEASE=1
  BUILD_TYPE_RELEASE_DEBUG=2

  # then set BUILD_TYPE to one of those codes based on CMAKE_BUILD_TYPE:
  $<$<CONFIG:Debug>:BUILD_TYPE=BUILD_TYPE_DEBUG>
  $<$<CONFIG:Release>:BUILD_TYPE=BUILD_TYPE_RELEASE>
  $<$<CONFIG:RelWithDebInfo>:BUILD_TYPE=BUILD_TYPE_RELEASE_DEBUG>
    BASICRENDERER_HAS_DIRECTSTORAGE=$<IF:$<BOOL:${BASICRENDERER_HAS_DIRECTSTORAGE}>,1,0>
    BASICRENDERER_ENABLE_NVPERF=$<IF:$<AND:$<BOOL:${BASICRENDERER_ENABLE_NVPERF}>,$<TARGET_EXISTS:NVPerf::Host>>,1,0>
)
target_compile_definitions(${BASICRENDERER_DEMO_TARGET} PRIVATE
  BUILD_TYPE_DEBUG=0
  BUILD_TYPE_RELEASE=1
  BUILD_TYPE_RELEASE_DEBUG=2
  $<$<CONFIG:Debug>:BUILD_TYPE=BUILD_TYPE_DEBUG>
  $<$<CONFIG:Release>:BUILD_TYPE=BUILD_TYPE_RELEASE>
  $<$<CONFIG:RelWithDebInfo>:BUILD_TYPE=BUILD_TYPE_RELEASE_DEBUG>
)
target_compile_definitions(ShaderPreprocessTests PRIVATE
  BUILD_TYPE_DEBUG=0
  BUILD_TYPE_RELEASE=1
  BUILD_TYPE_RELEASE_DEBUG=2
  $<$<CONFIG:Debug>:BUILD_TYPE=BUILD_TYPE_DEBUG>
  $<$<CONFIG:Release>:BUILD_TYPE=BUILD_TYPE_RELEASE>
  $<$<CONFIG:RelWithDebInfo>:BUILD_TYPE=BUILD_TYPE_RELEASE_DEBUG>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  if(TARGET Microsoft::DirectXTex)
    get_target_property(_basicrenderer_directxtex_debug_lib Microsoft::DirectXTex IMPORTED_LOCATION_DEBUG)
    get_target_property(_basicrenderer_directxtex_release_lib Microsoft::DirectXTex IMPORTED_LOCATION_RELEASE)
    if(_basicrenderer_directxtex_release_lib
        AND _basicrenderer_directxtex_debug_lib
        AND NOT "${_basicrenderer_directxtex_debug_lib}" STREQUAL "${_basicrenderer_directxtex_release_lib}")
      set_property(TARGET Microsoft::DirectXTex PROPERTY MAP_IMPORTED_CONFIG_DEBUG Release)
      set_property(TARGET Microsoft::DirectXTex PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
      set_property(TARGET Microsoft::DirectXTex PROPERTY IMPORTED_LOCATION_DEBUG "${_basicrenderer_directxtex_release_lib}")
    endif()
  endif()
  if(TARGET assimp::assimp)
    get_target_property(_basicrenderer_assimp_debug_lib assimp::assimp IMPORTED_LOCATION_DEBUG)
    get_target_property(_basicrenderer_assimp_release_lib assimp::assimp IMPORTED_LOCATION_RELEASE)
    if(_basicrenderer_assimp_release_lib
        AND _basicrenderer_assimp_debug_lib
        AND NOT "${_basicrenderer_assimp_debug_lib}" STREQUAL "${_basicrenderer_assimp_release_lib}")
      set_property(TARGET assimp::assimp PROPERTY MAP_IMPORTED_CONFIG_DEBUG Release)
      set_property(TARGET assimp::assimp PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
      set_property(TARGET assimp::assimp PROPERTY IMPORTED_LOCATION_DEBUG "${_basicrenderer_assimp_release_lib}")
    endif()
  endif()
endif()

target_link_libraries(${PROJECT_NAME} 
PRIVATE 
    dxgi.lib 
    bcrypt.lib
    lz4::lz4
    xatlas
    geometry-central
    spdlog::spdlog_header_only 
    Microsoft::DirectXTex 
    imgui::imgui 
    Microsoft.GSL::GSL
    meshoptimizer
    Eigen3::Eigen
    embree
    assimp::assimp 
    flecs::flecs 
    flecs::flecs_static 
    unofficial::tree-sitter::tree-sitter
    Streamline::headers
    Pix::headers
    usd
    sdf
    tf
    vt
    arch
    plug
    ar
    usdShade
    usdSkel
    implot::implot
    OpenRenderGraph::OpenRenderGraph
    BasicScene::BasicScene
    BasicRHI::BasicRHI
    ORGModuleServices::ORGModuleServices
    Boost::container_hash
    Boost::interprocess
    TBB::tbb
    Tracy::TracyClient
    unofficial::sqlite3::sqlite3
    slang::slang
    "${BASICRENDERER_SOURCE_ROOT}/WinPixEventRuntime.lib" 
    "${BASICRENDERER_SOURCE_ROOT}/sl.interposer.lib" 
    "${BASICRENDERER_SOURCE_ROOT}/ffx_sssr_x64drel.lib"
    "${BASICRENDERER_SOURCE_ROOT}/hlsl_parser.lib"
)

target_link_libraries(${PROJECT_NAME}
PUBLIC
    spdlog::spdlog_header_only
    Microsoft::DirectX-Headers
    meshoptimizer
    flecs::flecs
    OpenRenderGraph::OpenRenderGraph
    BasicScene::BasicScene
    BasicRHI::BasicRHI
    BasicTelemetry::Core
    nlohmann_json::nlohmann_json
)

if(TARGET BasicRenderer_DirectStorage)
    target_link_libraries(${PROJECT_NAME} PRIVATE BasicRenderer_DirectStorage)
endif()

if(BASICRENDERER_ENABLE_NVPERF)
    if(TARGET NVPerf::Host)
        target_link_libraries(${PROJECT_NAME} PRIVATE NVPerf::Host)
        target_link_libraries(${BASICRENDERER_DEMO_TARGET} PRIVATE NVPerf::Host)
        message(STATUS "BasicRenderer: NVIDIA Nsight Perf SDK integration enabled")
    else()
        message(WARNING "BasicRenderer: BASICRENDERER_ENABLE_NVPERF was requested, but NVPerf::Host is unavailable")
    endif()
endif()

target_link_libraries(${BASICRENDERER_DEMO_TARGET}
PRIVATE
    ${PROJECT_NAME}
    spdlog::spdlog_header_only
    imgui::imgui
    Pix::headers
    "${BASICRENDERER_SOURCE_ROOT}/WinPixEventRuntime.lib"
)

if(BASICRENDERER_DIRECTSTORAGE_DLL)
    set(DIRECTSTORAGE_DEST_FILE "${BASICRENDERER_OUTPUT_ROOT}/dstorage.dll")
    add_symlink_or_copy_per_config(
        "${BASICRENDERER_DIRECTSTORAGE_DLL}"
        "${DIRECTSTORAGE_DEST_FILE}"
        FALSE
        "directstorage_dll"
    )
endif()

if(BASICRENDERER_DIRECTSTORAGE_CORE_DLL)
    set(DIRECTSTORAGE_CORE_DEST_FILE "${BASICRENDERER_OUTPUT_ROOT}/dstoragecore.dll")
    add_symlink_or_copy_per_config(
        "${BASICRENDERER_DIRECTSTORAGE_CORE_DLL}"
        "${DIRECTSTORAGE_CORE_DEST_FILE}"
        FALSE
        "directstoragecore_dll"
    )
endif()

if(BASICRENDERER_ENABLE_NVPERF AND NVPERF_GRFX_HOST_DLL)
    add_custom_command(TARGET ${BASICRENDERER_DEMO_TARGET} POST_BUILD
        COMMENT "Copying NVIDIA Nsight Perf SDK host DLL"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NVPERF_GRFX_HOST_DLL}"
            "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/nvperf_grfx_host.dll"
        VERBATIM
    )
endif()

if(BASICRHI_ENABLE_RESHAPE AND (TARGET GRS.Backends.DX12.Layer OR TARGET GRS.Backends.Vulkan.Layer))
    target_link_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_BINARY_DIR}/External/lib"
    )
    target_link_directories(${BASICRENDERER_DEMO_TARGET} PRIVATE
        "${CMAKE_BINARY_DIR}/External/lib"
    )

    set(_basicrenderer_stage_reshape_debug_backend OFF)
    set(_basicrenderer_stage_reshape_waterfall_backend OFF)
    set(_basicrenderer_stage_reshape_device_commands_backend OFF)
    set(_basicrenderer_stage_reshape_loop_backend OFF)
    set(_basicrenderer_stage_reshape_export_stability_backend OFF)

    set(_basicrenderer_reshape_plugins)
    set(_basicrenderer_reshape_plugin_copy_commands)

    function(br_add_reshape_plugin plugin_target plugin_manifest)
        if(TARGET ${plugin_target})
            get_filename_component(plugin_manifest_name "${plugin_manifest}" NAME)
            list(APPEND _basicrenderer_reshape_plugins ${plugin_target})
            list(APPEND _basicrenderer_reshape_plugin_copy_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${plugin_target}>"
                    "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/$<TARGET_FILE_NAME:${plugin_target}>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${plugin_manifest}"
                    "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/${plugin_manifest_name}"
            )
            set(_basicrenderer_reshape_plugins "${_basicrenderer_reshape_plugins}" PARENT_SCOPE)
            set(_basicrenderer_reshape_plugin_copy_commands "${_basicrenderer_reshape_plugin_copy_commands}" PARENT_SCOPE)
        endif()
    endfunction()

    br_add_reshape_plugin(GRS.Features.Descriptor.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/Descriptor/Descriptor.xml")
    br_add_reshape_plugin(GRS.Features.ResourceBounds.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/ResourceBounds/ResourceBounds.xml")
    br_add_reshape_plugin(GRS.Features.Initialization.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/Initialization/Initialization.xml")
    br_add_reshape_plugin(GRS.Features.Concurrency.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/Concurrency/Concurrency.xml")
    if(_basicrenderer_stage_reshape_loop_backend)
        br_add_reshape_plugin(GRS.Features.Loop.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/Loop/Loop.xml")
    endif()
    if(_basicrenderer_stage_reshape_debug_backend)
        br_add_reshape_plugin(GRS.Features.Debug.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/Debug/Debug.xml")
    endif()
    if(_basicrenderer_stage_reshape_waterfall_backend)
        br_add_reshape_plugin(GRS.Features.Waterfall.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/Waterfall/Waterfall.xml")
    endif()
    if(_basicrenderer_stage_reshape_device_commands_backend)
        br_add_reshape_plugin(GRS.Features.DeviceCommands.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/DeviceCommands/DeviceCommands.xml")
    endif()
    if(_basicrenderer_stage_reshape_export_stability_backend)
        br_add_reshape_plugin(GRS.Features.ExportStability.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/ExportStability/ExportStability.xml")
    endif()
    br_add_reshape_plugin(GRS.Features.ExportIndices.Backend "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Features/ExportIndices/ExportIndices.xml")

    set(_basicrenderer_reshape_backend_targets ${_basicrenderer_reshape_plugins})
    set(_basicrenderer_reshape_backend_copy_commands)
    if(TARGET GRS.Backends.DX12.Layer)
        list(APPEND _basicrenderer_reshape_backend_targets GRS.Backends.DX12.Layer)
        list(APPEND _basicrenderer_reshape_backend_copy_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:GRS.Backends.DX12.Layer>"
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/$<TARGET_FILE_NAME:GRS.Backends.DX12.Layer>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:GRS.Backends.DX12.Layer>/Dependencies/DXC/GRS.dxil.dll"
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Dependencies/DXC/GRS.dxil.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:GRS.Backends.DX12.Layer>/Dependencies/DXC/GRS.dxcompiler.dll"
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Dependencies/DXC/GRS.dxcompiler.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:GRS.Backends.DX12.Layer>/Dependencies/DXC/GRS.dxilconv.dll"
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Dependencies/DXC/GRS.dxilconv.dll"
        )
    endif()
    if(TARGET GRS.Backends.Vulkan.Layer)
        list(APPEND _basicrenderer_reshape_backend_targets GRS.Backends.Vulkan.Layer)
        list(APPEND _basicrenderer_reshape_backend_copy_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:GRS.Backends.Vulkan.Layer>"
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/$<TARGET_FILE_NAME:GRS.Backends.Vulkan.Layer>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BASICRENDERER_REPO_ROOT}/GPU-Reshape/Source/Backends/Vulkan/Layer/Resources/VK_LAYER_GPUOPEN_GRS.json"
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/VK_LAYER_GPUOPEN_GRS.json"
        )
    endif()

    add_dependencies(${BASICRENDERER_DEMO_TARGET}
        ${_basicrenderer_reshape_backend_targets}
    )

    add_custom_command(TARGET ${BASICRENDERER_DEMO_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Dependencies/DXC"
        ${_basicrenderer_reshape_backend_copy_commands}
        ${_basicrenderer_reshape_plugin_copy_commands}
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/GRS.Features.Debug.Backend.dll"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/Debug.xml"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/GRS.Features.Waterfall.Backend.dll"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/Waterfall.xml"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/GRS.Features.DeviceCommands.Backend.dll"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/DeviceCommands.xml"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/GRS.Features.Loop.Backend.dll"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/Loop.xml"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/GRS.Features.ExportStability.Backend.dll"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${BASICRENDERER_DEMO_TARGET}>/Plugins/ExportStability.xml"
    )
endif()

target_link_libraries(ShaderPreprocessTests
PRIVATE
    unofficial::tree-sitter::tree-sitter
    BasicRHI::BasicRHI
    "${BASICRENDERER_SOURCE_ROOT}/hlsl_parser.lib"
)

target_link_libraries(SkeletonArtifactCacheTests PRIVATE ${PROJECT_NAME})
target_include_directories(SkeletonArtifactCacheTests PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
target_link_libraries(PipelineRecipeTests PRIVATE ${PROJECT_NAME})
target_include_directories(PipelineRecipeTests PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
target_link_libraries(MaterialEvalVariantTests PRIVATE ${PROJECT_NAME})
target_include_directories(MaterialEvalVariantTests PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
target_link_libraries(MaterialOwnershipGuardTests PRIVATE ${PROJECT_NAME})
target_include_directories(MaterialOwnershipGuardTests PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
target_compile_features(MaterialOwnershipGuardTests PRIVATE cxx_std_23)
target_link_libraries(VirtualShadowBudgetTests PRIVATE ${PROJECT_NAME})
target_include_directories(VirtualShadowBudgetTests PRIVATE "${BASICRENDERER_SOURCE_ROOT}/src")
if(MSVC)
    target_compile_options(SkeletonArtifactCacheTests PRIVATE /std:c++latest)
    target_compile_options(PipelineRecipeTests PRIVATE /std:c++latest)
    target_compile_options(MaterialEvalVariantTests PRIVATE /std:c++latest)
    target_compile_options(VirtualShadowBudgetTests PRIVATE /std:c++latest)
endif()

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME RendererBoundaryAudit
    COMMAND "${Python3_EXECUTABLE}" "${BASICRENDERER_REPO_ROOT}/scripts/Audit-RendererBoundaries.py")
add_test(NAME RendererBoundaryAuditTests
    COMMAND "${Python3_EXECUTABLE}" "${BASICRENDERER_REPO_ROOT}/scripts/TestRendererBoundaries.py")
add_test(NAME RendererBuildInputAudit
    COMMAND "${Python3_EXECUTABLE}" "${BASICRENDERER_REPO_ROOT}/scripts/Audit-RendererBuildInputs.py")
add_test(NAME ShaderPreprocessTests COMMAND ShaderPreprocessTests)
add_test(NAME SkeletonArtifactCacheTests COMMAND SkeletonArtifactCacheTests)
add_test(NAME PipelineRecipeTests COMMAND PipelineRecipeTests)
add_test(NAME MaterialEvalVariantTests COMMAND MaterialEvalVariantTests)
add_test(NAME MaterialOwnershipGuardTests COMMAND MaterialOwnershipGuardTests)
add_test(NAME VirtualShadowBudgetTests COMMAND VirtualShadowBudgetTests)
add_test(NAME BoundedSpscQueueTests COMMAND BoundedSpscQueueTests)
add_test(NAME TaskSchedulerManagerTests COMMAND TaskSchedulerManagerTests)
add_test(NAME AsyncStateGraphTests COMMAND AsyncStateGraphTests)
add_test(NAME CLodCoordinatorTests COMMAND CLodCoordinatorTests)
add_test(NAME StatisticalSamplerTests COMMAND StatisticalSamplerTests)

if(WIN32)
    # SARP tests use the deployed runtime, including optional SDK DLLs and
    # relative runtime assets. Standalone BasicRenderer uses its output root.
    set(_basicrenderer_runtime_tests
        ShaderPreprocessTests
        SkeletonArtifactCacheTests
        PipelineRecipeTests
        MaterialEvalVariantTests
        VirtualShadowBudgetTests
        BoundedSpscQueueTests
        TaskSchedulerManagerTests
        AsyncStateGraphTests
        CLodCoordinatorTests
        StatisticalSamplerTests)
    if(BASICRENDERER_TEST_RUNTIME_DIR)
        set_tests_properties(${_basicrenderer_runtime_tests} PROPERTIES
            WORKING_DIRECTORY "${BASICRENDERER_TEST_RUNTIME_DIR}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${BASICRENDERER_TEST_RUNTIME_DIR}")
    else()
        set_tests_properties(${_basicrenderer_runtime_tests} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${BASICRENDERER_OUTPUT_ROOT}")
    endif()
    # Metadata-only publication preparation tests need no deployed assets.
    foreach(_pipeline_runtime_dll WinPixEventRuntime.dll dxcompiler.dll dxil.dll hlsl.dll)
        if(EXISTS "${BASICRENDERER_SOURCE_ROOT}/${_pipeline_runtime_dll}")
            add_custom_command(TARGET PipelineRecipeTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${BASICRENDERER_SOURCE_ROOT}/${_pipeline_runtime_dll}"
                    "$<TARGET_FILE_DIR:PipelineRecipeTests>")
        endif()
    endforeach()
    set_tests_properties(PipelineRecipeTests PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:PipelineRecipeTests>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${BASICRENDERER_OUTPUT_ROOT}")
    if(BASICRENDERER_ENABLE_NVPERF AND NVPERF_GRFX_HOST_DLL)
        add_custom_command(TARGET PipelineRecipeTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NVPERF_GRFX_HOST_DLL}"
                "$<TARGET_FILE_DIR:PipelineRecipeTests>"
            VERBATIM)
    endif()
endif()
