#include <BasicRenderer/Assets/CachePack.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <windows.h>

// A process opens the pack once and holds it mapped, which is itself part of what is tested.
// So every step runs in its own process and the parent never touches the pack.
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

bool Holds(const fs::path& path, std::string_view text) {
    CachePack::Entry entry{};
    return CachePack::Find(path, entry) && entry.size == text.size() &&
        std::memcmp(entry.data, text.data(), text.size()) == 0;
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
    const fs::path mesh = clod / L"scene" / L"Mesh_A.clodbin";
    const fs::path meta = clod / L"scene" / L"Mesh_A.clodmeta";
    const fs::path nif = clod / L"nif" / L"tree.nifmeta";
    const fs::path mesh_b = clod / L"scene" / L"Mesh_B.clodbin";
    const fs::path changes = fs::path(clod / L"world.sarppack.changes");
    const fs::path pack = clod / L"world.sarppack";
    const std::string_view step = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
    std::string summary;
    CachePack::Entry entry{};

    if (step.empty()) {
        std::error_code ec;
        fs::remove_all(root, ec);
        _putenv_s("SARP_CACHE_ROOT", root.string().c_str());
        WriteFile(mesh, "container");
        WriteFile(meta, "metadata");
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
        WriteFile(mesh_b, "second mesh");
        RunStep(L"rewrite-meta");
        RunStep(L"update");
        RunStep(L"after-update");
        fs::remove(nif, ec);
        RunStep(L"update");
        RunStep(L"after-removal");
        RunStep(L"full-rewrite");
        RunStep(L"after-removal");
        fs::remove_all(root, ec);
        if (g_failures == 0) {
            std::puts("CachePackTests: all passed");
        }
    } else if (step == "build") {
        Check(CachePack::Build(summary), "build");
    } else if (step == "lookups") {
        Check(Holds(mesh, "container"), "finds a container");
        Check(Holds(meta, "metadata"), "finds metadata");
        Check(Holds(nif, "nif data"), "finds NIF metadata");
        std::wstring differentSpelling = mesh.wstring();
        for (auto& ch : differentSpelling) {
            ch = ch == L'\\' ? L'/' : static_cast<wchar_t>(::towupper(ch));
        }
        Check(CachePack::Find(differentSpelling, entry), "ignores case and slash direction, as the filesystem does");
        Check(!CachePack::Find(clod / L"manifest.tsv", entry), "does not pack other files");
        Check(!CachePack::Find(clod / L"scene" / L"Mesh_B.clodbin", entry), "misses an unknown file");
        Check(!CachePack::Find(fs::temp_directory_path() / L"elsewhere" / L"clod" / L"scene" / L"Mesh_A.clodbin", entry), "misses a path outside the cache");
    } else if (step == "note-change") {
        Check(Holds(mesh, "container"), "served before the change");
        CachePack::NoteFileChanged(mesh);
        Check(!CachePack::Find(mesh, entry), "a rewritten file leaves the pack at once");
        Check(Holds(meta, "metadata"), "other files are unaffected");
    } else if (step == "change-persists") {
        Check(!CachePack::Find(mesh, entry), "a later process ignores the stale entry");
        Check(Holds(meta, "metadata"), "a later process still serves other files");
    } else if (step == "failed-replace") {
        // A running renderer holds the pack mapped, so the new one cannot land and the old one stays in use.
        Check(Holds(meta, "metadata"), "the pack is mapped");
        Check(!CachePack::Build(summary), "replacing a mapped pack fails");
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
    } else if (step == "rewrite-meta") {
        // The same size, so only the noted change tells the build it's stale.
        WriteFile(meta, "METADATA");
        CachePack::NoteFileChanged(meta);
    } else if (step == "update") {
        Check(CachePack::Build(summary), "an update succeeds");
        Check(summary.find("in place") != std::string::npos, "a small change updates the pack in place");
        Check(!fs::exists(changes), "an update forgets stale entries");
    } else if (step == "after-update") {
        Check(Holds(mesh_b, "second mesh"), "an update adds a new file");
        Check(Holds(meta, "METADATA"), "an update replaces a rewritten file of the same size");
        Check(Holds(mesh, "container v2"), "an update keeps the files it didn't touch");
        Check(Holds(nif, "nif data"), "an update keeps every other file");
    } else if (step == "after-removal") {
        Check(!CachePack::Find(nif, entry), "a deleted file leaves the pack");
        Check(Holds(mesh, "container v2") && Holds(mesh_b, "second mesh") && Holds(meta, "METADATA"), "the rest stay");
    } else if (step == "full-rewrite") {
        Check(CachePack::Build(summary, true), "a full rewrite succeeds");
        Check(summary.find("in place") == std::string::npos, "a full rewrite rewrites");
    } else {
        Check(false, "unknown step");
    }
    return g_failures;
}
