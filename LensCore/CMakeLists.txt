# LensCore: CPU reference for the physical camera lens: prescriptions, exact sequential tracing,
# paraxial and derived tables, the exact pupil, and the polynomial lens's least-squares fit. It is the
# ground truth for the HLSL explicit tracer. No renderer, no GPU. See SARP's docs/lens_simulation.md.

cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_NAME)
    # Standalone configure, for quick iteration: cmake -S LensCore -B <dir> -DCMAKE_PREFIX_PATH=<vcpkg share root>
    project(LensCore LANGUAGES CXX)
    enable_testing()
endif()

find_package(nlohmann_json REQUIRED CONFIG)
find_package(Threads REQUIRED)

add_library(LensCore STATIC
    "include/LensCore/BrLens.h"
    "include/LensCore/Lens.h"
    "include/LensCore/LensAnalysis.h"
    "include/LensCore/LensDataset.h"
    "include/LensCore/LensPolynomial.h"
    "include/LensCore/LensTrace.h"
    "src/BrLens.cpp"
    "src/Lens.cpp"
    "src/LensAnalysis.cpp"
    "src/LensDataset.cpp"
    "src/LensPolynomial.cpp"
    "src/LensTrace.cpp"
)
target_include_directories(LensCore PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(LensCore PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

add_executable(LensBake "tools/LensBake.cpp")
target_link_libraries(LensBake PRIVATE LensCore nlohmann_json::nlohmann_json Threads::Threads)
set_target_properties(LensBake PROPERTIES
    # Beside the other offline tools in the runtime root, not in a per-config subdirectory.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BasicRenderer"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/BasicRenderer"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/BasicRenderer"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/BasicRenderer"
)

add_executable(LensCoreTests "tests/LensCoreTests.cpp")
target_link_libraries(LensCoreTests PRIVATE LensCore)

get_filename_component(LENS_CORE_LENS_DIR "${CMAKE_CURRENT_LIST_DIR}/../lenses" ABSOLUTE)
foreach(_lens_target LensCore LensBake LensCoreTests)
    set_target_properties(${_lens_target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(${_lens_target} PRIVATE NOMINMAX _UNICODE UNICODE LENS_CORE_LENS_DIR="${LENS_CORE_LENS_DIR}")
    if(MSVC)
        target_compile_options(${_lens_target} PRIVATE /Zc:preprocessor /EHsc)
    endif()
endforeach()

add_test(NAME LensCoreTests COMMAND LensCoreTests)
