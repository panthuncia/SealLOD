#include "Pipeline/ShaderCompilation/RendererShaderStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/CachePaths.h>
#include "Utilities/Utilities.h"

namespace br::shaderstore {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kBundleMagic = 0x41535242; // "BRSA"

std::atomic<org::services::ShaderStore*> g_openedStore{ nullptr };
std::atomic<uint64_t> g_requests{ 0 };
std::atomic<uint64_t> g_reportedRequests{ 0 };
std::atomic<int64_t> g_lastRequestTicks{ 0 };
std::atomic<bool> g_batchReportRunning{ false };
// Set once the old manifest's lines are in the new one (or there were none): only then may the old file go.
std::atomic<bool> g_oldManifestMerged{ false };
// How long no request may arrive before a batch counts as ended. Startup compiles in waves; this keeps it to one line.
constexpr std::chrono::milliseconds kBatchSettleTime{ 2000 };

int64_t NowTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

fs::path StoreDirectory() { return GetCacheRootPath() / L"shaders" / L"store"; }
fs::path OldDirectory(const wchar_t* format) { return GetCacheRootPath() / L"shaders" / format; }
fs::path OldManifestPath() { return OldDirectory(L"dxil") / L"compiled_bundles.txt"; }

uint64_t MaxBytes()
{
    uint64_t megabytes = 4096;
    if (const char* text = std::getenv("SARP_SHADER_STORE_MAX_MB"); text && text[0] != '\0') {
        megabytes = std::strtoull(text, nullptr, 10);
    }
    return megabytes << 20;
}

bool ReadWholeFile(const fs::path& path, std::string& out)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const auto size = stream.tellg();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    stream.seekg(0);
    return out.empty() || static_cast<bool>(stream.read(out.data(), static_cast<std::streamsize>(out.size())));
}

// The old cache (schema 7: shaders/dxil and shaders/spirv, every artifact keyed on one digest of the whole source tree)
// is never read again, and nothing else would ever delete it. Its compile manifest moves first: the offline
// preprocessor replays it. The rest is deleted on its own thread, since it can be thousands of files, and a later start
// finishes what an exit interrupted. Only the old artifact names are touched.
void RetireOldCache()
{
    (void)ManifestPath(); // Moves the old manifest first, if nothing has asked for it yet.
    std::thread([] {
        uint64_t removed = 0, bytes = 0;
        for (const wchar_t* format : { L"dxil", L"spirv" }) {
            const fs::path directory = OldDirectory(format);
            std::error_code ec;
            for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
                const auto name = it->path().filename().wstring();
                const bool artifact = (name.starts_with(L"shader_bundle_") || name.starts_with(L"shader_library_")) && name.ends_with(L".bin");
                const bool manifest = name == L"compiled_bundles.txt" && g_oldManifestMerged.load();
                if (!artifact && !manifest) continue;
                std::error_code entryError;
                const auto size = it->file_size(entryError);
                if (fs::remove(it->path(), entryError)) {
                    ++removed;
                    bytes += entryError ? 0 : size;
                }
            }
            std::error_code removeError;
            fs::remove(directory, removeError); // Only if empty: anything else there is not ours.
        }
        if (removed) {
            spdlog::info("Shader store: deleted {} files ({} MB) of the old shader cache in {}", removed, bytes >> 20,
                (GetCacheRootPath() / L"shaders").string());
        }
    }).detach();
}

