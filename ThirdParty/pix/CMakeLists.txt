cmake_minimum_required(VERSION 3.18)

# Header-only "package" for PIX
project(PixHeaders LANGUAGES NONE)

# Real target
add_library(PixHeaders INTERFACE)

# Public alias the rest of the tree will use
add_library(Pix::headers ALIAS PixHeaders)

# Expose this directory as an include path (build tree + install tree)
target_include_directories(PixHeaders INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

# If you want install support (optional)
include(GNUInstallDirs)

# Name the exported/imported target
set_target_properties(PixHeaders PROPERTIES EXPORT_NAME headers)

install(TARGETS PixHeaders
        EXPORT PixHeadersTargets)

# Install headers preserving the folder layout under include/
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
          PATTERN "*.h" PATTERN "*.hpp")

# Generate an export file so downstream projects can `find_package` if desired
install(EXPORT PixHeadersTargets
        NAMESPACE Pix::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PixHeaders)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/PixHeadersConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/PixHeadersConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PixHeaders)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PixHeadersConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PixHeaders)
