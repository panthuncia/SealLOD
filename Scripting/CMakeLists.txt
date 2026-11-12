# BasicRendererScripting: in-process C# scenario scripting. A native host library (hostfxr loader, the C ABI in
# include/BasicRendererScripting/ScriptAbi.h, ScriptHost) plus the managed runtime and API it loads
# (Managed/BasicRenderer.Scripting). See docs/scripting.md.
#
# Host applications check BASICRENDERER_SCRIPTING_AVAILABLE, link BasicRendererScripting and call
# basicrenderer_scripting_deploy(<target>) so the managed runtime and the example scenarios are copied beside the
# executable. Added before BasicRenderer/ so the demo can do that next to its own definition.

option(BASICRENDERER_ENABLE_SCRIPTING "Build the C# scenario scripting host (needs the .NET 10 SDK)" ON)
set(BASICRENDERER_SCRIPTING_AVAILABLE FALSE CACHE INTERNAL "")
if(NOT BASICRENDERER_ENABLE_SCRIPTING)
    return()
endif()
if(NOT WIN32)
    message(STATUS "BasicRenderer scripting: disabled (Windows only)")
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/DotNetHosting.cmake")
if(NOT BASICRENDERER_DOTNET_FOUND)
    message(STATUS "BasicRenderer scripting: disabled, ${BASICRENDERER_DOTNET_REASON}")
    return()
endif()
set(BASICRENDERER_SCRIPTING_AVAILABLE TRUE CACHE INTERNAL "")

# --- Managed runtime ---------------------------------------------------------------------------------------------

set(BR_SCRIPTING_MANAGED_DIR "${CMAKE_CURRENT_LIST_DIR}/Managed/BasicRenderer.Scripting")
set(BR_SCRIPTING_MANAGED_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/Directory.Build.props"
    "${BR_SCRIPTING_MANAGED_DIR}/BasicRenderer.Scripting.csproj"
    "${BR_SCRIPTING_MANAGED_DIR}/Api/Scenario.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Api/ScenarioContext.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Api/TimeExtensions.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Interop/Native.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Interop/NativeAbi.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Runtime/FrameScheduler.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Runtime/ScenarioCompiler.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Runtime/ScenarioLoadContext.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Runtime/ScenarioResult.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Runtime/ScenarioRunner.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Runtime/ScriptRuntime.cs"
    "${BR_SCRIPTING_MANAGED_DIR}/Runtime/ScriptSession.cs"
)
set(BR_SCRIPTING_SCENARIO_FILES
    "${CMAKE_CURRENT_LIST_DIR}/Scenarios/Scenarios.csproj"
    "${CMAKE_CURRENT_LIST_DIR}/Scenarios/Hello.cs"
    CACHE INTERNAL "")
# Cache entries, not variables: basicrenderer_scripting_deploy runs in its caller's directory scope.
set(BR_SCRIPTING_MANAGED_OUT "${CMAKE_CURRENT_BINARY_DIR}/managed" CACHE INTERNAL "")
set(BR_SCRIPTING_DOTNET_ARTIFACTS "${CMAKE_CURRENT_BINARY_DIR}/dotnet-artifacts")
# Release regardless of the native configuration: scenario stack traces come from the scenarios' own PDBs, and the
# runtime's embedded PDB still gives file:line for its frames.
set(BR_SCRIPTING_DOTNET_BUILD
    "${BASICRENDERER_DOTNET_EXECUTABLE}" build --nologo -v:minimal -c Release
    "-p:ArtifactsPath=${BR_SCRIPTING_DOTNET_ARTIFACTS}")

add_custom_command(
    OUTPUT "${BR_SCRIPTING_MANAGED_OUT}/BasicRenderer.Scripting.dll"
    COMMAND ${BR_SCRIPTING_DOTNET_BUILD} "${BR_SCRIPTING_MANAGED_DIR}/BasicRenderer.Scripting.csproj" -o "${BR_SCRIPTING_MANAGED_OUT}"
    DEPENDS ${BR_SCRIPTING_MANAGED_SOURCES}
    COMMENT "Building the managed scripting runtime (BasicRenderer.Scripting)"
    VERBATIM)
add_custom_target(BasicRendererScriptingManaged ALL DEPENDS "${BR_SCRIPTING_MANAGED_OUT}/BasicRenderer.Scripting.dll")

# --- Native host -------------------------------------------------------------------------------------------------