org::services::ShaderStore& OpenStore()
{
    // Never destroyed: static destructors may still compile (a late pipeline build), and the index is saved as it
    // changes, so nothing is lost at exit. Windows releases the store's ownership mutex with the process.
    static org::services::ShaderStore* const store = [] {
        org::services::ShaderStoreOptions options;
        options.sourceRoot = SourceRoot();
        options.cacheDirectory = StoreDirectory();
        options.maxBytes = MaxBytes();
        auto* created = new org::services::ShaderStore(std::move(options));
        if (!created->OwnsIndex()) {
            spdlog::warn("Shader store: another process owns {}; this one uses it but does not save its index", StoreDirectory().string());
        }
        const auto info = created->IndexInfo();
        spdlog::info("Shader store opened at {} (sources under {}): {} variants, {} artifacts, {} MB, {} evicted, limit {} MB",
            StoreDirectory().string(), SourceRoot().string(), info.variants, info.artifactsAtStart, info.artifactBytesAtStart >> 20,
            info.artifactsEvicted, MaxBytes() >> 20);
        if (info.overCapBytes) {
            spdlog::warn("Shader store is {} MB over its limit, because the last starts used all of it. Raise SARP_SHADER_STORE_MAX_MB "
                "if you switch between builds or settings often.", info.overCapBytes >> 20);
        }
        RetireOldCache();
        g_openedStore = created;
        return created;
    }();
    return *store;
}

template<typename T>
void Append(std::vector<std::byte>& out, const T& value)
{
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), first, first + sizeof(T));
}

void AppendText(std::vector<std::byte>& out, std::string_view text)
{
    Append(out, static_cast<uint32_t>(text.size()));
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    out.insert(out.end(), first, first + text.size());
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : m_bytes(bytes) {}
    template<typename T> bool Value(T& out)
    {
        if (m_bytes.size() - m_position < sizeof(T)) return false;
        std::memcpy(&out, m_bytes.data() + m_position, sizeof(T));
        m_position += sizeof(T);
        return true;
    }
    bool Bytes(size_t size, std::span<const std::byte>& out)
    {
        if (m_bytes.size() - m_position < size) return false;
        out = m_bytes.subspan(m_position, size);
        m_position += size;
        return true;
    }
    bool Text(std::string& out)
    {
        uint32_t size = 0;
        std::span<const std::byte> bytes;
        if (!Value(size) || !Bytes(size, bytes)) return false;
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }
    bool Done() const { return m_position == m_bytes.size(); }

private:
    std::span<const std::byte> m_bytes;
    size_t m_position = 0;
};

bool ReadTexts(Reader& reader, std::vector<std::string>& out)
{
    uint32_t count = 0;
    if (!reader.Value(count)) return false;
    out.clear();
    for (uint32_t index = 0; index < count; ++index) {
        if (!reader.Text(out.emplace_back())) return false;
    }
    return true;
}

} // namespace

std::vector<std::byte> EncodeBundle(const BundleArtifact& artifact)
{
    std::vector<std::byte> out;
    Append(out, kBundleMagic);
    Append(out, kBundleLayoutVersion);
    Append(out, artifact.resourceIdsHash);
    for (const auto* ids : { &artifact.mandatoryIds, &artifact.optionalIds }) {
        Append(out, static_cast<uint32_t>(ids->size()));
        for (const auto& id : *ids) AppendText(out, id);
    }
    Append(out, static_cast<uint32_t>(artifact.stages.size()));
    for (const auto& stage : artifact.stages) {
        Append(out, static_cast<uint8_t>(stage.kind));
        Append(out, static_cast<uint64_t>(stage.bytecode.size()));
        out.insert(out.end(), stage.bytecode.begin(), stage.bytecode.end());
    }
    return out;
}

bool DecodeBundle(std::span<const std::byte> bytes, BundleArtifact& artifact)
{
    Reader reader(bytes);
    uint32_t magic = 0, version = 0, stageCount = 0;
    if (!reader.Value(magic) || magic != kBundleMagic || !reader.Value(version) || version != kBundleLayoutVersion) return false;
    if (!reader.Value(artifact.resourceIdsHash) || !ReadTexts(reader, artifact.mandatoryIds) || !ReadTexts(reader, artifact.optionalIds)) return false;
    if (!reader.Value(stageCount)) return false;
    artifact.stages.clear();
    for (uint32_t index = 0; index < stageCount; ++index) {
        uint8_t kind = 0;
        uint64_t size = 0;
        std::span<const std::byte> code;
        if (!reader.Value(kind) || !reader.Value(size) || size == 0 || !reader.Bytes(static_cast<size_t>(size), code)) return false;
        if (kind < static_cast<uint8_t>(StageKind::Vertex) || kind > static_cast<uint8_t>(StageKind::Library)) return false;
        artifact.stages.push_back({ static_cast<StageKind>(kind), std::vector<std::byte>(code.begin(), code.end()) });
    }
    return reader.Done();
}

