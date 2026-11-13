using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace BasicRenderer.Scripting.Runtime;

/// <summary>
/// Compiled scenario assemblies by content: the SHA-256 of every source file's path and bytes, the implicit usings,
/// and the identity of this runtime and of Roslyn. Unchanged sources skip Roslyn (about 1.8 s on a cold start); any
/// edit, or a rebuilt runtime, misses. Held in memory for the process and on disk under
/// <c>BASICRENDERER_SCRIPT_CACHE</c> (default: cache\scripts beside the executable; "off" disables the disk).
/// </summary>
internal static class CompileCache
{
    private static readonly ConcurrentDictionary<string, CompileOutput> s_memory = new();
    private static readonly JsonSerializerOptions s_json = new() { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };

    public static string Key(IReadOnlyList<string> sources)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        void Add(string text) => hash.AppendData(Encoding.UTF8.GetBytes(text + "\n"));
        Add("v1");
        Add(typeof(Scenario).Assembly.ManifestModule.ModuleVersionId.ToString());
        Add(typeof(Microsoft.CodeAnalysis.Compilation).Assembly.ManifestModule.ModuleVersionId.ToString());
        Add(string.Join(';', ScenarioCompiler.GlobalUsings));
        foreach (var file in sources) {
            Add(file.ToLowerInvariant());
            hash.AppendData(File.ReadAllBytes(file));
        }
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    public static string? Directory
    {
        get
        {
            var configured = Environment.GetEnvironmentVariable("BASICRENDERER_SCRIPT_CACHE");
            if (string.Equals(configured, "off", StringComparison.OrdinalIgnoreCase)) return null;
            if (!string.IsNullOrWhiteSpace(configured)) return configured;
            var exe = Environment.ProcessPath;
            if (exe == null) return null;
            // Never beside a shared dotnet.exe (managed tests): only a host application gets a disk cache by default.
            if (string.Equals(Path.GetFileNameWithoutExtension(exe), "dotnet", StringComparison.OrdinalIgnoreCase)) return null;
            return Path.Combine(Path.GetDirectoryName(exe)!, "cache", "scripts");
        }
    }

    public static bool TryGet(string key, out CompileOutput output)
    {
        if (s_memory.TryGetValue(key, out output!)) return true;
        var directory = Directory;
        if (directory == null) return false;
        try {
            var dll = Path.Combine(directory, key + ".dll");
            var pdb = Path.Combine(directory, key + ".pdb");
            var diagnostics = Path.Combine(directory, key + ".json");
            if (!File.Exists(dll) || !File.Exists(pdb)) return false;
            output = new CompileOutput
            {
                Assembly = File.ReadAllBytes(dll),
                Pdb = File.ReadAllBytes(pdb),
                Diagnostics = File.Exists(diagnostics)
                    ? JsonSerializer.Deserialize<List<CompileDiagnostic>>(File.ReadAllText(diagnostics), s_json) ?? new()
                    : new(),
            };
            s_memory[key] = output;
            return true;
        } catch (Exception) {
            return false;
        }
    }

    public static void Store(string key, CompileOutput output)
    {
        if (!output.Succeeded) return;
        s_memory[key] = output;
        var directory = Directory;
        if (directory == null) return;
        try {
            System.IO.Directory.CreateDirectory(directory);
            // Written beside, then renamed, so a reader never sees half an assembly.
            WriteAtomically(Path.Combine(directory, key + ".json"), JsonSerializer.SerializeToUtf8Bytes(output.Diagnostics, s_json));
            WriteAtomically(Path.Combine(directory, key + ".pdb"), output.Pdb!);
            WriteAtomically(Path.Combine(directory, key + ".dll"), output.Assembly!);
        } catch (Exception e) {
            // A cache that cannot be written only costs the next compile.
            Interop.Native.Log(Interop.NativeLogLevel.Warn, $"could not write the scenario compile cache in {directory}: {e.Message}");
        }
    }

    private static void WriteAtomically(string path, byte[] bytes)
    {
        var partial = path + ".partial";
        File.WriteAllBytes(partial, bytes);
        File.Move(partial, path, overwrite: true);
    }
}
