# Require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(USDPLUGINS_HOST_TARGET "BasicRenderer" CACHE STRING "Target to depend on for plugin staging hooks")
get_filename_component(USDPLUGINS_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(USDPLUGINS_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/BasicRenderer")

# Find OpenUSD (must provide USD_INCLUDE_DIRS and USD_LIBRARIES)
find_package(pxr REQUIRED)
find_package(spdlog REQUIRED)

# Find CURL for HTTP fetching
find_package(curl REQUIRED)

# Our plugin target
add_library(usd_httpResolver MODULE
    HttpResolver.cpp
    HttpResolver.h
)

# Link against USD and CURL
target_link_libraries(usd_httpResolver PRIVATE
    spdlog::spdlog
    arch
    plug
    ar
    ${CURL_LIBRARIES}
)

# On Windows, ensure it's called usd_httpResolver.dll (no lib prefix)
set_target_properties(usd_httpResolver PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
)

install(TARGETS usd_httpResolver
        LIBRARY DESTINATION ${USDPLUGINS_OUTPUT_ROOT}
)

set_target_properties(usd_httpResolver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${USDPLUGINS_OUTPUT_ROOT}"
    LIBRARY_OUTPUT_DIRECTORY "${USDPLUGINS_OUTPUT_ROOT}"
    ARCHIVE_OUTPUT_DIRECTORY "${USDPLUGINS_OUTPUT_ROOT}"
)

install(FILES plugInfo.json
    DESTINATION plugins/pxr
)


function(add_symlink_or_copy_per_config source destination IS_DIRECTORY target_name)
    cmake_parse_arguments(_args "" "" "IS_DIRECTORY;TARGET_NAME" ${ARGN})

    if (NOT target_name)
        get_filename_component(target_name "${destination}" NAME)
    endif()

    set(_sync_stamp "${CMAKE_CURRENT_BINARY_DIR}/runtime_sync/$<CONFIG>/${target_name}.stamp")
    set(_sync_stamp_dir "${CMAKE_CURRENT_BINARY_DIR}/runtime_sync/$<CONFIG>")

    if (IS_DIRECTORY)
        add_custom_command(
            OUTPUT "${_sync_stamp}"
            COMMENT "Syncing directory ${destination} <- ${source}"
            VERBATIM
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_sync_stamp_dir}"
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${destination}"
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy_directory,create_symlink> "${source}" "${destination}"
            COMMAND ${CMAKE_COMMAND} -E touch "${_sync_stamp}"
        )
    else()
        add_custom_command(
            OUTPUT "${_sync_stamp}"
            COMMENT "Syncing file ${destination} <- ${source}"
            VERBATIM
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_sync_stamp_dir}"
            COMMAND ${CMAKE_COMMAND} -E remove "${destination}"
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy,create_symlink> "${source}" "${destination}"
            COMMAND ${CMAKE_COMMAND} -E touch "${_sync_stamp}"
            DEPENDS "${source}"
        )
    endif()

    add_custom_target("${target_name}_sync" DEPENDS "${_sync_stamp}")
    if(TARGET "${USDPLUGINS_HOST_TARGET}")
        add_dependencies("${USDPLUGINS_HOST_TARGET}" "${target_name}_sync")
    endif()
endfunction()

# Endure folder exists
file(MAKE_DIRECTORY "${USDPLUGINS_OUTPUT_ROOT}/thirdPartyUSDPlugins")

add_symlink_or_copy_per_config(
    "${USDPLUGINS_SOURCE_ROOT}/httpResolver"
    "${USDPLUGINS_OUTPUT_ROOT}/thirdPartyUSDPlugins/httpResolver"
    TRUE  # IS_DIRECTORY
    "httpResolver"
)