org::services::ShaderStore& Store() { return OpenStore(); }

const fs::path& SourceRoot()
{
    static const fs::path root = fs::path(GetExePath()).lexically_normal();
    return root;
}

const org::services::ShaderStoreHash& CompilerIdentity()
{
    static const org::services::ShaderStoreHash identity = [] {
        wchar_t modulePath[MAX_PATH] = {};
        const HMODULE module = GetModuleHandleW(L"dxcompiler.dll");
        std::string bytes;
        if (!module || GetModuleFileNameW(module, modulePath, MAX_PATH) == 0 || !ReadWholeFile(modulePath, bytes)) {
            spdlog::warn("Shader store: could not read the loaded dxcompiler.dll, so shaders are keyed without its identity");
            return org::services::ShaderStoreHash{};
        }
        return org::services::HashShaderText(bytes);
    }();
    return identity;
}

void BeginGeneration()
{
    if (auto* store = g_openedStore.load()) store->BeginGeneration();
}

void NoteRequest() noexcept
{
    g_lastRequestTicks.store(NowTicks(), std::memory_order_relaxed);
    g_requests.fetch_add(1, std::memory_order_relaxed);
}

void FinishBatchWhenSettled()
{
    if (!HasUnreportedRequests() || g_batchReportRunning.load(std::memory_order_relaxed)) return;
    const auto settle = std::chrono::duration_cast<std::chrono::steady_clock::duration>(kBatchSettleTime).count();
    if (NowTicks() - g_lastRequestTicks.load(std::memory_order_relaxed) < settle) return;
    if (g_batchReportRunning.exchange(true)) return;
    std::thread([] {
        try {
            FinishBatch("shader compilation");
        } catch (const std::exception& error) {
            spdlog::warn("Shader store: could not report the batch: {}", error.what());
        }
        g_batchReportRunning = false;
    }).detach();
}

bool HasUnreportedRequests() noexcept
{
    return g_requests.load(std::memory_order_relaxed) != g_reportedRequests.load(std::memory_order_relaxed);
}

std::string FinishBatch(std::string_view batch)
{
    g_reportedRequests.store(g_requests.load(std::memory_order_relaxed), std::memory_order_relaxed);
    auto& store = Store();
    store.Flush();
    const auto info = store.IndexInfo();
    std::string line = "Shader store after " + std::string(batch) + ": " + store.Summary() + " Index: " + std::to_string(info.variants) +
        " variants, " + std::to_string(info.dependencySets) + " dependency sets, " + std::to_string(info.dependencyEntries) + " entries, " +
        std::to_string(info.files) + " files, " + std::to_string(info.savedBytes >> 10) + " KB saved.";
    spdlog::info("{}", line);
    return line;
}

std::string DescribeJson()
{
    auto& store = Store();
    const auto stats = store.Stats();
    const auto info = store.IndexInfo();
    const auto usage = store.MeasureArtifacts();
    nlohmann::json misses;
    for (size_t index = 1; index < static_cast<size_t>(org::services::ShaderStoreMiss::Count); ++index) {
        misses[MissName(static_cast<org::services::ShaderStoreMiss>(index))] = stats.misses[index];
    }
    const nlohmann::json json{
        { "summary", store.Summary() },
        { "directory", StoreDirectory().string() },
        { "source_root", SourceRoot().string() },
        { "owns_index", store.OwnsIndex() },
        { "generation", store.Generation() },
        { "requests", stats.requests },
        { "hits", stats.hits },
        { "absorbed", stats.absorbed },
        { "compiled", stats.compiled },
        { "failed", stats.failed },
        { "failures_remembered", stats.failuresFromCache },
        { "not_saved", stats.notSaved },
        { "preprocessed", stats.preprocessed },
        { "source_files_read", stats.sourceFilesRead },
        { "misses", misses },
        { "first_changed_file", stats.firstChangedFile },
        { "variants", info.variants },
        { "dependency_sets", info.dependencySets },
        { "artifacts", usage.count },
        { "artifact_bytes", usage.bytes },
        { "artifacts_evicted_at_start", info.artifactsEvicted },
        { "max_bytes", MaxBytes() },
    };
    return json.dump();
}

