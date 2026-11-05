# CLodEval: CPU-only measurement of CLod hierarchies. It links the cache format and never the
# renderer or generator, so metric edits rebuild quickly and need no GPU. Builds run CLodCacheTool as
# a child process. See docs/clod-evaluation.md.

cmake_minimum_required(VERSION 3.16)

find_package(lz4 CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED CONFIG)

get_filename_component(CLOD_EVAL_BR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../BasicRenderer" ABSOLUTE)

add_library(CLodEvalCore STATIC
    "src/Analyzers/Analyzers.cpp"
    "src/Analyzers/Analyzers.h"
    "src/Analyzers/GeometryAnalyzers.cpp"
    "src/Analyzers/HierarchyAnalyzers.cpp"
    "src/Analyzers/MemoryAnalyzers.cpp"
    "src/Analyzers/VoxelFidelity.cpp"
    "src/Analyzers/VoxelFidelity.h"
    "src/Capture.cpp"
    "src/Capture.h"
    "src/Core/Analyzer.h"
    "src/Core/CachedMesh.cpp"
    "src/Core/CachedMesh.h"
    "src/Core/Image.cpp"
    "src/Core/Image.h"
    "src/Core/Metrics.cpp"
    "src/Core/Metrics.h"
    "src/Core/Process.cpp"
    "src/Core/Process.h"
    "src/Core/Results.cpp"
    "src/Core/Results.h"
    "src/Suite.cpp"
    "src/Transition.cpp"
    "src/Transition.h"
    "src/Suite.h"
)
target_include_directories(CLodEvalCore
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src"
    PRIVATE
        "${CLOD_EVAL_BR_ROOT}"  # "../shaders/Common/defines.h"
)
target_link_libraries(CLodEvalCore
    PUBLIC
        CLodCacheFormat
        nlohmann_json::nlohmann_json
    PRIVATE
        lz4::lz4
)
target_compile_definitions(CLodEvalCore PRIVATE
    CLOD_EVAL_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
)

add_executable(CLodEval "src/main.cpp")
target_link_libraries(CLodEval PRIVATE CLodEvalCore)
# Beside CLodCacheTool, which it finds by looking next to itself.
set_target_properties(CLodEval PROPERTIES
    # Beside CLodCacheTool and CLodCapture in the runtime root, and not in a per-config subdirectory.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BasicRenderer"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/BasicRenderer"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/BasicRenderer"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/BasicRenderer"
)

add_executable(CLodEvalTests "tests/CLodEvalTests.cpp")
target_link_libraries(CLodEvalTests PRIVATE CLodEvalCore CLodBuildCore)

foreach(_clod_eval_target CLodEvalCore CLodEval CLodEvalTests)
    set_target_properties(${_clod_eval_target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(${_clod_eval_target} PRIVATE NOMINMAX _UNICODE UNICODE)
    if(MSVC)
        target_compile_options(${_clod_eval_target} PRIVATE /Zc:preprocessor /EHsc /bigobj)
    endif()
endforeach()

add_test(NAME CLodEvalTests COMMAND CLodEvalTests)
# The generator it links needs oneTBB's DLL, which is synced to the runtime root.
if(BASICRENDERER_TEST_RUNTIME_DIR)
    set(_clod_eval_test_runtime "${BASICRENDERER_TEST_RUNTIME_DIR}")
else()
    set(_clod_eval_test_runtime "${CMAKE_BINARY_DIR}/BasicRenderer")
endif()
set_tests_properties(CLodEvalTests PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_clod_eval_test_runtime}")
