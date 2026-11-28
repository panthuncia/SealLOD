# Alembic (and its Imath) is used only by HairCore's groom importer.
find_package(Alembic CONFIG REQUIRED)
if(COMMAND basicrenderer_use_release_library_for_debug)
    basicrenderer_use_release_library_for_debug(Alembic::Alembic)
    basicrenderer_use_release_library_for_debug(Imath::Imath)
endif()

add_library(HairCore STATIC
    include/Hair/AlembicGroomReader.h
    include/Hair/GroomBuilder.h
    include/Hair/GroomFile.h
    include/Hair/GroomSource.h
    include/Hair/HairBsdfShared.h
    include/Hair/HairFileReader.h
    include/Hair/HairScatterShared.h
    include/Hair/HairVolumeShared.h
    include/Hair/HairGPUTypes.h
    include/Hair/HairLod.h
    include/Hair/HairLodShared.h
    include/Hair/HairMath.h
    include/Hair/ProceduralGroom.h
    src/AlembicGroomReader.cpp
    src/GroomBuilder.cpp
    src/GroomFile.cpp
    src/GroomSource.cpp
    src/HairFileReader.cpp
    src/ProceduralGroom.cpp)
add_library(BasicRenderer::HairCore ALIAS HairCore)
target_compile_features(HairCore PUBLIC cxx_std_20)
target_include_directories(HairCore PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(HairCore PRIVATE Alembic::Alembic)

# The tests link Alembic themselves to write the archives they import.
add_executable(HairCoreTests
    tests/AlembicGroomTests.cpp
    tests/GroomBuilderTests.cpp
    tests/HairBsdfTests.cpp
    tests/HairCoreTests.cpp
    tests/HairScatterTests.cpp)
target_link_libraries(HairCoreTests PRIVATE HairCore Alembic::Alembic)
add_test(NAME HairCoreTests COMMAND HairCoreTests)

# The renderer plugin: runtime, frame graph extension and control commands. It uses the renderer's public headers only.
add_library(HairPlugin STATIC
    include/Hair/HairCommands.h
    include/Hair/HairExtension.h
    include/Hair/HairRuntime.h
    include/Hair/HairRuntimeGPUTypes.h
    src/HairCommands.cpp
    src/HairExtension.cpp
    src/HairGpuBuffers.h
    src/HairRuntime.cpp)
add_library(BasicRenderer::HairPlugin ALIAS HairPlugin)
target_compile_features(HairPlugin PUBLIC cxx_std_20)
target_include_directories(HairPlugin PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(HairPlugin PUBLIC HairCore BasicRenderer::BasicRenderer)

# The shaders are compiled at run time from HairShaders/ in the renderer's runtime root (beside shaders/), with the
# C++/HLSL headers they share with the plugin in HairShaders/Hair/.
set(HAIR_SHADER_STAGE_DIR "${CMAKE_BINARY_DIR}/BasicRenderer/HairShaders")
set(HAIR_SHADER_FILES
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairBsdf.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairCommon.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairComposite.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairDebugOverlay.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairGuideLevels.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairGuideSkin.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairHit.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairRaster.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairRasterCulled.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairResolve.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairRootCheck.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairRootFrame.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairSelectEvaluate.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairShade.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairShadeList.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairShadow.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairShading.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairSimulate.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairStructs.hlsli"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairVolume.hlsl"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/hairVolume.hlsli")
set(HAIR_SHADER_SHARED_HEADERS
    "${CMAKE_CURRENT_LIST_DIR}/include/Hair/HairBsdfShared.h"
    "${CMAKE_CURRENT_LIST_DIR}/include/Hair/HairGPUTypes.h"
    "${CMAKE_CURRENT_LIST_DIR}/include/Hair/HairLodShared.h"
    "${CMAKE_CURRENT_LIST_DIR}/include/Hair/HairRuntimeGPUTypes.h"
    "${CMAKE_CURRENT_LIST_DIR}/include/Hair/HairScatterShared.h"
    "${CMAKE_CURRENT_LIST_DIR}/include/Hair/HairVolumeShared.h")
set(HAIR_SHADER_STAGE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/HairShaders.stamp")
add_custom_command(
    OUTPUT "${HAIR_SHADER_STAGE_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${HAIR_SHADER_STAGE_DIR}/Hair"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${HAIR_SHADER_FILES} "${HAIR_SHADER_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${HAIR_SHADER_SHARED_HEADERS} "${HAIR_SHADER_STAGE_DIR}/Hair"
    COMMAND "${CMAKE_COMMAND}" -E touch "${HAIR_SHADER_STAGE_STAMP}"
    DEPENDS ${HAIR_SHADER_FILES} ${HAIR_SHADER_SHARED_HEADERS}
    COMMENT "Staging the hair shaders"
    VERBATIM)
add_custom_target(HairShaders DEPENDS "${HAIR_SHADER_STAGE_STAMP}")
add_dependencies(HairPlugin HairShaders)
