#include "DotNetRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

#include <format>

namespace br::scripting::detail {

    namespace {

        std::string Narrow(const std::wstring& text)
        {
            if (text.empty()) return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        std::string HostError(const char* step, int32_t code)
        {
            return std::format("{} failed (0x{:08x}). Is the .NET 10 runtime installed? "
                               "https://dotnet.microsoft.com/download",
                step, static_cast<uint32_t>(code));
        }

    } // namespace

    bool DotNetRuntime::Initialize(const std::filesystem::path& runtimeConfigPath, std::string& error)
    {
        if (m_loadAssembly) return true;
        if (!std::filesystem::exists(runtimeConfigPath)) {
            error = std::format("scripting runtime config not found: {}", runtimeConfigPath.string());
            return false;
        }

        wchar_t hostfxrPath[MAX_PATH];
        size_t hostfxrPathSize = MAX_PATH;
        if (const int rc = get_hostfxr_path(hostfxrPath, &hostfxrPathSize, nullptr); rc != 0) {
            error = HostError("get_hostfxr_path", rc);
            return false;
        }
        HMODULE hostfxr = LoadLibraryW(hostfxrPath);
        if (!hostfxr) {
            error = std::format("could not load {}", Narrow(hostfxrPath));
            return false;
        }
        const auto initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
            GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config"));
        const auto getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate"));
        const auto close = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(hostfxr, "hostfxr_close"));
        if (!initialize || !getDelegate || !close) {
            error = "hostfxr is missing the hosting entry points (needs .NET 8 or later)";
            return false;
        }

        hostfxr_handle context = nullptr;
        const int rc = initialize(runtimeConfigPath.c_str(), nullptr, &context);
        // 0 is success; 1 and 2 report an already-initialized runtime, which is also usable.
        if (rc < 0 || rc > 2 || !context) {
            if (context) close(context);
            error = HostError("hostfxr_initialize_for_runtime_config", rc);
            return false;
        }

        void* loadAssembly = nullptr;
        void* getFunctionPointer = nullptr;
        const int loadRc = getDelegate(context, hdt_load_assembly, &loadAssembly);
        const int getRc = getDelegate(context, hdt_get_function_pointer, &getFunctionPointer);
        // The runtime stays loaded; the context is only needed to obtain the delegates.
        close(context);
        if (loadRc != 0 || getRc != 0 || !loadAssembly || !getFunctionPointer) {
            error = HostError("hostfxr_get_runtime_delegate", loadRc != 0 ? loadRc : getRc);
            return false;
        }
        m_loadAssembly = loadAssembly;
        m_getFunctionPointer = getFunctionPointer;
        return true;
    }

    void* DotNetRuntime::GetUnmanagedEntryPoint(const std::filesystem::path& assemblyPath, const wchar_t* typeName, const wchar_t* methodName,
        std::string& error)
    {
        if (!m_loadAssembly) {
            error = "the .NET runtime is not initialized";
            return nullptr;
        }
        const auto loadAssembly = reinterpret_cast<load_assembly_fn>(m_loadAssembly);
        if (const int rc = loadAssembly(assemblyPath.c_str(), nullptr, nullptr); rc != 0) {
            error = std::format("loading {} failed (0x{:08x})", assemblyPath.string(), static_cast<uint32_t>(rc));
            return nullptr;
        }
        const auto getFunctionPointer = reinterpret_cast<get_function_pointer_fn>(m_getFunctionPointer);
        void* entry = nullptr;
        if (const int rc = getFunctionPointer(typeName, methodName, UNMANAGEDCALLERSONLY_METHOD, nullptr, nullptr, &entry); rc != 0 || !entry) {
            error = std::format("resolving {}::{} failed (0x{:08x})", Narrow(typeName), Narrow(methodName), static_cast<uint32_t>(rc));
            return nullptr;
        }
        return entry;
    }

} // namespace br::scripting::detail
