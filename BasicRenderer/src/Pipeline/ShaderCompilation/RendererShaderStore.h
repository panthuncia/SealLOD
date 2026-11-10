#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ORGModuleServices/ShaderStore.h>

// The renderer's one persistent shader cache: org::services::ShaderStore in <cache root>/shaders/store.
//
// An artifact is valid while every file its build read still has the same contents, so editing one shader rebuilds only
// the variants that read it. Paths are recorded relative to the executable's folder, which holds shaders/, SARPShaders/
// and NVSL/, so the offline preprocessor and a renderer installed elsewhere share artifacts.
namespace br::shaderstore {

// The format byte of a stored artifact. A renderer artifact is a whole bundle (below), not a bare DXIL or SPIR-V module,
// so it does not use ORG's own tags (DXIL 0, SPIR-V 1, DXBC 2).
inline constexpr uint8_t kFormatBundleDxil = 0x10;
inline constexpr uint8_t kFormatBundleSpirv = 0x11;

enum class StageKind : uint8_t {
    Vertex = 1,
    Pixel = 2,
    Amplification = 3,
    Mesh = 4,
    Compute = 5,
    Library = 6,
};

// What one stored artifact holds: every stage's bytecode and the BRSL resource slots they share.
struct BundleArtifact {
    struct Stage {
        StageKind kind = StageKind::Vertex;
        std::vector<std::byte> bytecode;
    };
    std::vector<Stage> stages;
    // As BRSL names them, before the runtime aliases.
    std::vector<std::string> mandatoryIds;
    std::vector<std::string> optionalIds;
    uint64_t resourceIdsHash = 0;
};

std::vector<std::byte> EncodeBundle(const BundleArtifact& artifact);
// False for anything that is not a whole bundle of this layout.
bool DecodeBundle(std::span<const std::byte> bytes, BundleArtifact& artifact);
// Bump when EncodeBundle's layout changes. It is part of every key.
inline constexpr uint32_t kBundleLayoutVersion = 1;

// Opened on first use. The cache root must be chosen by then.
org::services::ShaderStore& Store();
// The folder every shader path is relative to: the executable's.
const std::filesystem::path& SourceRoot();
// The loaded dxcompiler.dll's contents, hashed once. Contents, not path or time: the offline preprocessor and the
// installed renderer load byte-identical copies from different folders.
const org::services::ShaderStoreHash& CompilerIdentity();

// Source files may have changed (a reload, a live recompile): later requests read them again. Safe before the store opens.
void BeginGeneration();

// Counts a request, so the frame loop knows a batch has something to report.
void NoteRequest() noexcept;
bool HasUnreportedRequests() noexcept;
// For the frame loop, once no compile is pending: when requests came and none arrived for a while (a batch ended),
// logs FinishBatch's line on a thread of its own, since saving the index can take milliseconds.
void FinishBatchWhenSettled();
// Saves the index and logs one line: what every request so far cost, and why anything was not a plain hit. Returns it.
std::string FinishBatch(std::string_view batch);
// The same line and the store's size, as JSON, for the control pipe.
std::string DescribeJson();

// Logs each request that was not a plain hit (SARP_SHADER_CACHE_DIAGNOSTICS=1).
bool DiagnosticsEnabled();
const char* OutcomeName(org::services::ShaderStoreOutcome outcome);
const char* MissName(org::services::ShaderStoreMiss miss);

// The compile manifest's file (see PSOManager::PrecompileRecordedShaderBundles).
std::filesystem::path ManifestPath();

// Generated shader code (LensManager's polynomial lens): sources that live in memory, included by file name from any folder
// (`#include "generated/<name>"`). They are not file dependencies of the store: a request that includes one must name it
// in its defines, and the name must change with the text (put its hash in it), so the variant key does. Registered
// sources stay for the process's lifetime.
void SetGeneratedSource(const std::string& name, std::string text);
// The text registered under a file name, or null.
std::shared_ptr<const std::string> GeneratedSource(std::string_view name);

} // namespace br::shaderstore
