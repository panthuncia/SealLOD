cmake_minimum_required(VERSION 3.18)

# Header-only "package" for Streamline
project(StreamlineHeaders LANGUAGES NONE)

# Real target
add_library(StreamlineHeaders INTERFACE)

# Public alias the rest of your tree will use
add_library(Streamline::headers ALIAS StreamlineHeaders)

# Expose this directory as an include path (build tree + install tree)
target_include_directories(StreamlineHeaders INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

# If you want install support (optional)
include(GNUInstallDirs)

# Name the exported/imported target "Streamline::headers"
set_target_properties(StreamlineHeaders PROPERTIES EXPORT_NAME headers)

install(TARGETS StreamlineHeaders
        EXPORT StreamlineHeadersTargets)

# Install headers preserving the folder layout under include/
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
          PATTERN "*.h" PATTERN "*.hpp")

# Generate an export file so downstream projects can `find_package` if desired
install(EXPORT StreamlineHeadersTargets
        NAMESPACE Streamline::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/StreamlineHeaders)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/StreamlineHeadersConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/StreamlineHeadersConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/StreamlineHeaders)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/StreamlineHeadersConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/StreamlineHeaders)
