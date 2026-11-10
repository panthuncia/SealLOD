// The renderer's use of org::services::ShaderStore (Pipeline/ShaderCompilation/RendererShaderStore.h): the bundle
// artifact layout, and that a store keeps hits across two source folders with the same files, as the offline
// preprocessor's build directory and an installed renderer have.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <ORGModuleServices/ShaderStore.h>

#include "Pipeline/ShaderCompilation/RendererShaderStore.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++g_failures;
    }
}

std::vector<std::byte> Bytes(std::string_view text)
{
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return { first, first + text.size() };
}

void BundleRoundTrip()
{
    br::shaderstore::BundleArtifact artifact;
    artifact.stages.push_back({ br::shaderstore::StageKind::Mesh, Bytes("mesh bytecode") });
    artifact.stages.push_back({ br::shaderstore::StageKind::Pixel, Bytes("pixel bytecode") });
    artifact.mandatoryIds = { "Builtin::PerFrameBuffer", "Builtin::CameraBuffer" };
    artifact.optionalIds = { "Builtin::Surface::Records" };
    artifact.resourceIdsHash = 0x1234567890ABCDEFull;

    const auto encoded = br::shaderstore::EncodeBundle(artifact);
    br::shaderstore::BundleArtifact decoded;
    Check(br::shaderstore::DecodeBundle(encoded, decoded), "a bundle decodes");
    Check(decoded.stages.size() == 2 && decoded.stages[0].kind == br::shaderstore::StageKind::Mesh &&
        decoded.stages[1].bytecode == artifact.stages[1].bytecode, "stages survive");
    Check(decoded.mandatoryIds == artifact.mandatoryIds && decoded.optionalIds == artifact.optionalIds, "resource slots survive");
    Check(decoded.resourceIdsHash == artifact.resourceIdsHash, "the resource hash survives");

    for (size_t size = 0; size < encoded.size(); ++size) {
        br::shaderstore::BundleArtifact truncated;
        if (br::shaderstore::DecodeBundle(std::span(encoded).first(size), truncated)) {
            Check(false, "a truncated bundle is rejected");
            break;
        }
    }
    auto trailing = encoded;
    trailing.push_back(std::byte{ 0 });
    Check(!br::shaderstore::DecodeBundle(trailing, decoded), "trailing bytes are rejected");
    auto foreign = encoded;
    foreign[0] = std::byte{ 'X' };
    Check(!br::shaderstore::DecodeBundle(foreign, decoded), "another magic is rejected");
}

// Stands in for PSOManager's build: reads its root and one include through the tracker, as DXC's include handler does.
class FakeBuild final : public org::services::ShaderVariantBuild {
public:
    explicit FakeBuild(std::filesystem::path sourceRoot) : m_sourceRoot(std::move(sourceRoot)) {}
    int compiles = 0;

    org::services::ShaderPreprocessOutput Preprocess(const std::string*, org::services::ShaderIncludeTracker& includes) override {
        org::services::ShaderPreprocessOutput output;
        const std::string* root = includes.Open(m_sourceRoot / "shaders/root.hlsl");
        includes.Open(m_sourceRoot / "shaders/missing.hlsli"); // A path DXC tries and does not find.
        const std::string* include = includes.Open(m_sourceRoot / "shaders/include/common.hlsli");
        if (!root || !include) return output;
        output.text = *include + "\n" + *root;
        output.succeeded = true;
        return output;
    }

    org::services::ShaderCompileOutput Compile(const std::string*, const org::services::ShaderPreprocessOutput& preprocessed, org::services::ShaderIncludeTracker&) override {
        ++compiles;
        br::shaderstore::BundleArtifact artifact;
        artifact.stages.push_back({ br::shaderstore::StageKind::Compute, Bytes(preprocessed.text) });
        return { true, false, br::shaderstore::EncodeBundle(artifact), {} };
    }

private:
    std::filesystem::path m_sourceRoot;
};

void WriteFile(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
}

org::services::ShaderStoreOutcome Resolve(const std::filesystem::path& sourceRoot, const std::filesystem::path& cache, int& compiles)
{
    org::services::ShaderStoreOptions options;
    options.sourceRoot = sourceRoot;
    options.cacheDirectory = cache;
    options.saveInterval = std::chrono::milliseconds(0);
    org::services::ShaderStore store(options);
    const std::string recipe = "5\x1fshaders/root.hlsl\x1fMainCS\x1f" "cs_6_6\n";
    FakeBuild build(sourceRoot);
    org::services::ShaderVariantRequest request;
    request.variantKey = org::services::HashShaderText("variant");
    request.compileConfigKey = org::services::HashShaderText("config");
    request.format = br::shaderstore::kFormatBundleDxil;
    request.rootBytes = std::as_bytes(std::span(recipe));
    request.hasRootBytes = true;
    request.build = &build;
    const auto result = store.Resolve(request);
    compiles += build.compiles;
    return result.outcome;
}

void HitsAcrossSourceFolders()
{
    const auto base = std::filesystem::temp_directory_path() / "RendererShaderStoreTests";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    const auto build = base / "build", installed = base / "installed folder", cache = base / "cache";
    for (const auto& root : { build, installed }) {
        WriteFile(root / "shaders/root.hlsl", "#include \"include/common.hlsli\"\n[numthreads(1,1,1)] void MainCS() {}\n");
        WriteFile(root / "shaders/include/common.hlsli", "static const uint kValue = 1;\n");
    }

    int compiles = 0;
    Check(Resolve(build, cache, compiles) == org::services::ShaderStoreOutcome::Compiled, "the first request compiles");
    Check(Resolve(installed, cache, compiles) == org::services::ShaderStoreOutcome::Hit, "another folder with the same files hits");

    WriteFile(installed / "shaders/include/common.hlsli", "static const uint kValue = 1; // a comment\n");
    Check(Resolve(installed, cache, compiles) == org::services::ShaderStoreOutcome::Compiled, "a changed include builds again");
    WriteFile(installed / "shaders/missing.hlsli", "// now it exists\n");
    Check(Resolve(installed, cache, compiles) == org::services::ShaderStoreOutcome::Absorbed,
        "a file that appears on a tried path is a change; the same text is absorbed");
    Check(compiles == 2, "only changed text compiles");
    std::filesystem::remove_all(base, ec);
}

} // namespace

int main()
{
    BundleRoundTrip();
    HitsAcrossSourceFolders();
    if (g_failures) return EXIT_FAILURE;
    std::printf("RendererShaderStoreTests passed\n");
    return EXIT_SUCCESS;
}
