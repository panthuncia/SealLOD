cmake_minimum_required(VERSION 3.24)
project(InstalledFirstPartyPackageConsumer LANGUAGES CXX)

set(CONSUMER_PACKAGE "" CACHE STRING "First-party package to consume independently")
set(consumer_packages BasicRHI BasicTelemetry BasicScene OpenRenderGraph ORGModuleServices)
if(NOT CONSUMER_PACKAGE IN_LIST consumer_packages)
    message(FATAL_ERROR "Select CONSUMER_PACKAGE from: ${consumer_packages}")
endif()
find_package(${CONSUMER_PACKAGE} CONFIG REQUIRED)

add_executable(InstalledFirstPartyPackageConsumer ${CONSUMER_PACKAGE}.cpp)
target_compile_features(InstalledFirstPartyPackageConsumer PRIVATE cxx_std_23)
if(CONSUMER_PACKAGE STREQUAL "BasicTelemetry")
    target_link_libraries(InstalledFirstPartyPackageConsumer PRIVATE BasicTelemetry::Core)
else()
    target_link_libraries(InstalledFirstPartyPackageConsumer PRIVATE
        ${CONSUMER_PACKAGE}::${CONSUMER_PACKAGE})
endif()