bool DiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SARP_SHADER_CACHE_DIAGNOSTICS");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

const char* OutcomeName(org::services::ShaderStoreOutcome outcome)
{
    switch (outcome) {
    case org::services::ShaderStoreOutcome::Hit: return "hit";
    case org::services::ShaderStoreOutcome::Absorbed: return "absorbed";
    case org::services::ShaderStoreOutcome::Compiled: return "compiled";
    case org::services::ShaderStoreOutcome::Failed: return "failed";
    }
    return "unknown";
}

const char* MissName(org::services::ShaderStoreMiss miss)
{
    switch (miss) {
    case org::services::ShaderStoreMiss::None: return "none";
    case org::services::ShaderStoreMiss::NoRecord: return "no_record";
    case org::services::ShaderStoreMiss::RootChanged: return "root_changed";
    case org::services::ShaderStoreMiss::DependencyChanged: return "dependency_changed";
    case org::services::ShaderStoreMiss::ArtifactInvalid: return "artifact_invalid";
    case org::services::ShaderStoreMiss::Disabled: return "disabled";
    case org::services::ShaderStoreMiss::Count: break;
    }
    return "unknown";
}

fs::path ManifestPath()
{
    const fs::path directory = GetCacheRootPath() / L"shaders";
    const fs::path path = directory / L"compiled_bundles.txt";
    // The old cache kept the manifest in shaders/dxil. Its lines join the new one on first use, before anything appends to
    // it (the offline preprocessor replays them); a root can hold both when an older build ran after a newer one. Only
    // once they are in does RetireOldCache delete the old file.
    static std::once_flag moved;
    std::call_once(moved, [&] {
        std::error_code ec;
        fs::create_directories(directory, ec);
        std::string oldLines;
        if (!ReadWholeFile(OldManifestPath(), oldLines) || oldLines.empty()) {
            g_oldManifestMerged = true;
            return;
        }
        std::string current;
        ReadWholeFile(path, current);
        std::unordered_set<std::string_view> present;
        for (size_t start = 0; start < current.size();) {
            const size_t end = std::min(current.find('\n', start), current.size());
            if (end > start) present.insert(std::string_view(current).substr(start, end - start));
            start = end + 1;
        }
        std::string missing;
        size_t added = 0;
        for (size_t start = 0; start < oldLines.size();) {
            const size_t end = std::min(oldLines.find('\n', start), oldLines.size());
            const auto line = std::string_view(oldLines).substr(start, end - start);
            if (!line.empty() && present.insert(line).second) {
                missing.append(line).push_back('\n');
                ++added;
            }
            start = end + 1;
        }
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!current.empty() && current.back() != '\n') out << '\n';
        out.write(missing.data(), static_cast<std::streamsize>(missing.size()));
        out.close();
        if (out) {
            g_oldManifestMerged = true;
            spdlog::info("Shader store: moved {} lines of the old compile manifest into {}", added, path.string());
        }
    });
    return path;
}

namespace {
std::mutex g_generatedMutex;
std::unordered_map<std::string, std::shared_ptr<const std::string>> g_generated;
}

void SetGeneratedSource(const std::string& name, std::string text)
{
    auto shared = std::make_shared<const std::string>(std::move(text));
    std::scoped_lock lock(g_generatedMutex);
    g_generated[name] = std::move(shared);
}

std::shared_ptr<const std::string> GeneratedSource(std::string_view name)
{
    std::scoped_lock lock(g_generatedMutex);
    const auto found = g_generated.find(std::string(name));
    return found == g_generated.end() ? nullptr : found->second;
}

} // namespace br::shaderstore
