add_subdirectory("Streamline")
add_subdirectory("pix")
add_subdirectory("meshoptimizer1.2")

if(WIN32 AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/NVPerf/include/nvperf_host.h")
    add_library(NVPerf::Host UNKNOWN IMPORTED GLOBAL)
    set_target_properties(NVPerf::Host PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/NVPerf/bin/x64/nvperf_grfx_host.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/NVPerf/include;${CMAKE_CURRENT_LIST_DIR}/NVPerf/include/windows-desktop-x64"
    )

    set(NVPERF_GRFX_HOST_DLL "${CMAKE_CURRENT_LIST_DIR}/NVPerf/bin/x64/nvperf_grfx_host.dll" CACHE FILEPATH "NVIDIA Nsight Perf SDK graphics host runtime DLL")
endif()
