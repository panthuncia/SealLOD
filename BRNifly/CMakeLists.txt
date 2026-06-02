cmake_minimum_required(VERSION 3.16)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(BRNifly LANGUAGES CXX)
endif()

find_package(nlohmann_json REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(pxr REQUIRED CONFIG)

add_executable(BRNifly
    "src/main.cpp"
)

set_target_properties(BRNifly PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BRNifly"
)

target_link_libraries(BRNifly PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    usd
    usdGeom
    usdShade
    sdf
    tf
    vt
    arch
)
if(WIN32)
    target_link_libraries(BRNifly PRIVATE DbgHelp)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(DEFINED BASICRENDERER_USD_ROOT)
    set(BRNIFLY_USD_ROOT "${BASICRENDERER_USD_ROOT}" CACHE PATH "" FORCE)
else()
    set(BRNIFLY_USD_ROOT "${REPO_ROOT}/ThirdParty" CACHE PATH "")
endif()
target_include_directories(BRNifly BEFORE PRIVATE
    "${BRNIFLY_USD_ROOT}/usd_$<IF:$<CONFIG:Debug>,dbg,rel>/include/"
)

if (MSVC)
    target_compile_options(BRNifly PRIVATE
        /Zi
        /Zc:preprocessor
        /EHsc
        /bigobj
        /wd4828 /wd4514 /wd5038 /wd4100 /wd5219
        /wd4625 /wd4626 /wd4820 /wd4365 /wd4464
        /wd4062 /wd5026 /wd5027 /wd5246 /wd4191
        /wd4061 /wd4623 /wd4710 /wd4324 /wd5267
        /wd5045
    )
    target_link_options(BRNifly PRIVATE /DEBUG:FULL)
    target_compile_options(BRNifly PRIVATE /external:anglebrackets /external:W0)
endif()

target_compile_definitions(BRNifly PRIVATE NOMINMAX _UNICODE UNICODE)

set(BRNIFLY_USD_RUNTIME_ROOT "${BRNIFLY_USD_ROOT}/usd_$<IF:$<CONFIG:Debug>,dbg,rel>")
set(BRNIFLY_USD_RUNTIME_SYNC_STAMP "${CMAKE_CURRENT_BINARY_DIR}/runtime_sync/$<CONFIG>/BRNiflyUsdRuntimeSync.stamp")
add_custom_command(
    OUTPUT "${BRNIFLY_USD_RUNTIME_SYNC_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/runtime_sync/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND}
        "-DUSD_RUNTIME_ROOT=${BRNIFLY_USD_RUNTIME_ROOT}"
        "-DTARGET_DIR=$<TARGET_FILE_DIR:BRNifly>"
        "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_LIST_DIR}/cmake/SyncUsdRuntime.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${BRNIFLY_USD_RUNTIME_SYNC_STAMP}"
    DEPENDS BRNifly
    COMMENT "Syncing OpenUSD runtime dependencies for BRNifly"
    VERBATIM
)
add_custom_target(BRNiflyUsdRuntimeSync DEPENDS "${BRNIFLY_USD_RUNTIME_SYNC_STAMP}")
