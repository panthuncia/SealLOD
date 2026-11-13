using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace BasicRenderer.Scripting.Runtime;

internal sealed class CompileOutput
{
    public byte[]? Assembly { get; init; }
    public byte[]? Pdb { get; init; }
    public List<CompileDiagnostic> Diagnostics { get; init; } = new();
    public bool Succeeded => Assembly != null;
}

/// <summary>
/// Compiles loose scenario sources with Roslyn into an in-memory assembly plus portable PDB, so exceptions carry
/// file:line. Thread-safe; the session runs it off the main thread.
/// </summary>
internal static class ScenarioCompiler
{
    /// <summary>
    /// The usings every scenario file gets implicitly. Scenarios/Scenarios.csproj declares the same list for the IDE;
    /// keep them in step.
    /// </summary>
    public static readonly string[] GlobalUsings =
    [
        "System",
        "System.Collections.Generic",
        "System.Linq",
        "System.Threading",
        "System.Threading.Tasks",
        "BasicRenderer.Scripting",
    ];

    private static readonly Lazy<IReadOnlyList<MetadataReference>> s_references = new(LoadReferences);

    /// <summary>A .cs file, or every .cs file under a directory (bin/ and obj/ excluded), in a stable order.</summary>
    public static IReadOnlyList<string> CollectSources(string path)
    {
        if (File.Exists(path)) return [Path.GetFullPath(path)];
        if (!Directory.Exists(path)) throw new FileNotFoundException($"script source not found: {path}");
        var root = Path.GetFullPath(path);
        return Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories)
            .Where(file => !IsBuildOutput(Path.GetRelativePath(root, file)))
            .OrderBy(file => file, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    public static CompileOutput Compile(IReadOnlyList<string> sourceFiles, string assemblyName)
    {
        var parseOptions = new CSharpParseOptions(LanguageVersion.Latest);
        var trees = new List<SyntaxTree>(sourceFiles.Count + 1);
        foreach (var file in sourceFiles) {
            using var stream = File.OpenRead(file);
            var text = SourceText.From(stream, Encoding.UTF8, canBeEmbedded: true);
            trees.Add(CSharpSyntaxTree.ParseText(text, parseOptions, file));
        }
        var usings = string.Join('\n', GlobalUsings.Select(u => $"global using {u};"));
        trees.Add(CSharpSyntaxTree.ParseText(SourceText.From(usings, Encoding.UTF8), parseOptions, "<global usings>"));

        var options = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary)
            .WithOptimizationLevel(OptimizationLevel.Debug)
            .WithNullableContextOptions(NullableContextOptions.Enable)
            .WithConcurrentBuild(true)
            .WithSpecificDiagnosticOptions(new Dictionary<string, ReportDiagnostic>
            {
                // Reference-version unification notes; the runtime's own assemblies are the references.
                ["CS1701"] = ReportDiagnostic.Suppress,
                ["CS1702"] = ReportDiagnostic.Suppress,
            });
        var compilation = CSharpCompilation.Create(assemblyName, trees, s_references.Value, options);

        using var peStream = new MemoryStream();
        using var pdbStream = new MemoryStream();
        var emitOptions = new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb);
        var embedded = trees.Where(t => t.FilePath != "<global usings>").Select(t => EmbeddedText.FromSource(t.FilePath, t.GetText()));
        EmitResult emitted = compilation.Emit(peStream, pdbStream, options: emitOptions, embeddedTexts: embedded);

        var diagnostics = new List<CompileDiagnostic>();
        bool unawaitedTask = false;
        foreach (var diagnostic in emitted.Diagnostics.Where(d => d.Severity is DiagnosticSeverity.Error or DiagnosticSeverity.Warning)) {
            if (diagnostic.Id == "CS4014") {
                // Unawaited: fine for an animation (they run alongside the scenario), a mistake for a Task (the wait
                // would not happen), so that is an error.
                if (IsAnimation(compilation, diagnostic)) continue;
                unawaitedTask = true;
                diagnostics.Add(ToDiagnostic(diagnostic) with
                {
                    Severity = "error",
                    Message = "this Task is not awaited, so the scenario does not wait for it: add 'await' (or '_ =' to run it in the background)",
                });
                continue;
            }
            diagnostics.Add(ToDiagnostic(diagnostic));
        }
        diagnostics = diagnostics.OrderBy(d => d.Severity == "error" ? 0 : 1).ToList();
        if (!emitted.Success || unawaitedTask) return new CompileOutput { Diagnostics = diagnostics };
        return new CompileOutput { Assembly = peStream.ToArray(), Pdb = pdbStream.ToArray(), Diagnostics = diagnostics };
    }

    private static bool IsAnimation(CSharpCompilation compilation, Diagnostic diagnostic)
    {
        var tree = diagnostic.Location.SourceTree;
        if (tree == null) return false;
        var node = tree.GetRoot().FindNode(diagnostic.Location.SourceSpan, getInnermostNodeForTie: true);
        if (node is Microsoft.CodeAnalysis.CSharp.Syntax.ExpressionStatementSyntax statement) node = statement.Expression;
        var type = compilation.GetSemanticModel(tree).GetTypeInfo(node).Type;
        var animation = compilation.GetTypeByMetadataName("BasicRenderer.Scripting.Animation");
        for (var t = type; t != null; t = t.BaseType) {
            if (SymbolEqualityComparer.Default.Equals(t, animation)) return true;
        }
        return false;
    }

    private static CompileDiagnostic ToDiagnostic(Diagnostic diagnostic)
    {
        var span = diagnostic.Location.GetMappedLineSpan();
        return new CompileDiagnostic(
            span.Path ?? "",
            span.StartLinePosition.Line + 1,
            span.StartLinePosition.Character + 1,
            diagnostic.Id,
            diagnostic.Severity == DiagnosticSeverity.Error ? "error" : "warning",
            diagnostic.GetMessage());
    }

    private static IReadOnlyList<MetadataReference> LoadReferences()
    {
        // The running framework's own assemblies. Under hostfxr the trusted platform list is exactly the shared
        // framework, so scenarios compile against what they will run on.
        var trusted = (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string ?? "")
            .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
            .Where(path => IsFrameworkAssembly(Path.GetFileName(path)));
        var scripting = typeof(Scenario).Assembly.Location;
        return trusted.Append(scripting)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Select(path => (MetadataReference)MetadataReference.CreateFromFile(path))
            .ToList();
    }

    private static bool IsFrameworkAssembly(string fileName) =>
        fileName.StartsWith("System.", StringComparison.OrdinalIgnoreCase)
        || fileName.Equals("System.dll", StringComparison.OrdinalIgnoreCase)
        || fileName.Equals("netstandard.dll", StringComparison.OrdinalIgnoreCase)
        || fileName.Equals("mscorlib.dll", StringComparison.OrdinalIgnoreCase)
        || fileName.Equals("Microsoft.CSharp.dll", StringComparison.OrdinalIgnoreCase)
        || fileName.Equals("Microsoft.Win32.Primitives.dll", StringComparison.OrdinalIgnoreCase);

    private static bool IsBuildOutput(string relativePath)
    {
        var first = relativePath.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)[0];
        return first.Equals("bin", StringComparison.OrdinalIgnoreCase) || first.Equals("obj", StringComparison.OrdinalIgnoreCase);
    }
}