add_library(BasicRendererScripting STATIC
    "include/BasicRendererScripting/ScriptAbi.h"
    "include/BasicRendererScripting/ScriptHost.h"
    "src/DotNetRuntime.h"
    "src/DotNetRuntime.cpp"
    "src/ScriptHost.cpp"
)
add_library(BasicRenderer::Scripting ALIAS BasicRendererScripting)
target_include_directories(BasicRendererScripting
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE "${BASICRENDERER_NETHOST_DIR}")
target_compile_features(BasicRendererScripting PUBLIC cxx_std_23)
target_compile_definitions(BasicRendererScripting PRIVATE UNICODE _UNICODE)
# The import library, not libnethost.lib: that one is built against the static CRT and this tree links /MD.
# nethost.dll is copied beside each host executable (basicrenderer_scripting_deploy, ScriptHostTests).
target_link_libraries(BasicRendererScripting PRIVATE "${BASICRENDERER_NETHOST_DIR}/nethost.lib")
set(BR_SCRIPTING_NETHOST_DLL "${BASICRENDERER_NETHOST_DIR}/nethost.dll" CACHE INTERNAL "")
add_dependencies(BasicRendererScripting BasicRendererScriptingManaged)
if(MSVC)
    target_compile_options(BasicRendererScripting PRIVATE /EHsc)
endif()

# basicrenderer_scripting_deploy(<target> [DESTINATIONS <dir>...])
# Copies the managed runtime to <dir>/scripting, nethost.dll to <dir>, and the example scenarios to <dir>/scenarios
# whenever any of them changes, independently of whether the target relinks. The default destination is the target's
# own output directory; a host that runs from elsewhere (the demo's runtime root) names that too.
function(basicrenderer_scripting_deploy target)
    cmake_parse_arguments(PARSE_ARGV 1 _deploy "" "" "DESTINATIONS")
    if(NOT _deploy_DESTINATIONS)
        set(_deploy_DESTINATIONS "$<TARGET_FILE_DIR:${target}>")
    endif()
    set(_commands "")
    foreach(_destination IN LISTS _deploy_DESTINATIONS)
        list(APPEND _commands
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${BR_SCRIPTING_MANAGED_OUT}" "${_destination}/scripting"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BR_SCRIPTING_NETHOST_DLL}" "${_destination}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_destination}/scenarios"
            COMMAND "${CMAKE_COMMAND}" -E copy ${BR_SCRIPTING_SCENARIO_FILES} "${_destination}/scenarios")
    endforeach()
    set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}.scripting-deploy.stamp")
    add_custom_command(
        OUTPUT "${_stamp}"
        ${_commands}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
        DEPENDS "${BR_SCRIPTING_MANAGED_OUT}/BasicRenderer.Scripting.dll" ${BR_SCRIPTING_SCENARIO_FILES}
        COMMENT "Deploying the scripting runtime for ${target}"
        VERBATIM)
    add_custom_target(${target}ScriptingDeploy ALL DEPENDS "${_stamp}")
    add_dependencies(${target} ${target}ScriptingDeploy)
endfunction()

# --- Tests -------------------------------------------------------------------------------------------------------

add_executable(ScriptHostTests "tests/ScriptHostTests.cpp")
target_link_libraries(ScriptHostTests PRIVATE BasicRendererScripting)
add_custom_command(TARGET ScriptHostTests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BR_SCRIPTING_NETHOST_DLL}" "$<TARGET_FILE_DIR:ScriptHostTests>"
    VERBATIM)
add_test(NAME ScriptHostTests
    COMMAND ScriptHostTests "${BR_SCRIPTING_MANAGED_OUT}" "${CMAKE_CURRENT_LIST_DIR}/Scenarios" "${CMAKE_CURRENT_LIST_DIR}/tests/fixtures")

set(BR_SCRIPTING_TESTS_DIR "${CMAKE_CURRENT_LIST_DIR}/tests/ScriptRuntimeTests")
set(BR_SCRIPTING_TESTS_OUT "${CMAKE_CURRENT_BINARY_DIR}/runtime-tests")
add_custom_command(
    OUTPUT "${BR_SCRIPTING_TESTS_OUT}/ScriptRuntimeTests.dll"
    COMMAND ${BR_SCRIPTING_DOTNET_BUILD} "${BR_SCRIPTING_TESTS_DIR}/ScriptRuntimeTests.csproj" -o "${BR_SCRIPTING_TESTS_OUT}"
    DEPENDS ${BR_SCRIPTING_MANAGED_SOURCES} "${BR_SCRIPTING_TESTS_DIR}/ScriptRuntimeTests.csproj" "${BR_SCRIPTING_TESTS_DIR}/Program.cs"
    COMMENT "Building the managed scripting tests (ScriptRuntimeTests)"
    VERBATIM)
add_custom_target(ScriptRuntimeTests ALL DEPENDS "${BR_SCRIPTING_TESTS_OUT}/ScriptRuntimeTests.dll")
add_test(NAME ScriptRuntimeTests
    COMMAND "${BASICRENDERER_DOTNET_EXECUTABLE}" "${BR_SCRIPTING_TESTS_OUT}/ScriptRuntimeTests.dll"
        "${CMAKE_CURRENT_LIST_DIR}/tests/fixtures" "${CMAKE_CURRENT_LIST_DIR}/Scenarios")
