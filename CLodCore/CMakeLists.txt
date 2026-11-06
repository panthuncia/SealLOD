# The CLod code that needs no GPU, split so a tool can link only what it uses.
#
#   CLodCacheFormat  reads and writes .clodmeta/.clodbin and decodes their pages.
#                    No USD, no D3D12, no generator. CLodEval links this alone.
#   CLodBuildCore    the hierarchy generator and voxelizer, plus the cache store.
#                    Used by CLodCacheTool and by the renderer's runtime fallback.
#
# Both are compiled once here and linked by the renderer rather than being compiled again in each
# executable. The source files stay under BasicRenderer/src, beside the code that uses them.

cmake_minimum_required(VERSION 3.16)

get_filename_component(BR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../BasicRenderer" ABSOLUTE)
set(BR_SRC "${BR_ROOT}/src")

find_package(Boost REQUIRED CONFIG COMPONENTS container_hash)
find_package(DirectX-Headers CONFIG REQUIRED)
find_package(Tracy REQUIRED CONFIG)
find_package(embree CONFIG REQUIRED)
find_package(flecs CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()
if(COMMAND basicrenderer_use_release_library_for_debug)
    basicrenderer_use_release_library_for_debug(fmt::fmt)
    basicrenderer_use_release_library_for_debug(Tracy::TracyClient)
endif()

function(clod_core_configure target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(${target} PRIVATE NOMINMAX _UNICODE UNICODE)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /Zc:preprocessor /EHsc /bigobj
            /wd4828 /wd4514 /wd5038 /wd4100 /wd5219
            /wd4625 /wd4626 /wd4820 /wd4365 /wd4464
            /wd4062 /wd5026 /wd5027 /wd5246 /wd4191
            /wd4061 /wd4623 /wd4710 /wd4324 /wd5267
            /wd5045
            /external:anglebrackets /external:W0
        )
    endif()
endfunction()

add_library(CLodCacheFormat STATIC
    "${BR_SRC}/Assets/Cache/CachePack.cpp"
    "${BR_SRC}/Assets/Cache/CachePathUtilities.cpp"
    "${BR_SRC}/Assets/Cache/ClusterLOD/CLodCache.cpp"
    "${BR_SRC}/Assets/Cache/ClusterLOD/CLodOpacityMicromapCache.cpp"
    "${BR_SRC}/Assets/Cache/ClusterLOD/CLodPageDecoder.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/DefaultCLodSettings.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/Voxels/VoxelSurfaceTrace.cpp"
)
clod_core_configure(CLodCacheFormat)
target_include_directories(CLodCacheFormat
    PUBLIC
        "${BR_ROOT}/include"
    PRIVATE
        "${BR_SRC}"
        "${BR_ROOT}"  # CLodCache.cpp includes "../shaders/Common/defines.h"
)
target_link_libraries(CLodCacheFormat
    PUBLIC
        BasicTelemetry::Core
        ${BASICRENDERER_SPDLOG_TARGET}
        Microsoft::DirectX-Headers
        meshoptimizer
        nlohmann_json::nlohmann_json
    PRIVATE
        Boost::container_hash
        Tracy::TracyClient
)

add_library(CLodBuildCore STATIC
    "${BR_SRC}/Assets/Cache/ClusterLOD/CLodCacheLoader.cpp"
    "${BR_SRC}/Assets/Cache/Skeleton/SkeletonArtifactValidation.cpp"
    "${BR_SRC}/Assets/Geometry/MeshIngestBuilder.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLOD.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODAssembly.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODClustering.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePacking.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODSkinningSidecar.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODValidation.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelFallback.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelOnly.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.cpp"
    "${BR_SRC}/Assets/GeometryProcessing/Voxels/VoxelGroupBuilder.cpp"
    "${BR_SRC}/Diagnostics/Telemetry/FrameTaskGraphTelemetry.cpp"
    "${BR_SRC}/Runtime/Scheduling/TaskSchedulerManager.cpp"
    "${BR_SRC}/VirtualGeometry/Reyes/Tessellation/ReyesTessellationGeneratedReferenceSource.cpp"
    "${BR_SRC}/VirtualGeometry/Reyes/Tessellation/ReyesTessellationTable.cpp"
)
clod_core_configure(CLodBuildCore)
target_include_directories(CLodBuildCore PRIVATE
    "${BR_SRC}"
    "${BR_ROOT}"
    "${BR_ROOT}/shaders"
    "${BR_ROOT}/generated"
)
# Headers reached through the generator's includes. Include paths only: nothing here links the RHI or graph.
foreach(_clod_core_headers BasicRHI::BasicRHI OpenRenderGraph::OpenRenderGraph)
    target_include_directories(CLodBuildCore PRIVATE $<TARGET_PROPERTY:${_clod_core_headers},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(CLodBuildCore PRIVATE $<TARGET_PROPERTY:${_clod_core_headers},INTERFACE_COMPILE_DEFINITIONS>)
endforeach()
target_link_libraries(CLodBuildCore
    PUBLIC
        CLodCacheFormat
    PRIVATE
        Boost::container_hash
        Tracy::TracyClient
        embree
        flecs::flecs_static
        lz4::lz4
        bcrypt.lib
)
# oneTBB headers with vcpkg's import library: see the note at the top of CLodCacheTool/CMakeLists.txt.
if(BASICRENDERER_VCPKG_TBB12_LIBRARY)
    target_link_libraries(CLodBuildCore PUBLIC "${BASICRENDERER_VCPKG_TBB12_LIBRARY}")
endif()
