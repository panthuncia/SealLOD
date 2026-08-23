add_subdirectory("Streamline")
add_subdirectory("pix")
add_subdirectory("meshoptimizer1.2")

set(_basicrenderer_nvperf_root "${CMAKE_CURRENT_LIST_DIR}/NvPerf")
if(WIN32
    AND EXISTS "${_basicrenderer_nvperf_root}/include/nvperf_host.h"
    AND EXISTS "${_basicrenderer_nvperf_root}/bin/x64/nvperf_grfx_host.lib"
    AND EXISTS "${_basicrenderer_nvperf_root}/bin/x64/nvperf_grfx_host.dll")
    add_library(NVPerf::Host UNKNOWN IMPORTED GLOBAL)
    set_target_properties(NVPerf::Host PROPERTIES
        IMPORTED_LOCATION "${_basicrenderer_nvperf_root}/bin/x64/nvperf_grfx_host.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_basicrenderer_nvperf_root}/include;${_basicrenderer_nvperf_root}/include/windows-desktop-x64"
    )

    set(NVPERF_GRFX_HOST_DLL "${_basicrenderer_nvperf_root}/bin/x64/nvperf_grfx_host.dll" CACHE FILEPATH "NVIDIA Nsight Perf SDK graphics host runtime DLL")
    message(STATUS "BasicRenderer: found NVIDIA Nsight Perf SDK at ${_basicrenderer_nvperf_root}")
endif()
