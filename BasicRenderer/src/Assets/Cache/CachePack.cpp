#include <BasicRenderer/Assets/CachePack.h>

#include <BasicRenderer/Assets/CachePaths.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include <windows.h>

namespace CachePack {
	namespace {
		constexpr std::uint64_t kMagic = 0x314B4341'50524153ull; // "SARPACK1"
		constexpr std::uint32_t kVersion = 1u;
		constexpr std::uint64_t kAlignment = 64u;

		struct Header {
			std::uint64_t magic = kMagic;
			std::uint32_t version = kVersion;
			std::uint32_t reserved = 0u;
			std::uint64_t entryCount = 0u;
			std::uint64_t indexOffset = 0u;
		};

		// Sorted by key. A key hashes the path relative to cache/clod, so the pack survives moving the cache root.
		struct IndexEntry {
			std::uint64_t key = 0u;
			std::uint64_t offset = 0u;
			std::uint64_t size = 0u;
		};

		std::filesystem::path PackRoot() {
			return GetCacheRootPath() / L"clod";
		}

		std::filesystem::path PackPath() {
			return PackRoot() / L"world.sarppack";
		}

		std::filesystem::path ChangesPath() {
			return PackRoot() / L"world.sarppack.changes";
		}

		// False for a path outside cache/clod. Uses the path's text only: this runs for every lookup.
		bool KeyForPath(const std::filesystem::path& cachePath, std::uint64_t& outKey) {
			static const std::wstring root = [] {
				std::wstring text = PackRoot().wstring();
				std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
					return ch == L'/' ? L'\\' : static_cast<wchar_t>(::towlower(ch));
				});
				while (!text.empty() && text.back() == L'\\') {
					text.pop_back();
				}
				return text;
			}();
			const std::wstring& text = cachePath.native();
			if (text.size() <= root.size() + 1u) {
				return false;
			}
			const auto normalize = [](wchar_t ch) {
				return ch == L'/' ? L'\\' : static_cast<wchar_t>(::towlower(ch));
			};
			for (std::size_t i = 0u; i < root.size(); ++i) {
				if (normalize(text[i]) != root[i]) {
					return false;
				}
			}
			if (normalize(text[root.size()]) != L'\\') {
				return false;
			}
			std::uint64_t hash = 14695981039346656037ull;
			for (std::size_t i = root.size() + 1u; i < text.size(); ++i) {
				hash ^= static_cast<std::uint64_t>(normalize(text[i]));
				hash *= 1099511628211ull;
			}
			outKey = hash;
			return true;
		}

		std::atomic<bool> g_readsDisabled{ false };

		struct OpenPack {
			const std::byte* view = nullptr;
			std::uint64_t fileSize = 0u;
			std::span<const IndexEntry> index;
			std::wstring path;
			std::shared_mutex changesMutex;
			std::unordered_set<std::uint64_t> changes;
			std::atomic<bool> anyChanges{ false };

			OpenPack() {
				path = PackPath().wstring();
				char* value = nullptr;
				std::size_t length = 0u;
				const bool off = _dupenv_s(&value, &length, "SARP_CACHE_PACK") == 0 && value && value[0] == '0';
				std::free(value);
				if (off || g_readsDisabled.load(std::memory_order_acquire)) {
					return;
				}
				// Deliberately never closed: entries hand out pointers into the view for the process lifetime.
				HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
				if (file == INVALID_HANDLE_VALUE) {
					spdlog::info("Cache pack: none at '{}'; using loose cache files", PackPath().string());
					return;
				}
				LARGE_INTEGER size{};
				if (!GetFileSizeEx(file, &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(Header))) {
					CloseHandle(file);
					return;
				}
				HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
				const void* mapped = mapping != nullptr ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
				if (mapped == nullptr) {
					spdlog::warn("Cache pack: failed to map '{}'; using loose cache files", PackPath().string());
					if (mapping != nullptr) {
						CloseHandle(mapping);
					}
					CloseHandle(file);
					return;
				}
				const auto* bytes = static_cast<const std::byte*>(mapped);
				const auto fileBytes = static_cast<std::uint64_t>(size.QuadPart);
				Header header{};
				std::memcpy(&header, bytes, sizeof(header));
				const std::uint64_t indexBytes = header.entryCount * sizeof(IndexEntry);
				if (header.magic != kMagic || header.version != kVersion || header.indexOffset > fileBytes ||
					header.entryCount > fileBytes / sizeof(IndexEntry) || indexBytes > fileBytes - header.indexOffset) {
					spdlog::warn("Cache pack: rejecting '{}': wrong format or truncated. Run the preprocessor to rebuild it.", PackPath().string());
					UnmapViewOfFile(mapped);
					CloseHandle(mapping);
					CloseHandle(file);
					return;
				}
				view = bytes;
				fileSize = fileBytes;
				index = { reinterpret_cast<const IndexEntry*>(bytes + header.indexOffset), static_cast<std::size_t>(header.entryCount) };

				std::ifstream changesFile(ChangesPath());
				std::string line;
				while (std::getline(changesFile, line)) {
					if (!line.empty()) {
						changes.insert(std::strtoull(line.c_str(), nullptr, 16));
					}
				}
				anyChanges.store(!changes.empty(), std::memory_order_release);
				spdlog::info("Cache pack: '{}' holds {} files, {} rewritten since it was built", PackPath().string(), index.size(), changes.size());
			}
		};

		OpenPack& Pack() {
			static OpenPack pack;
			return pack;
		}
	}

	bool Find(const std::filesystem::path& cachePath, Entry& outEntry) {
		OpenPack& pack = Pack();
		std::uint64_t key = 0u;
		if (pack.view == nullptr || !KeyForPath(cachePath, key)) {
			return false;
		}
		const auto it = std::lower_bound(pack.index.begin(), pack.index.end(), key,
			[](const IndexEntry& entry, std::uint64_t value) { return entry.key < value; });
		if (it == pack.index.end() || it->key != key || it->offset > pack.fileSize || it->size > pack.fileSize - it->offset) {
			return false;
		}
		if (pack.anyChanges.load(std::memory_order_acquire)) {
			std::shared_lock lock(pack.changesMutex);
			if (pack.changes.contains(key)) {
				return false;
			}
		}
		outEntry = { pack.view + it->offset, it->offset, it->size };
		return true;
	}

	const std::wstring& PackFilePath() {
		return Pack().path;
	}

	void DisableReadsForThisProcess() {
		g_readsDisabled.store(true, std::memory_order_release);
	}

	void NoteFileChanged(const std::filesystem::path& cachePath) {
		std::uint64_t key = 0u;
		if (!KeyForPath(cachePath, key)) {
			return;
		}
		OpenPack& pack = Pack();
		static std::mutex fileMutex;
		{
			std::unique_lock lock(pack.changesMutex);
			if (!pack.changes.insert(key).second) {
				return;
			}
			pack.anyChanges.store(true, std::memory_order_release);
		}
		// Recorded even when reads are off or no pack exists yet: a later process must not trust a pack built before this.
		std::error_code ec;
		if (!std::filesystem::exists(PackPath(), ec)) {
			return;
		}
		std::lock_guard lock(fileMutex);
		std::ofstream changesFile(ChangesPath(), std::ios::app);
		changesFile << std::hex << key << '\n';
		if (!changesFile.good()) {
			spdlog::warn("Cache pack: failed to record a rewritten cache file in '{}'", ChangesPath().string());
		}
	}

	bool Build(std::string& outSummary) {
		ZoneScopedN("CachePack::Build");
		const std::filesystem::path root = PackRoot();
		struct Source {
			std::filesystem::path path;
			std::uint64_t key = 0u;
			std::uint64_t size = 0u;
		};
		std::vector<Source> sources;
		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
			!ec && it != end; it.increment(ec)) {
			if (!it->is_regular_file(ec)) {
				continue;
			}
			const auto extension = it->path().extension();
			if (extension != L".clodbin" && extension != L".clodmeta" && extension != L".nifmeta") {
				continue;
			}
			Source source{};
			source.path = it->path();
			source.size = it->file_size(ec);
			if (ec || !KeyForPath(source.path, source.key)) {
				outSummary = "cannot index '" + source.path.string() + "'";
				return false;
			}
			sources.push_back(std::move(source));
		}
		if (ec) {
			outSummary = "cannot walk '" + root.string() + "': " + ec.message();
			return false;
		}
		// A stable order keeps one mesh's files together, so neighbouring reads stay near each other on disk.
		std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.path < b.path; });

		const std::filesystem::path tempPath = PackPath().wstring() + L".tmp";
		std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) {
			outSummary = "cannot create '" + tempPath.string() + "'";
			return false;
		}
		Header header{};
		out.write(reinterpret_cast<const char*>(&header), sizeof(header));
		std::uint64_t cursor = sizeof(header);
		std::vector<IndexEntry> index;
		index.reserve(sources.size());
		std::vector<char> buffer(4u * 1024u * 1024u);
		const auto fail = [&](std::string message) {
			out.close();
			std::error_code removeError;
			std::filesystem::remove(tempPath, removeError);
			outSummary = std::move(message);
			return false;
		};
		for (const Source& source : sources) {
			const std::uint64_t aligned = (cursor + kAlignment - 1u) & ~(kAlignment - 1u);
			static const char zeros[kAlignment] = {};
			out.write(zeros, static_cast<std::streamsize>(aligned - cursor));
			cursor = aligned;
			std::ifstream in(source.path, std::ios::binary);
			if (!in.is_open()) {
				return fail("cannot read '" + source.path.string() + "'");
			}
			std::uint64_t copied = 0u;
			while (copied < source.size) {
				const auto chunk = static_cast<std::streamsize>((std::min<std::uint64_t>)(buffer.size(), source.size - copied));
				in.read(buffer.data(), chunk);
				if (in.gcount() != chunk) {
					return fail("short read from '" + source.path.string() + "'");
				}
				out.write(buffer.data(), chunk);
				copied += static_cast<std::uint64_t>(chunk);
			}
			index.push_back({ source.key, cursor, source.size });
			cursor += source.size;
		}
		std::sort(index.begin(), index.end(), [](const IndexEntry& a, const IndexEntry& b) { return a.key < b.key; });
		if (std::adjacent_find(index.begin(), index.end(), [](const IndexEntry& a, const IndexEntry& b) { return a.key == b.key; }) != index.end()) {
			return fail("two cache files hash to one key");
		}
		const std::uint64_t indexOffset = (cursor + kAlignment - 1u) & ~(kAlignment - 1u);
		static const char indexPadding[kAlignment] = {};
		out.write(indexPadding, static_cast<std::streamsize>(indexOffset - cursor));
		out.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size() * sizeof(IndexEntry)));
		header.entryCount = index.size();
		header.indexOffset = indexOffset;
		out.seekp(0);
		out.write(reinterpret_cast<const char*>(&header), sizeof(header));
		out.close();
		if (!out.good()) {
			return fail("failed writing '" + tempPath.string() + "'");
		}
		// Forget the changes only once the new pack is in place: a failed replace leaves the old pack, still stale.
		std::filesystem::rename(tempPath, PackPath(), ec);
		if (ec) {
			return fail("cannot replace '" + PackPath().string() + "': " + ec.message() + " (is the renderer running?)");
		}
		std::filesystem::remove(ChangesPath(), ec);
		std::ostringstream summary;
		summary << index.size() << " files, " << (indexOffset >> 20u) << " MiB, at '" << PackPath().string() << "'";
		outSummary = summary.str();
		return true;
	}
}
