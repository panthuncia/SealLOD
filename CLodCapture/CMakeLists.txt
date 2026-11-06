# CLodCapture: renders a job file to a bundle of images and counters. See docs/clod-evaluation.md.
# It links the whole renderer, so it is the slow one to rebuild, and it holds no metrics or rigs.

cmake_minimum_required(VERSION 3.16)

get_filename_component(CLOD_CAPTURE_BR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../BasicRenderer" ABSOLUTE)
set(CLOD_CAPTURE_RUNTIME_ROOT "${CMAKE_BINARY_DIR}/BasicRenderer")

find_package(spdlog REQUIRED CONFIG)
get_filename_component(CLOD_CAPTURE_EVAL_SRC "${CMAKE_CURRENT_LIST_DIR}/../CLodEval/src" ABSOLUTE)
add_executable(CLodCapture
    "CaptureShared.h"
    "main.cpp"
    "Viewer.cpp"
    "Viewer.h"
    "ViewerHierarchy.cpp"
    "ViewerHierarchy.h"
    "ViewerInspect.cpp"
    "ViewerInspect.h"
    "ViewerPanels.cpp"
    "ViewerTexture.cpp"
    "ViewerTexture.h"
    # The viewer reads caches the same way the analyzers do, and saves screenshots with their PNG writer.
    "${CLOD_CAPTURE_EVAL_SRC}/Core/CachedMesh.cpp"
    "${CLOD_CAPTURE_EVAL_SRC}/Core/Image.cpp"
)
target_link_libraries(CLodCapture PRIVATE
    BasicRenderer
    ${BASICRENDERER_SPDLOG_TARGET}
    Pix::headers
    "${CLOD_CAPTURE_BR_ROOT}/WinPixEventRuntime.lib"
)
# The readback and settings interfaces it reaches are the same private ones the demo uses.
target_include_directories(CLodCapture PRIVATE "${CLOD_CAPTURE_BR_ROOT}/src" "${CLOD_CAPTURE_EVAL_SRC}" "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(CLodCapture PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    # In the runtime root, where shaders/, D3D/ and the DLLs are, and not in a per-config subdirectory.
    RUNTIME_OUTPUT_DIRECTORY "${CLOD_CAPTURE_RUNTIME_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CLOD_CAPTURE_RUNTIME_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CLOD_CAPTURE_RUNTIME_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CLOD_CAPTURE_RUNTIME_ROOT}"
)
target_compile_definitions(CLodCapture PRIVATE NOMINMAX _UNICODE UNICODE)
if(MSVC)
    target_compile_options(CLodCapture PRIVATE /Zc:preprocessor /EHsc /bigobj)
endif()
add_dependencies(CLodCapture tbb_config_sync)
