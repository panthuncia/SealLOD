#include "Assets/Textures/Cache/ProcessedTextureCache.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <BasicRenderer/Assets/CachePaths.h>

namespace br::processed_texture_cache {

namespace {

// Every processed texture's header, in one file. A warm load otherwise opens each of
// thousands of cache files just to read its first bytes, and under a virtual filesystem
// every open waits on one process-wide lock. Cache file names contain their key, so a
// header can only change with the format version, which loading checks.
//
// Entries from earlier runs are immutable, so hits take no lock.
class HeaderIndex {
public:
    static HeaderIndex& Get() {
        static HeaderIndex index;
        return index;
    }

    ~HeaderIndex() { Save(); }

    [[nodiscard]] bool Find(const std::wstring& path, FileHeader& header) const {
        if (!m_enabled) return false;
        if (const auto loaded = m_loaded.find(path); loaded != m_loaded.end()) {
            header = loaded->second;
            return true;
        }
        std::shared_lock lock(m_recordedMutex);
        const auto recorded = m_recorded.find(path);
        if (recorded == m_recorded.end()) return false;
        header = recorded->second;
        return true;
    }

    void Record(const std::wstring& path, const FileHeader& header) {
        if (!m_enabled || m_loaded.contains(path)) return;
        std::unique_lock lock(m_recordedMutex);
        m_recorded.insert_or_assign(path, header);
    }

private:
    HeaderIndex() : m_file(GetCacheFilePath(L"processed_header_index.bin", L"textures")) {
        m_enabled = !EnvironmentIs("SARP_RESOLVE_INDEX", "0");
        if (m_enabled && !EnvironmentIs("SARP_RESOLVE_INDEX_REBUILD", "1")) Load();
    }

    [[nodiscard]] static bool EnvironmentIs(const char* name, const char* expected) {
        char* value = nullptr;
        std::size_t length = 0;
        bool matches = false;
        if (_dupenv_s(&value, &length, name) == 0 && value) matches = std::string(value) == expected;
        std::free(value);
        return matches;
    }

    void Load() {
        std::ifstream in(std::filesystem::path(m_file), std::ios::binary);
        std::uint32_t pathLength = 0;
        std::wstring path;
        FileHeader header{};
        while (in.read(reinterpret_cast<char*>(&pathLength), sizeof(pathLength))) {
            path.resize(pathLength);
            if (!in.read(reinterpret_cast<char*>(path.data()), pathLength * sizeof(wchar_t)) ||
                !in.read(reinterpret_cast<char*>(&header), sizeof(header))) break;
            if (header.magic == kMagic && header.version == kVersion && header.headerSize == sizeof(header)) {
                m_loaded.emplace(path, header);
            }
        }
    }

    void Save() const {
        if (!m_enabled || m_recorded.empty()) return;
        std::filesystem::path temporary(m_file);
        temporary += L".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) return;
            // Sorted by path, so the same entries always produce the same file.
            std::vector<const std::pair<const std::wstring, FileHeader>*> entries;
            entries.reserve(m_loaded.size() + m_recorded.size());
            for (const auto& entry : m_loaded) entries.push_back(&entry);
            for (const auto& entry : m_recorded) entries.push_back(&entry);
            std::ranges::sort(entries, [](const auto* a, const auto* b) { return a->first < b->first; });
            for (const auto* entry : entries) {
                const auto& [path, header] = *entry;
                const auto pathLength = static_cast<std::uint32_t>(path.size());
                out.write(reinterpret_cast<const char*>(&pathLength), sizeof(pathLength));
                out.write(reinterpret_cast<const char*>(path.data()), pathLength * sizeof(wchar_t));
                out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            }
            if (!out) return;
        }
        std::error_code error;
        std::filesystem::rename(temporary, m_file, error);
        if (error) std::filesystem::remove(temporary, error);
    }

    std::wstring m_file;
    bool m_enabled = false;
    std::unordered_map<std::wstring, FileHeader> m_loaded;
    mutable std::shared_mutex m_recordedMutex;
    std::unordered_map<std::wstring, FileHeader> m_recorded;
};

}

bool TryFindIndexedHeader(const std::wstring& path, FileHeader& header) {
    return HeaderIndex::Get().Find(path, header);
}

void RecordIndexedHeader(const std::wstring& path, const FileHeader& header) {
    HeaderIndex::Get().Record(path, header);
}

}
