# Locates the .NET SDK and its native hosting pack (nethost.h, hostfxr.h, coreclr_delegates.h, nethost.lib).
#
# Sets, in the caller's scope:
#   BASICRENDERER_DOTNET_FOUND        TRUE when a .NET >= 10 SDK and a matching win-x64 host pack exist
#   BASICRENDERER_DOTNET_EXECUTABLE   dotnet.exe
#   BASICRENDERER_NETHOST_DIR         the host pack's native directory
#   BASICRENDERER_DOTNET_REASON       why it was not found

set(BASICRENDERER_DOTNET_FOUND FALSE)
set(BASICRENDERER_DOTNET_REASON "")

find_program(BASICRENDERER_DOTNET_EXECUTABLE dotnet
    HINTS "$ENV{DOTNET_ROOT}" "$ENV{ProgramFiles}/dotnet")
if(NOT BASICRENDERER_DOTNET_EXECUTABLE)
    set(BASICRENDERER_DOTNET_REASON "dotnet was not found (install the .NET 10 SDK, or set DOTNET_ROOT)")
    return()
endif()

# Resolve a shim or symlink to the real install root, which holds packs/.
get_filename_component(_br_dotnet_exe "${BASICRENDERER_DOTNET_EXECUTABLE}" REALPATH)
get_filename_component(_br_dotnet_root "${_br_dotnet_exe}" DIRECTORY)

# Not a source list: the host pack's version directory is only known at configure time.
file(GLOB _br_host_packs LIST_DIRECTORIES true "${_br_dotnet_root}/packs/Microsoft.NETCore.App.Host.win-x64/*")
set(_br_best_version "")
foreach(_br_pack IN LISTS _br_host_packs)
    get_filename_component(_br_version "${_br_pack}" NAME)
    if(_br_version VERSION_GREATER_EQUAL 10.0 AND (NOT _br_best_version OR _br_version VERSION_GREATER _br_best_version)
        AND EXISTS "${_br_pack}/runtimes/win-x64/native/nethost.lib")
        set(_br_best_version "${_br_version}")
    endif()
endforeach()
if(NOT _br_best_version)
    set(BASICRENDERER_DOTNET_REASON "no .NET >= 10 win-x64 host pack under ${_br_dotnet_root}/packs (install the .NET 10 SDK)")
    return()
endif()

set(BASICRENDERER_NETHOST_DIR "${_br_dotnet_root}/packs/Microsoft.NETCore.App.Host.win-x64/${_br_best_version}/runtimes/win-x64/native")
set(BASICRENDERER_DOTNET_FOUND TRUE)
message(STATUS "BasicRenderer scripting: .NET host pack ${_br_best_version} (${BASICRENDERER_NETHOST_DIR})")
