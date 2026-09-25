# CLodCacheTool – Offline ClusterLOD cache builder (CLI)
# Shares the geometry-extraction & CLod-build sources with BasicRenderer
# but has NO GPU/D3D12 dependencies.
#
# TBB dependency note: the prebuilt OpenUSD packages can define TBB::tbb for
# their bundled TBB runtime, while vcpkg Embree uses oneTBB (whose current
# Windows import library is named tbb12).  Until OpenUSD and Embree are built
# against one shared TBB package, do not assume TBB::tbb satisfies both or
# hard-code a vcpkg triplet.  The explicit Embree-side library lookup below is
# intentionally derived from vcpkg's active installed root and triplet.

cmake_minimum_required(VERSION 3.16)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(CLodCacheTool LANGUAGES CXX)
endif()

# Paths into the BasicRenderer tree
get_filename_component(BR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../BasicRenderer" ABSOLUTE)
set(BR_SRC      "${BR_ROOT}/src")
set(BR_INCLUDE  "${BR_ROOT}/include")
set(BR_SHADERS  "${BR_ROOT}/shaders")   # for Common/defines.h

# Shared sources (compiled independently from BasicRenderer)
set(CLOD_SHARED_SOURCES
    # Geometry extractors
    "${BR_SRC}/Import/GlTFGeometryExtractor.cpp"
    "${BR_SRC}/Import/AssimpGeometryExtractor.cpp"
    "${BR_SRC}/Import/USDGeometryExtractor.cpp"
    "${BR_SRC}/Import/USDMaterialCache.cpp"
    "${BR_SRC}/Import/BRNiflyClient.cpp"

    # CLod build & cache infrastructure
    "${BR_SRC}/Import/CLodCache.cpp"
    "${BR_SRC}/Import/CLodCacheLoader.cpp"
    "${BR_SRC}/Import/SkeletonArtifactValidation.cpp"
    "${BR_SRC}/Import/DefaultCLodSettings.cpp"
    "${BR_SRC}/Mesh/MeshIngestBuilder.cpp"
    "${BR_SRC}/Mesh/ClusterLOD.cpp"
    "${BR_SRC}/Mesh/ClusterLODUtilities.cpp"
    "${BR_SRC}/Mesh/VoxelGroupBuilder.cpp"
    "${BR_SRC}/Render/GraphExtensions/ClusterLOD/ReyesTessellationTable.cpp"
    "${BR_SRC}/Render/GraphExtensions/ClusterLOD/ReyesTessellationGeneratedReferenceSource.cpp"

    # Utilities
    "${BR_SRC}/Utilities/CachePathUtilities.cpp"

    # Singletons needed by the pipeline
    "${BR_SRC}/Managers/Singletons/TaskSchedulerManager.cpp"

    # Telemetry used by TaskSchedulerManager
    "${BR_SRC}/Telemetry/FrameTaskGraphTelemetry.cpp"
)

add_executable(CLodCacheTool
    "main.cpp"
    ${CLOD_SHARED_SOURCES}
)

set_target_properties(CLodCacheTool PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    # Output the exe next to BasicRenderer.exe so it can find the
    # same USD / TBB / Assimp DLLs that are already synced there.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BasicRenderer"
)

# Include directories
target_include_directories(CLodCacheTool PRIVATE
    "${BR_INCLUDE}"
    "${BR_SHADERS}"          # ClusterLODUtilities.cpp includes "../shaders/Common/defines.h"
    "${BR_ROOT}/generated"
)

# The shaders include is for the relative-path include in ClusterLODUtilities.
# That file does  #include "../shaders/Common/defines.h"  which resolves relative
# to the .cpp location (BasicRenderer/src/Mesh/).  We add the parent of "shaders"
# so that  ../shaders/Common/defines.h  resolves correctly.
target_include_directories(CLodCacheTool PRIVATE "${BR_ROOT}")

if(TARGET BasicRHI::BasicRHI)
    target_include_directories(CLodCacheTool PRIVATE
        $<TARGET_PROPERTY:BasicRHI::BasicRHI,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_compile_definitions(CLodCacheTool PRIVATE
        $<TARGET_PROPERTY:BasicRHI::BasicRHI,INTERFACE_COMPILE_DEFINITIONS>
    )
else()
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    target_include_directories(CLodCacheTool PRIVATE "${REPO_ROOT}/BasicRHI")
endif()

if(TARGET OpenRenderGraph::OpenRenderGraph)
    target_include_directories(CLodCacheTool PRIVATE
        $<TARGET_PROPERTY:OpenRenderGraph::OpenRenderGraph,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_compile_definitions(CLodCacheTool PRIVATE
        $<TARGET_PROPERTY:OpenRenderGraph::OpenRenderGraph,INTERFACE_COMPILE_DEFINITIONS>
    )
else()
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    target_include_directories(CLodCacheTool PRIVATE "${REPO_ROOT}/OpenRenderGraph/include")
endif()

# Find packages (same vcpkg packages BasicRenderer uses) 
find_package(spdlog       REQUIRED CONFIG)
find_package(assimp       REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED CONFIG)
find_package(Boost        REQUIRED CONFIG COMPONENTS container_hash)
find_package(DirectX-Headers CONFIG REQUIRED)
find_package(Tracy REQUIRED CONFIG)
find_package(embree CONFIG REQUIRED)
find_package(flecs CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)

# OpenUSD – prefix path already set by the top-level CMakeLists.txt
find_package(pxr REQUIRED CONFIG)
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()

set(_clod_cache_tool_vcpkg_installed_dir "${VCPKG_INSTALLED_DIR}")
if(NOT _clod_cache_tool_vcpkg_installed_dir)
    set(_clod_cache_tool_vcpkg_installed_dir "${_VCPKG_INSTALLED_DIR}")
endif()
find_library(
    CLOD_CACHE_TOOL_TBB12_LIBRARY
    NAMES tbb12
    PATHS "${_clod_cache_tool_vcpkg_installed_dir}/${VCPKG_TARGET_TRIPLET}/lib"
    NO_DEFAULT_PATH
)
if(NOT CLOD_CACHE_TOOL_TBB12_LIBRARY)
    message(FATAL_ERROR
        "CLodCacheTool requires the vcpkg oneTBB library used by Embree; "
        "it was not found for triplet '${VCPKG_TARGET_TRIPLET}' under "
        "'${_clod_cache_tool_vcpkg_installed_dir}'"
    )
endif()
if(COMMAND basicrenderer_use_release_library_for_debug)
    basicrenderer_use_release_library_for_debug(fmt::fmt)
    basicrenderer_use_release_library_for_debug(Tracy::TracyClient)
endif()

# meshoptimizer is added via ThirdParty/meshoptimizer1.2

# Link libraries
target_link_libraries(CLodCacheTool PRIVATE
	BasicTelemetry::Core
	spdlog::spdlog_header_only
    assimp::assimp
    nlohmann_json::nlohmann_json
    meshoptimizer
    Boost::container_hash
    Tracy::TracyClient
    embree
    flecs::flecs_static
    bcrypt.lib
    lz4::lz4
    "${CLOD_CACHE_TOOL_TBB12_LIBRARY}"

    # OpenUSD libs needed by CLodCache / CLodCacheLoader / USDGeometryExtractor
    usd
    sdf
    tf
    vt
    arch
    plug
    ar
    usdShade
    usdSkel
)

# DirectXMath (header-only, from DirectX-Headers)
target_include_directories(CLodCacheTool PRIVATE ${DIRECTX_HEADERS_INCLUDE_DIR})

# OpenUSD headers
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# This target lives at BasicRenderer/CLodCacheTool, so its repository root is
# two parents up. The previous one-parent calculation pointed to a
# non-existent BasicRenderer/ThirdParty directory after a fresh checkout.
set(BASICRENDERER_USD_ROOT_TOOL "${REPO_ROOT}/ThirdParty" CACHE PATH "" FORCE)
target_include_directories(CLodCacheTool BEFORE PRIVATE
    "${BASICRENDERER_USD_ROOT_TOOL}/usd_$<IF:$<CONFIG:Debug>,dbg,rel>/include/"
)

# MSVC tweaks
if (MSVC)
    target_compile_options(CLodCacheTool PRIVATE
        /Zc:preprocessor
        /EHsc
        /bigobj
        /wd4828 /wd4514 /wd5038 /wd4100 /wd5219
        /wd4625 /wd4626 /wd4820 /wd4365 /wd4464
        /wd4062 /wd5026 /wd5027 /wd5246 /wd4191
        /wd4061 /wd4623 /wd4710 /wd4324 /wd5267
        /wd5045
    )
    target_compile_options(CLodCacheTool PRIVATE /external:anglebrackets /external:W0)
endif()

target_compile_definitions(CLodCacheTool PRIVATE NOMINMAX _UNICODE UNICODE)
