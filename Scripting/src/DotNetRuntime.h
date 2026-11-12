#pragma once

// Loads CoreCLR in-process through nethost/hostfxr and resolves [UnmanagedCallersOnly] entry points from assemblies
// loaded into the default AssemblyLoadContext.

#include <filesystem>
#include <string>

namespace br::scripting::detail {

    class DotNetRuntime {
    public:
        // Initializes the runtime described by runtimeConfigPath (a *.runtimeconfig.json). Once per process.
        bool Initialize(const std::filesystem::path& runtimeConfigPath, std::string& error);

        // Loads assemblyPath into the default AssemblyLoadContext and returns a pointer to an
        // [UnmanagedCallersOnly] static method. typeName is assembly-qualified.
        void* GetUnmanagedEntryPoint(const std::filesystem::path& assemblyPath, const wchar_t* typeName, const wchar_t* methodName,
            std::string& error);

    private:
        void* m_loadAssembly = nullptr;        // load_assembly_fn
        void* m_getFunctionPointer = nullptr;  // get_function_pointer_fn
    };

} // namespace br::scripting::detail
