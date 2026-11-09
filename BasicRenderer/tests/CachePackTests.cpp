#include <BasicRenderer/Assets/CachePack.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <windows.h>

// A process opens the pack once and holds it mapped, which is itself part of what is tested.
// So every step runs in its own process and the parent never reads the pack.
namespace {
namespace fs = std::filesystem;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++g_failures;
    }
}

void WriteFile(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

bool Holds(const fs::path& path, std::string_view text) {
    CachePack::Entry entry{};
    return CachePack::Find(path, entry) && entry.size == text.size() &&
        std::memcmp(entry.data, text.data(), text.size()) == 0;
}

// A CLod entry the build judges current: real metadata, under the name the current settings give its key.
CLodCache::CacheLookup MeshLookup() {
    const CLodCache::CacheKey key{ "cache_pack_test.source", "/mesh", "" };
    return CLodCache::BuildCacheLookup(key, CLodCache::ComputeBuildConfigHash(key.sourceIdentifier));
}

void RunStep(const wchar_t* step) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + step;
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    DWORD exitCode = 1;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    if (exitCode != 0) {
        std::fprintf(stderr, "FAILED: step %ls\n", step);
        ++g_failures;
    }
}
}

int main(int argc, char** argv) {
    const fs::path root = fs::temp_directory_path() / L"sarp_cache_pack_tests";
    const fs::path clod = root / L"clod";
    const std::string_view step = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
    if (step.empty()) {
        std::error_code ec;
        fs::remove_all(root, ec);
        _putenv_s("SARP_CACHE_ROOT", root.string().c_str());
    }
    const CLodCache::CacheLookup lookup = MeshLookup();
    const fs::path meta = lookup.metadataPath;
    const fs::path mesh = lookup.containerPath;
    const fs::path oldMeta = meta.parent_path() / L"older_build.clodmeta";
    const fs::path oldMesh = meta.parent_path() / L"older_build.clodbin";
    const fs::path nif = clod / L"nif" / L"tree.nifmeta";
    const fs::path nif_b = clod / L"nif" / L"rock.nifmeta";
    const fs::path changes = fs::path(clod / L"world.sarppack.changes");
    const fs::path pack = clod / L"world.sarppack";
    const fs::path metaCopy = root / L"metadata.copy";
    std::string summary;
    CachePack::Entry entry{};
    CachePack::BuildOptions removeLoose;
    removeLoose.removeLooseFiles = true;

    if (step.empty()) {
        std::error_code ec;
        CLodCache::CacheData data;
        data.buildConfigHash = lookup.buildConfigHash;
        Check(CLodCache::Save(lookup.key, data), "saves a CLod entry");
        Check(CLodCache::IsCurrentMetadataFile(meta), "the saved entry is current");
        WriteFile(mesh, "container");
        fs::copy_file(meta, metaCopy, ec);
        // An older build: valid metadata under a name today's settings don't give it.
        fs::copy_file(meta, oldMeta, ec);
        WriteFile(oldMesh, "old container");
        WriteFile(nif, "nif data");
        WriteFile(clod / L"manifest.tsv", "not an artifact");
        RunStep(L"build");
        RunStep(L"lookups");
        RunStep(L"note-change");
        RunStep(L"change-persists");
        RunStep(L"failed-replace");
        RunStep(L"change-persists");
        WriteFile(mesh, "container v2");
        RunStep(L"rebuild");
        RunStep(L"after-rebuild");
        // Builds after the first update the pack where it lies.
        RunStep(L"unchanged");
        WriteFile(nif_b, "second nif");
        RunStep(L"rewrite-nif");
        RunStep(L"update");
        RunStep(L"after-update");
        RunStep(L"remove-loose");
        Check(!fs::exists(meta) && !fs::exists(mesh) && !fs::exists(nif) && !fs::exists(oldMeta) && !fs::exists(oldMesh),
            "removing loose files deletes packed ones and older builds");
        Check(fs::exists(clod / L"manifest.tsv"), "removing loose files keeps other files");
        RunStep(L"after-remove-loose");
        RunStep(L"unchanged");
        RunStep(L"full-rewrite");
        RunStep(L"after-remove-loose");
        WriteFile(nif_b, "second nif v2");
        RunStep(L"update");
        RunStep(L"after-loose-replaces");
        RunStep(L"note-deletion");
        RunStep(L"update");
        RunStep(L"after-deletion");
        fs::remove_all(root, ec);
        if (g_failures == 0) {
            std::puts("CachePackTests: all passed");
        }
    } else if (step == "build") {
        Check(CachePack::Build(summary), "build");
    } else if (step == "lookups") {
        Check(Holds(mesh, "container"), "finds a container");
        Check(Holds(meta, ReadFile(metaCopy)), "finds metadata");
        Check(Holds(nif, "nif data"), "finds NIF metadata");
        std::wstring differentSpelling = mesh.wstring();
        for (auto& ch : differentSpelling) {
            ch = ch == L'\\' ? L'/' : static_cast<wchar_t>(::towupper(ch));
        }
        Check(CachePack::Find(differentSpelling, entry), "ignores case and slash direction, as the filesystem does");
        Check(!CachePack::Find(clod / L"manifest.tsv", entry), "does not pack other files");
        Check(!CachePack::Find(oldMeta, entry) && !CachePack::Find(oldMesh, entry), "leaves out an older build");
        Check(!CachePack::Find(nif_b, entry), "misses an unknown file");
        Check(!CachePack::Find(fs::temp_directory_path() / L"elsewhere" / L"clod" / L"nif" / L"tree.nifmeta", entry), "misses a path outside the cache");
    } else if (step == "note-change") {
        Check(Holds(mesh, "container"), "served before the change");
        CachePack::NoteFileChanged(mesh);
        Check(!CachePack::Find(mesh, entry), "a rewritten file leaves the pack at once");
        Check(Holds(nif, "nif data"), "other files are unaffected");
    } else if (step == "change-persists") {
        Check(!CachePack::Find(mesh, entry), "a later process ignores the stale entry");
        Check(Holds(nif, "nif data"), "a later process still serves other files");
    } else if (step == "failed-replace") {
        // A running renderer holds the pack mapped, so the new one cannot land and the old one stays in use.
        Check(Holds(nif, "nif data"), "the pack is mapped");
        Check(!CachePack::Build(summary, true), "replacing a mapped pack fails");
        Check(fs::exists(changes), "a failed build keeps the record of stale entries");
    } else if (step == "rebuild") {
        Check(CachePack::Build(summary), "a rebuild succeeds");
        Check(!fs::exists(changes), "a rebuild forgets stale entries");
    } else if (step == "after-rebuild") {
        Check(Holds(mesh, "container v2"), "a rebuild picks up the rewritten file");
    } else if (step == "unchanged") {
        const auto before = fs::last_write_time(pack);
        Check(CachePack::Build(summary), "a build with nothing changed succeeds");
        Check(summary.find("up to date") != std::string::npos, "a build with nothing changed says so");
        Check(fs::last_write_time(pack) == before, "a build with nothing changed writes nothing");
    } else if (step == "rewrite-nif") {
        // The same size, so only the noted change tells the build it's stale.
        CachePack::NoteFileChanged(nif);
        WriteFile(nif, "NIF DATA");
    } else if (step == "update") {
        Check(CachePack::Build(summary), "an update succeeds");
        Check(summary.find("in place") != std::string::npos, "a small change updates the pack in place");
        Check(!fs::exists(changes), "an update forgets stale entries");
    } else if (step == "after-update") {
        Check(Holds(nif_b, "second nif"), "an update adds a new file");
        Check(Holds(nif, "NIF DATA"), "an update replaces a rewritten file of the same size");
        Check(Holds(mesh, "container v2"), "an update keeps the files it didn't touch");
        Check(Holds(meta, ReadFile(metaCopy)), "an update keeps every other file");
    } else if (step == "remove-loose") {
        Check(CachePack::Build(summary, removeLoose), "a build that removes loose files succeeds");
    } else if (step == "after-remove-loose") {
        Check(Holds(mesh, "container v2") && Holds(meta, ReadFile(metaCopy)) && Holds(nif, "NIF DATA") && Holds(nif_b, "second nif"),
            "the pack serves every entry with no loose file");
    } else if (step == "full-rewrite") {
        Check(CachePack::Build(summary, true), "a full rewrite succeeds");
        Check(summary.find("in place") == std::string::npos, "a full rewrite rewrites");
        Check(summary.find("4 carried over") != std::string::npos, "a full rewrite carries over entries with no loose file");
    } else if (step == "after-loose-replaces") {
        Check(Holds(nif_b, "second nif v2"), "a new loose file replaces its packed entry");
        Check(Holds(nif, "NIF DATA"), "entries with no loose file stay");
    } else if (step == "note-deletion") {
        // A deleted cache file with no loose copy: only the note says it's gone.
        CachePack::NoteFileChanged(nif);
    } else if (step == "after-deletion") {
        Check(!CachePack::Find(nif, entry), "a noted deletion leaves the pack");
        Check(Holds(mesh, "container v2") && Holds(meta, ReadFile(metaCopy)), "the rest stay");
    } else {
        Check(false, "unknown step");
    }
    return g_failures;
}
