#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Managers/Singletons/brslHelpers.h"

namespace
{
    DxcBuffer MakeBuffer(const std::string& source)
    {
        DxcBuffer buffer = {};
        buffer.Ptr = source.data();
        buffer.Size = source.size();
        buffer.Encoding = 0;
        return buffer;
    }

    void Require(bool condition, const std::string& message)
    {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool Contains(std::string_view haystack, std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }

    std::string LoadUtf8File(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("Failed to open file: " + path.string());
        }

        std::string text(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        ValidateUtf8OrThrow(text, path.string());
        return text;
    }

    void WriteUtf8File(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("Failed to write file: " + path.string());
        }

        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            throw std::runtime_error("Failed to flush file: " + path.string());
        }
    }

    std::unordered_map<std::string, std::string> BuildSequentialReplacementMap(
        const PreparedShaderSource& prepared)
    {
        std::vector<std::string> mandatoryIDs(prepared.mandatoryIDs.begin(), prepared.mandatoryIDs.end());
        std::vector<std::string> optionalIDs(prepared.optionalIDs.begin(), prepared.optionalIDs.end());
        std::sort(mandatoryIDs.begin(), mandatoryIDs.end());
        std::sort(optionalIDs.begin(), optionalIDs.end());

        std::unordered_map<std::string, std::string> replacementMap;
        uint32_t nextIndex = 0;
        for (const std::string& id : mandatoryIDs) {
            replacementMap.emplace(id, "ResourceDescriptorIndex" + std::to_string(nextIndex++));
        }
        for (const std::string& id : optionalIDs) {
            replacementMap.emplace(id, "ResourceDescriptorIndex" + std::to_string(nextIndex++));
        }
        return replacementMap;
    }

    int RunDumpMode(int argc, char** argv)
    {
        if (argc != 5) {
            std::cerr << "Usage: ShaderPreprocessTests dump-final <preprocessed-input> <entry-point> <output>\n";
            return 2;
        }

        const std::filesystem::path inputPath = argv[2];
        const std::string entryPoint = argv[3];
        const std::filesystem::path outputPath = argv[4];

        const std::string source = LoadUtf8File(inputPath);
        PreparedShaderSource prepared = PrepareShaderSourceForEntryPoint(MakeBuffer(source), entryPoint);
        std::unordered_map<std::string, std::string> replacementMap = BuildSequentialReplacementMap(prepared);
        const std::string finalized = FinalizePreparedShaderSource(prepared, replacementMap);
        WriteUtf8File(outputPath, finalized);

        std::cout << "entry=" << entryPoint << '\n';
        std::cout << "mandatoryIDs=" << prepared.mandatoryIDs.size() << '\n';
        std::cout << "optionalIDs=" << prepared.optionalIDs.size() << '\n';
        std::cout << "diagnostics=" << FormatShaderPreprocessDiagnostics(prepared.diagnostics) << '\n';
        std::cout << "output=" << outputPath.string() << '\n';
        return 0;
    }

    void RunTest(
        const char* name,
        const std::function<void()>& fn,
        int& failureCount)
    {
        try {
            fn();
            std::cout << "[PASS] " << name << '\n';
        }
        catch (const std::exception& ex) {
            ++failureCount;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        }
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "dump-final") {
        return RunDumpMode(argc, argv);
    }

    int failureCount = 0;

    RunTest("catalog handles attributes semantics prototypes and overload names", []() {
        const std::string source = R"(
[numthreads(8, 8, 1)]
void DecoratedCS(uint3 tid : SV_DispatchThreadID)
{
}

float ForwardDeclared(float value);

float ForwardDeclared(float value) : SV_Target
{
    return value;
}

float Overload(float value)
{
    return value;
}

float Overload(int value)
{
    return (float)value;
}
)";

        ShaderPreprocessDiagnostics diagnostics =
            AnalyzeShaderSourceCatalog(source.c_str(), source.size());

        Require(diagnostics.parseSucceeded, "tree-sitter failed to parse representative HLSL");
        Require(diagnostics.safeToPrune, "representative HLSL should be safe to prune");
        Require(Contains(JoinStrings(diagnostics.discoveredFunctionDefinitions, ","), "DecoratedCS"),
            "expected DecoratedCS in discovered definitions");
        Require(Contains(JoinStrings(diagnostics.discoveredFunctionDefinitions, ","), "ForwardDeclared"),
            "expected ForwardDeclared in discovered definitions");
        Require(Contains(JoinStrings(diagnostics.discoveredFunctionDefinitions, ","), "Overload"),
            "expected Overload in discovered definitions");
        }, failureCount);

    RunTest("catalog handles dxc line marker between work graph signature and body", []() {
        const std::string source = R"(
struct TraverseNodeRecord
{
    uint nodeIdPacked;
};

template<typename T>
struct GroupNodeInputRecords
{
};

template<typename T>
struct NodeOutput
{
};

[Shader("node")]
[NodeID("TraverseNodes")]
[NodeLaunch("coalescing")]
[NumThreads(32, 1, 1)]
void WG_TraverseNodes(
    [MaxRecords(32)] GroupNodeInputRecords<TraverseNodeRecord> inRecs,
    uint GI : SV_GroupIndex,
    [MaxRecords(128)] NodeOutput<TraverseNodeRecord> TraverseNodes,
    [NodeID("LeafNodes")] [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<TraverseNodeRecord> LeafNodes)
#line 2204 "hlsl.hlsl"
{
}
)";

        ShaderPreprocessDiagnostics diagnostics =
            AnalyzeShaderSourceCatalog(source.c_str(), source.size());

        Require(diagnostics.parseSucceeded, "tree-sitter failed to produce a root for work graph signature");
        Require(diagnostics.safeToPrune, "work graph signature with dxc line marker should be safe to prune");
        Require(Contains(JoinStrings(diagnostics.discoveredFunctionDefinitions, ","), "WG_TraverseNodes"),
            "expected WG_TraverseNodes in discovered definitions");
        }, failureCount);

    RunTest("catalog masks dxc line markers inside parameter and argument lists", []() {
        const std::string source = R"(
float LineMarkedHelper(
    float first
#line 120 "included.hlsli"
    , float second)
{
    return first + second;
}

[numthreads(1, 1, 1)]
void LineMarkedCS()
{
    const float result = LineMarkedHelper(
        1.0f
# 240 "shader.hlsl"
        , 2.0f);
}
)";

        ShaderPreprocessDiagnostics diagnostics =
            AnalyzeShaderSourceCatalog(source.c_str(), source.size());

        Require(diagnostics.parseSucceeded, "tree-sitter failed to produce a root for line-marked lists");
        Require(diagnostics.errorNodeCount == 0, "line markers inside lists should not create parse errors");
        Require(diagnostics.safeToPrune, "line-marked parameter and argument lists should be safe to prune");
        const std::string definitions = JoinStrings(diagnostics.discoveredFunctionDefinitions, ",");
        Require(Contains(definitions, "LineMarkedHelper"), "expected line-marked helper definition");
        Require(Contains(definitions, "LineMarkedCS"), "expected line-marked entry-point definition");
        }, failureCount);

    RunTest("line marker masking preserves pruning byte offsets", []() {
        const std::string source = R"(
float RetainedHelper(float first
#line 40 "included.hlsli"
    , float second)
{
    return first + second;
}

float RemovedHelper()
{
    return 3.0f;
}

[numthreads(1, 1, 1)]
void OffsetPreservingCS()
{
    const float result = RetainedHelper(1.0f
#line 80 "shader.hlsl"
        , 2.0f);
}
)";

        PreparedShaderSource prepared =
            PrepareShaderSourceForEntryPoint(MakeBuffer(source), "OffsetPreservingCS");

        Require(prepared.diagnostics.safeToPrune, "line-marked source should remain safe to prune");
        Require(prepared.diagnostics.pruningApplied, "unused helper should be pruned");
        Require(Contains(prepared.sourceBeforeRewrite, "RetainedHelper"), "retained helper was removed");
        Require(Contains(prepared.sourceBeforeRewrite, "OffsetPreservingCS"), "entry point was removed");
        Require(!Contains(prepared.sourceBeforeRewrite, "RemovedHelper"),
            "tree-sitter offsets did not remove the intended helper");
        }, failureCount);

    RunTest("per-view depth copy drops unreachable helper chain", []() {
        const std::string source = R"(
float UsedHelper()
{
    return 1.0f;
}

float DeadLeaf()
{
    return 2.0f;
}

float ResolveClodSampleFromVisKeyWithFace()
{
    return DeadLeaf();
}

[numthreads(8, 8, 1)]
void PerViewPrimaryDepthCopyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint cameraBufferIndex = ResourceDescriptorIndex(Builtin::CameraBuffer);
    float value = UsedHelper();
}
)";

        PreparedShaderSource prepared =
            PrepareShaderSourceForEntryPoint(MakeBuffer(source), "PerViewPrimaryDepthCopyCS");

        Require(prepared.diagnostics.safeToPrune, "expected pruning to be safe");
        Require(!Contains(prepared.sourceBeforeRewrite, "ResolveClodSampleFromVisKeyWithFace"),
            "unreachable ResolveClodSampleFromVisKeyWithFace should be pruned");
        Require(!Contains(prepared.sourceBeforeRewrite, "DeadLeaf"),
            "unreachable leaf helper should be pruned");
        Require(Contains(prepared.sourceBeforeRewrite, "UsedHelper"),
            "reachable helper should remain");

        std::unordered_map<std::string, std::string> replacementMap = {
            {"Builtin::CameraBuffer", "ResourceDescriptorIndex0"},
        };
        std::string finalized = FinalizePreparedShaderSource(prepared, replacementMap);
        Require(!Contains(finalized, "Builtin::"), "finalized shader should not contain Builtin::");
        Require(CollectResourceDescriptorCallsFromText(finalized).empty(),
            "finalized shader should not contain unresolved descriptor calls");
        }, failureCount);

    RunTest("gbuffer-like root keeps full helper chain and rewrites builtins", []() {
        const std::string source = R"(
float DecodeCompressedPosition()
{
    return 1.0f;
}

float BuildClodMaterialUvCache()
{
    return DecodeCompressedPosition();
}

float ResolveClodSampleFromVisKeyWithFace()
{
    uint cameraIndex = ResourceDescriptorIndex(Builtin::CameraBuffer);
    uint materialIndex = ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer);
    return BuildClodMaterialUvCache() + (float)cameraIndex + (float)materialIndex;
}

float ResolveClodSampleFromVisKey()
{
    return ResolveClodSampleFromVisKeyWithFace();
}

[numthreads(8, 8, 1)]
void GBufferMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    float value = ResolveClodSampleFromVisKey();
}
)";

        PreparedShaderSource prepared =
            PrepareShaderSourceForEntryPoint(MakeBuffer(source), "GBufferMain");

        Require(Contains(prepared.sourceBeforeRewrite, "ResolveClodSampleFromVisKeyWithFace"),
            "reachable resolve helper should remain");
        Require(Contains(prepared.sourceBeforeRewrite, "BuildClodMaterialUvCache"),
            "reachable material helper should remain");
        Require(Contains(prepared.sourceBeforeRewrite, "DecodeCompressedPosition"),
            "reachable decode helper should remain");

        std::unordered_map<std::string, std::string> replacementMap = {
            {"Builtin::CameraBuffer", "ResourceDescriptorIndex0"},
            {"Builtin::PerMaterialDataBuffer", "ResourceDescriptorIndex1"},
        };
        std::string finalized = FinalizePreparedShaderSource(prepared, replacementMap);
        Require(!Contains(finalized, "Builtin::"), "finalized shader should not contain Builtin::");
        Require(CollectResourceDescriptorCallsFromText(finalized).empty(),
            "finalized shader should not contain unresolved descriptor calls");
        }, failureCount);

    RunTest("per-frame builtin rewrite resolves descriptor lookup", []() {
        const std::string source = R"(
[numthreads(8, 8, 1)]
void UsesPerFrameBuiltinCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    uint cameraIndex = ResourceDescriptorIndex(Builtin::CameraBuffer);
    uint frameIndex = perFrameBuffer.frameIndex + cameraIndex;
}
)";

        PreparedShaderSource prepared =
            PrepareShaderSourceForEntryPoint(MakeBuffer(source), "UsesPerFrameBuiltinCS");

        std::unordered_map<std::string, std::string> replacementMap = {
            {"Builtin::PerFrameBuffer", "ResourceDescriptorIndex0"},
            {"Builtin::CameraBuffer", "ResourceDescriptorIndex1"},
        };
        std::string finalized = FinalizePreparedShaderSource(prepared, replacementMap);
        Require(!Contains(finalized, "Builtin::"), "finalized shader should not contain Builtin::");
        Require(CollectResourceDescriptorCallsFromText(finalized).empty(),
            "finalized shader should not contain unresolved descriptor calls");
        }, failureCount);

    RunTest("brdf pixel entry prunes unused fullscreen view-ray shader", []() {
        const std::string source = R"(
struct FULLSCREEN_VS_OUTPUT
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD1;
    float3 viewRayVS : TEXCOORD2;
};

FULLSCREEN_VS_OUTPUT FullscreenVSMain(uint vid : SV_VertexID)
{
    StructuredBuffer<Camera> cameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    FULLSCREEN_VS_OUTPUT o;
    o.position = float4(0.0f, 0.0f, 0.0f, 1.0f);
    o.uv = float2(0.0f, 0.0f);
    o.viewRayVS = cameraBuffer[0].position.xyz;
    return o;
}

FULLSCREEN_VS_OUTPUT FullscreenVSNoViewRayMain(uint vid : SV_VertexID)
{
    FULLSCREEN_VS_OUTPUT o;
    o.position = float4(0.0f, 0.0f, 0.0f, 1.0f);
    o.uv = float2(0.0f, 0.0f);
    o.viewRayVS = float3(0.0f, 0.0f, 0.0f);
    return o;
}

float2 IntegrateBRDF(float NdotV, float roughness)
{
    return float2(NdotV, roughness);
}

float2 PSMain(FULLSCREEN_VS_OUTPUT input) : SV_TARGET
{
    return IntegrateBRDF(input.uv.x, input.uv.y);
}
)";

        PreparedShaderSource prepared =
            PrepareShaderSourceForEntryPoint(MakeBuffer(source), "PSMain");

        Require(prepared.diagnostics.safeToPrune, "expected BRDF-style shader to be safe to prune");
        Require(Contains(prepared.sourceBeforeRewrite, "PSMain"),
            "pixel entry point should remain");
        Require(Contains(prepared.sourceBeforeRewrite, "IntegrateBRDF"),
            "reachable BRDF helper should remain");
        Require(!Contains(prepared.sourceBeforeRewrite, "FullscreenVSMain"),
            "unused view-ray fullscreen shader should be pruned");
        Require(!Contains(prepared.sourceBeforeRewrite, "Builtin::CameraBuffer"),
            "unused fullscreen shader Builtin reference should be pruned before rewrite");

        std::string finalized = FinalizePreparedShaderSource(prepared, {});
        Require(!Contains(finalized, "Builtin::"), "finalized shader should not contain Builtin::");
        Require(CollectResourceDescriptorCallsFromText(finalized).empty(),
            "finalized shader should not contain unresolved descriptor calls");
        }, failureCount);

    RunTest("parse degradation triggers fallback but rewrite still completes", []() {
        const std::string source = R"(
float BrokenFunction(

[numthreads(8, 8, 1)]
void PerViewPrimaryDepthCopyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint cameraBufferIndex = ResourceDescriptorIndex(Builtin::CameraBuffer);
}
)";

        PreparedShaderSource prepared =
            PrepareShaderSourceForEntryPoint(MakeBuffer(source), "PerViewPrimaryDepthCopyCS");

        Require(prepared.diagnostics.usedFallbackRewrite,
            "parse degradation should disable pruning and use fallback rewriting");
        Require(Contains(prepared.sourceBeforeRewrite, "PerViewPrimaryDepthCopyCS"),
            "fallback path should keep the original source");

        std::unordered_map<std::string, std::string> replacementMap = {
            {"Builtin::CameraBuffer", "ResourceDescriptorIndex0"},
        };
        std::string finalized = FinalizePreparedShaderSource(prepared, replacementMap);
        Require(!Contains(finalized, "Builtin::"), "fallback rewritten shader should not contain Builtin::");
        Require(CollectResourceDescriptorCallsFromText(finalized).empty(),
            "fallback rewritten shader should not contain unresolved descriptor calls");
        }, failureCount);

    RunTest("hash canonicalization ignores preprocessor line filenames", []() {
        const std::string sourceA =
            "#line 1 \"C:/temp/run-a/workGraphCulling.hlsl\"\n"
            "#line 12 \"C:/temp/run-a/include/common.hlsli\"\n"
            "[Shader(\"node\")]\n"
            "void WG_ObjectCull() {}\n";
        const std::string sourceB =
            "#line 1 \"D:/different/session/workGraphCulling.hlsl\"\r\n"
            "#line 12 \"D:/different/session/include/common.hlsli\"\r\n"
            "[Shader(\"node\")]\r\n"
            "void WG_ObjectCull() {}\r\n";

        const std::string canonicalA = CanonicalizePreprocessedShaderSourceForHash(sourceA.data(), sourceA.size());
        const std::string canonicalB = CanonicalizePreprocessedShaderSourceForHash(sourceB.data(), sourceB.size());

        Require(canonicalA == canonicalB,
            "canonicalized preprocessed sources should ignore varying #line filenames and newline styles");
        Require(!Contains(canonicalA, "#line"),
            "canonicalized source should drop line directives from the hash input");
        Require(Contains(canonicalA, "WG_ObjectCull"),
            "canonicalized source should preserve shader code");
        }, failureCount);

    RunTest("invalid utf8 is rejected", []() {
        const std::string invalidUtf8("\xC3\x28", 2);
        bool threw = false;
        try {
            auto unusedPrepared = PrepareShaderSourceForEntryPoint(MakeBuffer(invalidUtf8), "Main");
            (void)unusedPrepared;
        }
        catch (const std::runtime_error&) {
            threw = true;
        }

        Require(threw, "invalid UTF-8 should throw");
        }, failureCount);

    RunTest("descriptor overflow is detectable", []() {
        std::string source = R"(
[numthreads(8, 8, 1)]
void OverflowCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
)";

        for (int i = 0; i < 33; ++i) {
            source += "    uint v" + std::to_string(i) + " = ResourceDescriptorIndex(Builtin::ID" + std::to_string(i) + ");\n";
        }
        source += "}\n";

        PreparedShaderSource prepared =
            PrepareShaderSourceForEntryPoint(MakeBuffer(source), "OverflowCS");
        Require(prepared.mandatoryIDs.size() == 33, "expected 33 collected descriptor identifiers");
        Require(prepared.mandatoryIDs.size() > 32, "overflow case should exceed the root constant budget");
        }, failureCount);

    RunTest("shader repository files are valid utf8", []() {
        const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
        const std::filesystem::path shaderRoot = repoRoot / "shaders";

        std::vector<std::filesystem::path> invalidFiles;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(shaderRoot)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const std::string extension = entry.path().extension().string();
            if (extension != ".hlsl" && extension != ".hlsli" && extension != ".h") {
                continue;
            }

            std::ifstream input(entry.path(), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (!IsValidUtf8(bytes)) {
                invalidFiles.push_back(entry.path());
            }
        }

        Require(invalidFiles.empty(),
            "found non-UTF-8 shader files under " + shaderRoot.string());
        }, failureCount);

    if (failureCount != 0) {
        std::cerr << failureCount << " shader preprocessing test(s) failed\n";
        return 1;
    }

    std::cout << "All shader preprocessing tests passed\n";
    return 0;
}
