if(NOT DEFINED USD_RUNTIME_ROOT)
    message(FATAL_ERROR "SyncUsdRuntime.cmake requires USD_RUNTIME_ROOT.")
endif()

if(NOT DEFINED TARGET_DIR)
    message(FATAL_ERROR "SyncUsdRuntime.cmake requires TARGET_DIR.")
endif()

if(NOT DEFINED CONFIG)
    set(CONFIG "")
endif()

file(MAKE_DIRECTORY "${TARGET_DIR}")
file(COPY "${USD_RUNTIME_ROOT}/lib/" DESTINATION "${TARGET_DIR}")

file(GLOB _brnifly_tbb_dlls "${USD_RUNTIME_ROOT}/bin/tbb*.dll")
foreach(_brnifly_tbb_dll IN LISTS _brnifly_tbb_dlls)
    file(COPY "${_brnifly_tbb_dll}" DESTINATION "${TARGET_DIR}")
endforeach()

if(CONFIG STREQUAL "Debug" AND NOT EXISTS "${TARGET_DIR}/tbb_debug.dll")
    if(EXISTS "${USD_RUNTIME_ROOT}/bin/tbb_debug.dll")
        file(COPY "${USD_RUNTIME_ROOT}/bin/tbb_debug.dll" DESTINATION "${TARGET_DIR}")
    elseif(EXISTS "${USD_RUNTIME_ROOT}/bin/tbb.dll")
        configure_file("${USD_RUNTIME_ROOT}/bin/tbb.dll" "${TARGET_DIR}/tbb_debug.dll" COPYONLY)
    else()
        message(FATAL_ERROR "Neither tbb_debug.dll nor tbb.dll was found in ${USD_RUNTIME_ROOT}/bin.")
    endif()
endif()
