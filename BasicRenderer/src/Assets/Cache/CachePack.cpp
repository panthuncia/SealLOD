#include <BasicRenderer/Assets/CachePack.h>

#include <BasicRenderer/Assets/CachePaths.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include <windows.h>

namespace CachePack {
	namespace {
		constexpr std::uint64_t kMagic = 0x314B4341'50524153ull; // "SARPACK1"
		constexpr std::uint32_t kVersion = 2u; // 2: entry names, so a build can carry over entries whose loose files are gone.
		constexpr std::uint64_t kAlignment = 64u;

		struct Header {
			std::uint64_t magic = kMagic;
			std::uint32_t version = kVersion;
			std::uint32_t reserved = 0u;
			std::uint64_t entryCount = 0u;
			std::uint64_t indexOffset = 0u;
			std::uint64_t namesOffset = 0u; // Each entry's path relative to cache/clod, in index order: a u16 length, then UTF-16.
			std::uint64_t namesBytes = 0u;
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
			std::span<const std::byte> names; // Parallel to index (Header::namesOffset).
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
				if (header.namesOffset <= fileBytes && header.namesBytes <= fileBytes - header.namesOffset) {
					names = { bytes + header.namesOffset, static_cast<std::size_t>(header.namesBytes) };
				}

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

	void ForEachEntryUnder(const std::filesystem::path& directory, const std::function<void(const std::filesystem::path&, const Entry&)>& visit) {
		OpenPack& pack = Pack();
		if (pack.view == nullptr || pack.names.empty()) {
			return;
		}
		const auto normalize = [](wchar_t ch) { return ch == L'/' ? L'\\' : static_cast<wchar_t>(::towlower(ch)); };
		// The directory relative to cache/clod, as the names hold paths.
		std::wstring root = PackRoot().wstring(), prefix = directory.wstring();
		std::transform(root.begin(), root.end(), root.begin(), normalize);
		std::transform(prefix.begin(), prefix.end(), prefix.begin(), normalize);
		while (!root.empty() && root.back() == L'\\') root.pop_back();
		if (prefix.size() <= root.size() || !prefix.starts_with(root) || prefix[root.size()] != L'\\') {
			return;
		}
		prefix = prefix.substr(root.size() + 1u);
		while (!prefix.empty() && prefix.back() == L'\\') prefix.pop_back();
		prefix.push_back(L'\\');

		std::size_t at = 0u;
		std::wstring name;
		for (const IndexEntry& entry : pack.index) {
			std::uint16_t length = 0u;
			if (pack.names.size() - at < sizeof(length)) return;
			std::memcpy(&length, pack.names.data() + at, sizeof(length));
			at += sizeof(length);
			if ((pack.names.size() - at) / sizeof(wchar_t) < length) return;
			name.resize(length);
			std::memcpy(name.data(), pack.names.data() + at, length * sizeof(wchar_t));
			at += length * sizeof(wchar_t);
			if (name.size() <= prefix.size()) continue;
			bool under = true;
			for (std::size_t i = 0; i < prefix.size() && under; ++i) under = normalize(name[i]) == prefix[i];
			if (!under || entry.offset > pack.fileSize || entry.size > pack.fileSize - entry.offset) continue;
			if (pack.anyChanges.load(std::memory_order_acquire)) {
				std::shared_lock lock(pack.changesMutex);
				if (pack.changes.contains(entry.key)) continue;
			}
			visit(PackRoot() / name, Entry{ pack.view + entry.offset, entry.offset, entry.size });
		}
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

	namespace {
		constexpr std::uint64_t kLoose = ~0ull;

		// A file to pack: a loose file under cache/clod, or an entry of the current pack whose loose file is gone.
		struct Source {
			std::filesystem::path path; // Where the loose file is or was.
			std::uint64_t key = 0u;
			std::uint64_t size = 0u;
			std::int64_t writeTime = 0;           // Loose files only.
			std::uint64_t packOffset = kLoose;    // In the current pack, for an entry with no loose file.
			bool Packed() const { return packOffset != kLoose; }
		};

		std::filesystem::path VerdictsPath() {
			return PackRoot() / L"world.sarppack.verdicts";
		}

		// Whether a metadata file was current, as of its size and write time, under one settings fingerprint. Judging a
		// file reads it, and most are older builds no pack holds: on a cold file cache that was most of a pack build.
		struct Verdict {
			std::uint64_t key = 0u;
			std::uint64_t size = 0u;
			std::int64_t writeTime = 0;
			std::uint64_t live = 0u;
		};
		constexpr std::uint64_t kVerdictsMagic = 0x31445256'50524153ull; // "SARPVRD1"

		std::vector<Verdict> ReadVerdicts(std::uint64_t fingerprint) {
			std::ifstream file(VerdictsPath(), std::ios::binary);
			std::uint64_t header[3]{};
			if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kVerdictsMagic || header[1] != fingerprint ||
				header[2] > (1ull << 28u)) {
				return {};
			}
			std::vector<Verdict> verdicts(static_cast<std::size_t>(header[2]));
			if (!file.read(reinterpret_cast<char*>(verdicts.data()), static_cast<std::streamsize>(verdicts.size() * sizeof(Verdict)))) {
				return {};
			}
			return verdicts;
		}

		// Best effort: without the file the next build judges every metadata file again.
		void WriteVerdicts(std::uint64_t fingerprint, std::vector<Verdict> verdicts) {
			std::sort(verdicts.begin(), verdicts.end(), [](const Verdict& a, const Verdict& b) { return a.key < b.key; });
			const std::filesystem::path temp = VerdictsPath().wstring() + L".tmp";
			{
				std::ofstream file(temp, std::ios::binary | std::ios::trunc);
				const std::uint64_t header[3]{ kVerdictsMagic, fingerprint, verdicts.size() };
				file.write(reinterpret_cast<const char*>(header), sizeof(header));
				file.write(reinterpret_cast<const char*>(verdicts.data()), static_cast<std::streamsize>(verdicts.size() * sizeof(Verdict)));
				if (!file.good()) return;
			}
			std::error_code ec;
			std::filesystem::rename(temp, VerdictsPath(), ec);
		}

		// A file copied into the pack at `offset`.
		struct Placement {
			const Source* source = nullptr;
			std::uint64_t offset = 0u;
		};

		std::uint64_t AlignUp(std::uint64_t value) {
			return (value + kAlignment - 1u) & ~(kAlignment - 1u);
		}

		constexpr std::uint64_t kSector = 4096u;
		constexpr std::uint64_t kBlockBytes = 16ull << 20u;

		std::uint64_t AlignSector(std::uint64_t value) {
			return (value + kSector - 1u) / kSector * kSector;
		}

		unsigned CopyThreads() {
			return std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
		}

		// Runs fn(i) for i in [0, count) on CopyThreads() threads; stops early once fn returns false.
		template <typename Fn> bool ParallelForEach(std::size_t count, Fn&& fn) {
			std::atomic<std::size_t> next{ 0u };
			std::atomic<bool> ok{ true };
			const auto worker = [&] {
				for (std::size_t i = next++; i < count && ok.load(std::memory_order_relaxed); i = next++) {
					if (!fn(i)) ok.store(false, std::memory_order_relaxed);
				}
			};
			std::vector<std::thread> threads;
			const unsigned threadCount = static_cast<unsigned>((std::min<std::size_t>)(CopyThreads(), (std::max<std::size_t>)(count, 1u)));
			for (unsigned t = 1u; t < threadCount; ++t) threads.emplace_back(worker);
			worker();
			for (auto& thread : threads) thread.join();
			return ok.load();
		}

		std::unordered_set<std::uint64_t> ReadChanges() {
			std::unordered_set<std::uint64_t> changes;
			std::ifstream changesFile(ChangesPath());
			std::string line;
			while (std::getline(changesFile, line)) {
				if (!line.empty()) {
					changes.insert(std::strtoull(line.c_str(), nullptr, 16));
				}
			}
			return changes;
		}

		bool ReadAt(HANDLE file, std::uint64_t offset, void* destination, std::uint64_t bytes) {
			auto* cursor = static_cast<char*>(destination);
			while (bytes != 0u) {
				OVERLAPPED at{};
				at.Offset = static_cast<DWORD>(offset);
				at.OffsetHigh = static_cast<DWORD>(offset >> 32u);
				const DWORD chunk = static_cast<DWORD>((std::min<std::uint64_t>)(bytes, 1u << 30u));
				DWORD read = 0;
				if (!ReadFile(file, cursor, chunk, &read, &at) || read != chunk) {
					return false;
				}
				cursor += chunk;
				offset += chunk;
				bytes -= chunk;
			}
			return true;
		}

		bool WriteAt(HANDLE file, std::uint64_t offset, const void* source, std::uint64_t bytes) {
			const auto* cursor = static_cast<const char*>(source);
			while (bytes != 0u) {
				OVERLAPPED at{};
				at.Offset = static_cast<DWORD>(offset);
				at.OffsetHigh = static_cast<DWORD>(offset >> 32u);
				const DWORD chunk = static_cast<DWORD>((std::min<std::uint64_t>)(bytes, 1u << 30u));
				DWORD written = 0;
				if (!WriteFile(file, cursor, chunk, &written, &at) || written != chunk) {
					return false;
				}
				cursor += chunk;
				offset += chunk;
				bytes -= chunk;
			}
			return true;
		}

		bool SetSize(HANDLE file, std::uint64_t bytes) {
			LARGE_INTEGER end{};
			end.QuadPart = static_cast<LONGLONG>(bytes);
			return SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
		}

		struct HandleCloser {
			HANDLE handle;
			~HandleCloser() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
		};

		// The path a pack entry is named by: relative to cache/clod.
		std::wstring RelativeName(const std::filesystem::path& path) {
			const std::wstring& text = path.native();
			const std::wstring root = PackRoot().native();
			std::size_t begin = root.size();
			while (begin < text.size() && (text[begin] == L'\\' || text[begin] == L'/')) ++begin;
			return text.substr(begin);
		}

		// The current pack's index and names, read when it is a pack this build can carry entries over from.
		struct CurrentPack {
			Header header{};
			std::uint64_t fileBytes = 0u;
			std::vector<IndexEntry> index;
			std::vector<std::wstring> names; // Parallel to index.
		};

		bool ParseNames(std::span<const std::byte> bytes, std::size_t count, std::vector<std::wstring>& outNames) {
			outNames.clear();
			outNames.reserve(count);
			std::size_t at = 0u;
			for (std::size_t i = 0; i < count; ++i) {
				std::uint16_t length = 0u;
				if (bytes.size() - at < sizeof(length)) return false;
				std::memcpy(&length, bytes.data() + at, sizeof(length));
				at += sizeof(length);
				if ((bytes.size() - at) / sizeof(wchar_t) < length) return false;
				std::wstring name(length, L'\0');
				std::memcpy(name.data(), bytes.data() + at, length * sizeof(wchar_t));
				at += length * sizeof(wchar_t);
				outNames.push_back(std::move(name));
			}
			return at == bytes.size();
		}

		std::vector<std::byte> SerializeNames(const std::vector<std::wstring>& names) {
			std::vector<std::byte> bytes;
			for (const std::wstring& name : names) {
				const auto length = static_cast<std::uint16_t>((std::min<std::size_t>)(name.size(), 0xFFFFu));
				const std::size_t at = bytes.size();
				bytes.resize(at + sizeof(length) + length * sizeof(wchar_t));
				std::memcpy(bytes.data() + at, &length, sizeof(length));
				std::memcpy(bytes.data() + at + sizeof(length), name.data(), length * sizeof(wchar_t));
			}
			return bytes;
		}

		std::optional<CurrentPack> ReadCurrentPack() {
			HANDLE file = CreateFileW(PackPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) return std::nullopt;
			HandleCloser closer{ file };
			CurrentPack pack;
			LARGE_INTEGER size{};
			if (!GetFileSizeEx(file, &size) || static_cast<std::uint64_t>(size.QuadPart) < sizeof(Header) || !ReadAt(file, 0u, &pack.header, sizeof(Header))) {
				return std::nullopt;
			}
			pack.fileBytes = static_cast<std::uint64_t>(size.QuadPart);
			const Header& header = pack.header;
			if (header.magic != kMagic || header.version != kVersion || header.indexOffset > pack.fileBytes ||
				header.entryCount > pack.fileBytes / sizeof(IndexEntry) || header.entryCount * sizeof(IndexEntry) > pack.fileBytes - header.indexOffset ||
				header.namesOffset > pack.fileBytes || header.namesBytes > pack.fileBytes - header.namesOffset) {
				return std::nullopt;
			}
			pack.index.resize(static_cast<std::size_t>(header.entryCount));
			std::vector<std::byte> names(static_cast<std::size_t>(header.namesBytes));
			if ((!pack.index.empty() && !ReadAt(file, header.indexOffset, pack.index.data(), pack.index.size() * sizeof(IndexEntry))) ||
				(!names.empty() && !ReadAt(file, header.namesOffset, names.data(), names.size())) || !ParseNames(names, pack.index.size(), pack.names)) {
				return std::nullopt;
			}
			for (const IndexEntry& entry : pack.index) {
				if (entry.offset > pack.fileBytes || entry.size > pack.fileBytes - entry.offset) return std::nullopt;
			}
			return pack;
		}

		// The pack's index and names, sorted by key, for a set of sources at their offsets.
		struct Table {
			std::vector<IndexEntry> index;
			std::vector<std::byte> names;
		};

		bool MakeTable(std::vector<std::pair<IndexEntry, const Source*>> rows, Table& outTable, std::string& outError) {
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first.key < b.first.key; });
			if (std::adjacent_find(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first.key == b.first.key; }) != rows.end()) {
				outError = "two cache files hash to one key";
				return false;
			}
			std::vector<std::wstring> names;
			names.reserve(rows.size());
			outTable.index.clear();
			outTable.index.reserve(rows.size());
			for (const auto& [entry, source] : rows) {
				outTable.index.push_back(entry);
				names.push_back(RelativeName(source->path));
			}
			outTable.names = SerializeNames(names);
			return true;
		}

		// Copies every placement into `packPath` at its offset, in large sector-aligned blocks written without the file
		// cache. Buffered writes (positioned, or through a mapped view) ran at one rate whatever the thread count, and on a
		// fast drive this is as quick while leaving the file cache to the reads. Each block gathers the files it covers
		// into an aligned buffer and goes out in one write; bytes between files are zeros. A block may write zeros past
		// the last file up to the next sector, so callers write their index and header afterwards and set the final size.
		// Placements must not overlap bytes the pack still needs. A source already in the current pack is read from it.
		bool CopyIntoPack(const std::filesystem::path& packPath, std::span<const Placement> placements, std::string& outError) {
			if (placements.empty()) return true;
			std::vector<const Placement*> order;
			order.reserve(placements.size());
			for (const Placement& placement : placements) order.push_back(&placement);
			std::sort(order.begin(), order.end(), [](const Placement* a, const Placement* b) { return a->offset < b->offset; });
			const std::uint64_t begin = order.front()->offset / kSector * kSector;
			std::uint64_t end = 0u;
			for (const Placement* placement : order) end = (std::max)(end, placement->offset + placement->source->size);
			end = AlignSector(end);
			{
				// No read access: an update holds the pack open for writing and shares only writes.
				HANDLE pack = CreateFileW(packPath.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL, nullptr);
				if (pack == INVALID_HANDLE_VALUE) {
					outError = "cannot open '" + packPath.string() + "' for writing";
					return false;
				}
				HandleCloser closer{ pack };
				LARGE_INTEGER size{};
				if (!GetFileSizeEx(pack, &size) || (static_cast<std::uint64_t>(size.QuadPart) < end && !SetSize(pack, end))) {
					outError = "cannot size '" + packPath.string() + "'";
					return false;
				}
			}
			std::mutex errorMutex;
			const auto fail = [&](std::string message) {
				std::lock_guard lock(errorMutex);
				if (outError.empty()) outError = std::move(message);
				return false;
			};
			struct Buffer {
				void* bytes = VirtualAlloc(nullptr, kBlockBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				~Buffer() {
					if (bytes) VirtualFree(bytes, 0, MEM_RELEASE);
				}
			};
			const std::size_t blocks = static_cast<std::size_t>((end - begin + kBlockBytes - 1u) / kBlockBytes);
			return ParallelForEach(blocks, [&](std::size_t b) {
				thread_local Buffer buffer;
				if (buffer.bytes == nullptr) return fail("out of memory for a pack block");
				auto* bytes = static_cast<std::byte*>(buffer.bytes);
				const std::uint64_t blockBegin = begin + b * kBlockBytes;
				const std::uint64_t blockEnd = (std::min)(blockBegin + kBlockBytes, end);
				std::memset(bytes, 0, static_cast<std::size_t>(blockEnd - blockBegin));
				HANDLE current = INVALID_HANDLE_VALUE;
				HandleCloser currentCloser{ INVALID_HANDLE_VALUE };
				// The first placement ending past the block's start: placements don't overlap, so their ends ascend too.
				auto it = std::partition_point(order.begin(), order.end(), [&](const Placement* p) { return p->offset + p->source->size <= blockBegin; });
				for (; it != order.end() && (*it)->offset < blockEnd; ++it) {
					const Placement& placement = **it;
					const Source& source = *placement.source;
					const std::uint64_t from = (std::max)(placement.offset, blockBegin);
					const std::uint64_t to = (std::min)(placement.offset + source.size, blockEnd);
					if (to <= from) continue;
					if (source.Packed()) {
						if (current == INVALID_HANDLE_VALUE) {
							current = CreateFileW(PackPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
								FILE_ATTRIBUTE_NORMAL, nullptr);
							if (current == INVALID_HANDLE_VALUE) return fail("cannot read '" + PackPath().string() + "'");
							currentCloser.handle = current;
						}
						if (!ReadAt(current, source.packOffset + (from - placement.offset), bytes + (from - blockBegin), to - from)) {
							return fail("short read of '" + source.path.string() + "' from the current pack");
						}
						continue;
					}
					HANDLE in = CreateFileW(source.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
					if (in == INVALID_HANDLE_VALUE) return fail("cannot read '" + source.path.string() + "'");
					HandleCloser inCloser{ in };
					if (!ReadAt(in, from - placement.offset, bytes + (from - blockBegin), to - from)) {
						return fail("short read from '" + source.path.string() + "'");
					}
				}
				// A handle per block: one synchronous handle serializes every write made through it.
				HANDLE out = CreateFileW(packPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, nullptr);
				if (out == INVALID_HANDLE_VALUE) return fail("cannot open '" + packPath.string() + "' for writing");
				HandleCloser outCloser{ out };
				if (!WriteAt(out, blockBegin, bytes, blockEnd - blockBegin)) return fail("failed writing a block of '" + packPath.string() + "'");
				return true;
			});
		}

		// The index, then the names, then the header naming them, each reaching the disk before the next.
		bool WriteTable(HANDLE file, const Header& header, const Table& table) {
			return WriteAt(file, header.indexOffset, table.index.data(), table.index.size() * sizeof(IndexEntry)) &&
				WriteAt(file, header.namesOffset, table.names.data(), table.names.size()) && FlushFileBuffers(file) &&
				WriteAt(file, 0u, &header, sizeof(header)) && FlushFileBuffers(file);
		}

		enum class Update { Done, Rebuild, Failed };

		// Brings the current pack up to date where it lies: entries it holds unchanged keep their bytes, new and rewritten
		// files are appended, then a new index and names after them, and the header last. Until the header is written the
		// old one still names the old index, which nothing overwrites, so an interrupted update leaves the old pack, and the
		// changes file keeps its stale entries out of use either way. Rebuild when there's no usable pack, or when bytes
		// nothing indexes would pass a quarter of it (and 64 MiB, so a small pack never rewrites over a few files).
		Update UpdateInPlace(const CurrentPack& current, const std::vector<Source>& sources, const std::unordered_set<std::uint64_t>& changes,
			std::string& outSummary) {
			HANDLE file = CreateFileW(PackPath().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				const DWORD error = GetLastError();
				if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED) {
					outSummary = "cannot update '" + PackPath().string() + "': it is open elsewhere (is the renderer running?)";
					return Update::Failed;
				}
				return Update::Rebuild;
			}
			HandleCloser closer{ file };
			LARGE_INTEGER size{};
			Header header{};
			if (!GetFileSizeEx(file, &size) || static_cast<std::uint64_t>(size.QuadPart) != current.fileBytes || !ReadAt(file, 0u, &header, sizeof(header)) ||
				std::memcmp(&header, &current.header, sizeof(header)) != 0) {
				return Update::Rebuild; // It changed since it was read.
			}
			const std::uint64_t fileBytes = current.fileBytes;
			const auto& old = current.index;

			std::vector<std::pair<IndexEntry, const Source*>> rows;
			rows.reserve(sources.size());
			std::vector<const Source*> appended;
			std::uint64_t liveBytes = 0u;
			std::uint64_t appendBytes = 0u;
			std::size_t replaced = 0u; // Appended over an entry the pack held.
			for (const Source& source : sources) {
				const auto it = std::lower_bound(old.begin(), old.end(), source.key, [](const IndexEntry& entry, std::uint64_t value) { return entry.key < value; });
				const bool held = it != old.end() && it->key == source.key;
				// A loose file the pack holds at its size, with no noted change, is the bytes the pack holds.
				const bool kept = held && (source.Packed() ? source.packOffset == it->offset : it->size == source.size && !changes.contains(source.key));
				if (kept) {
					rows.push_back({ *it, &source });
				} else {
					if (source.Packed()) {
						outSummary = "a carried-over entry is not in the pack";
						return Update::Failed;
					}
					if (held) ++replaced;
					appended.push_back(&source);
					appendBytes = AlignUp(appendBytes) + source.size;
				}
				liveBytes += source.size;
			}
			if (appended.empty() && rows.size() == old.size() && changes.empty()) {
				outSummary = std::to_string(rows.size()) + " files, " + std::to_string(fileBytes >> 20u) + " MiB, already up to date at '" + PackPath().string() + "'";
				return Update::Done;
			}
			// Sector-aligned, so the copy's first block starts past every byte the pack holds.
			const std::uint64_t dataOffset = AlignSector(fileBytes);
			std::vector<Placement> placements;
			placements.reserve(appended.size());
			std::uint64_t cursor = dataOffset;
			for (const Source* source : appended) {
				cursor = AlignUp(cursor);
				placements.push_back({ source, cursor });
				rows.push_back({ IndexEntry{ source->key, cursor, source->size }, source });
				cursor += source->size;
			}
			Table table;
			if (!MakeTable(std::move(rows), table, outSummary)) return Update::Failed;
			Header updated = header;
			updated.entryCount = table.index.size();
			updated.indexOffset = AlignUp(cursor);
			updated.namesOffset = updated.indexOffset + table.index.size() * sizeof(IndexEntry);
			updated.namesBytes = table.names.size();
			const std::uint64_t finalBytes = updated.namesOffset + updated.namesBytes;
			// Old copies, the old index and names, and alignment: bytes nothing reads.
			const std::uint64_t unindexed = finalBytes - liveBytes - table.index.size() * sizeof(IndexEntry) - table.names.size() - sizeof(Header);
			if (unindexed * 4u > finalBytes && unindexed > (64ull << 20u)) {
				return Update::Rebuild;
			}
			std::string copyError;
			if (!CopyIntoPack(PackPath(), placements, copyError)) {
				outSummary = copyError;
				return Update::Failed;
			}
			if (!WriteTable(file, updated, table)) {
				outSummary = "failed writing the pack's index";
				return Update::Failed;
			}
			if (!SetSize(file, finalBytes)) {
				outSummary = "failed trimming the pack";
				return Update::Failed;
			}
			std::error_code ec;
			std::filesystem::remove(ChangesPath(), ec);
			std::ostringstream summary;
			const std::size_t kept = table.index.size() - appended.size();
			summary << table.index.size() << " files, " << (finalBytes >> 20u) << " MiB, updated in place at '" << PackPath().string() << "': " << appended.size()
					<< " appended (" << replaced << " of them rewritten, " << (appendBytes >> 20u) << " MiB), " << (old.size() - kept - replaced) << " removed, "
					<< (unindexed >> 20u) << " MiB unused";
			outSummary = summary.str();
			return Update::Done;
		}

		// The file name up to its first dot: a CLod entry's metadata, container and micromaps share it.
		std::wstring EntryStem(const std::filesystem::path& path) {
			const std::wstring name = path.filename().wstring();
			return name.substr(0, name.find(L'.'));
		}

		// Deletes the loose files a finished pack made redundant, and then the directories left empty. Returns how many
		// it deleted; a file it can't delete stays, and is either packed or not needed.
		std::size_t RemoveLooseFiles(const std::vector<std::filesystem::path>& files) {
			std::atomic<std::size_t> removed{ 0u };
			ParallelForEach(files.size(), [&](std::size_t i) {
				if (DeleteFileW(files[i].c_str())) removed.fetch_add(1u, std::memory_order_relaxed);
				return true;
			});
			std::vector<std::wstring> directories;
			for (const auto& file : files) directories.push_back(file.parent_path().native());
			std::sort(directories.begin(), directories.end());
			directories.erase(std::unique(directories.begin(), directories.end()), directories.end());
			// Deepest first, so a parent is tried after its children. RemoveDirectory refuses one that isn't empty.
			std::sort(directories.begin(), directories.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
			const std::wstring root = PackRoot().native();
			for (const auto& directory : directories) {
				if (directory.size() > root.size()) RemoveDirectoryW(directory.c_str());
			}
			return removed.load();
		}
	}

	bool Build(std::string& outSummary) {
		return Build(outSummary, BuildOptions{});
	}

	bool Build(std::string& outSummary, bool rewrite) {
		BuildOptions options;
		options.rewrite = rewrite;
		return Build(outSummary, options);
	}

	bool Build(std::string& outSummary, const BuildOptions& options) {
		ZoneScopedN("CachePack::Build");
		const auto begin = std::chrono::steady_clock::now();
		const std::filesystem::path root = PackRoot();
		const std::optional<CurrentPack> current = ReadCurrentPack();
		const auto changes = ReadChanges();

		std::vector<Source> candidates;
		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
			!ec && it != end; it.increment(ec)) {
			if (!it->is_regular_file(ec)) {
				continue;
			}
			const auto extension = it->path().extension();
			if (extension != L".clodbin" && extension != L".clodmeta" && extension != L".nifmeta" && extension != L".clodomm") {
				continue;
			}
			Source source{};
			source.path = it->path();
			source.size = it->file_size(ec);
			if (!ec) source.writeTime = static_cast<std::int64_t>(it->last_write_time(ec).time_since_epoch().count());
			if (ec || !KeyForPath(source.path, source.key)) {
				outSummary = "cannot index '" + source.path.string() + "'";
				return false;
			}
			candidates.push_back(std::move(source));
		}
		if (ec) {
			outSummary = "cannot walk '" + root.string() + "': " + ec.message();
			return false;
		}
		std::vector<std::filesystem::path> looseFiles;
		looseFiles.reserve(candidates.size());
		for (const Source& candidate : candidates) looseFiles.push_back(candidate.path);
		// The pack is where most entries live once their loose files are deleted: carry over each one with no loose file,
		// unless a noted change says its file was deleted or rewritten since.
		std::size_t carried = 0u, deleted = 0u;
		if (current) {
			std::unordered_set<std::uint64_t> looseKeys;
			looseKeys.reserve(candidates.size());
			for (const Source& candidate : candidates) looseKeys.insert(candidate.key);
			for (std::size_t i = 0; i < current->index.size(); ++i) {
				const IndexEntry& entry = current->index[i];
				if (looseKeys.contains(entry.key)) continue;
				if (changes.contains(entry.key)) {
					++deleted;
					continue;
				}
				Source source{};
				source.path = root / current->names[i];
				source.key = entry.key;
				source.size = entry.size;
				source.packOffset = entry.offset;
				candidates.push_back(std::move(source));
				++carried;
			}
		}
		const auto walked = std::chrono::steady_clock::now();

		// Only current CLod entries: a schema or settings change leaves every older build beside the new one, and packing
		// those made the pack several times larger than what any load reads. An entry is its metadata file's stem; its
		// container and micromaps go with it. Payload caches (.nifmeta) are kept as they are.
		std::vector<std::uint8_t> live(candidates.size(), 0u);
		std::vector<std::size_t> metadata;
		for (std::size_t i = 0; i < candidates.size(); ++i) {
			if (candidates[i].path.extension() == L".clodmeta") metadata.push_back(i);
			else if (candidates[i].path.extension() == L".nifmeta") live[i] = 1u;
		}
		const std::uint64_t fingerprint = CLodCache::CurrentSettingsFingerprint();
		const std::vector<Verdict> known = ReadVerdicts(fingerprint);
		std::vector<Verdict> verdicts(metadata.size());
		std::atomic<std::size_t> judged{ 0u };
		// Packed metadata is judged from the pack's bytes, through a view of it held for this phase only.
		const std::byte* view = nullptr;
		HANDLE viewFile = INVALID_HANDLE_VALUE, viewMapping = nullptr;
		if (carried != 0u) {
			viewFile = CreateFileW(PackPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			viewMapping = viewFile != INVALID_HANDLE_VALUE ? CreateFileMappingW(viewFile, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
			view = viewMapping != nullptr ? static_cast<const std::byte*>(MapViewOfFile(viewMapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
		}
		ParallelForEach(metadata.size(), [&](std::size_t m) {
			const Source& candidate = candidates[metadata[m]];
			const auto it = std::lower_bound(known.begin(), known.end(), candidate.key, [](const Verdict& v, std::uint64_t key) { return v.key < key; });
			Verdict& verdict = verdicts[m];
			// A packed entry's bytes are the ones judged when it was loose, or when it was first carried over.
			if (it != known.end() && it->key == candidate.key && it->size == candidate.size && (candidate.Packed() || it->writeTime == candidate.writeTime)) {
				verdict = *it;
			} else {
				bool isLive = false;
				if (!candidate.Packed()) {
					isLive = CLodCache::IsCurrentMetadataFile(candidate.path);
				} else if (view != nullptr) {
					isLive = CLodCache::IsCurrentMetadata({ view + candidate.packOffset, static_cast<std::size_t>(candidate.size) }, candidate.path.filename().wstring());
				}
				verdict = { candidate.key, candidate.size, candidate.writeTime, isLive ? 1u : 0u };
				judged.fetch_add(1u, std::memory_order_relaxed);
			}
			live[metadata[m]] = verdict.live != 0u ? 1u : 0u;
			return true;
		});
		if (view != nullptr) UnmapViewOfFile(view);
		if (viewMapping != nullptr) CloseHandle(viewMapping);
		if (viewFile != INVALID_HANDLE_VALUE) CloseHandle(viewFile);
		if (judged.load() != 0u || known.size() != verdicts.size()) {
			WriteVerdicts(fingerprint, std::move(verdicts));
		}
		std::unordered_set<std::wstring> liveEntries;
		for (const std::size_t m : metadata) {
			if (live[m]) liveEntries.insert((candidates[m].path.parent_path() / EntryStem(candidates[m].path)).native());
		}
		std::vector<Source> sources;
		std::uint64_t staleFiles = 0u, staleBytes = 0u;
		for (std::size_t i = 0; i < candidates.size(); ++i) {
			const auto extension = candidates[i].path.extension();
			const bool keep = live[i] != 0u ||
				((extension == L".clodbin" || extension == L".clodomm") &&
					liveEntries.contains((candidates[i].path.parent_path() / EntryStem(candidates[i].path)).native()));
			if (keep) {
				sources.push_back(std::move(candidates[i]));
			} else {
				++staleFiles;
				staleBytes += candidates[i].size;
			}
		}
		// A stable order keeps one mesh's files together, so neighbouring reads stay near each other on disk.
		std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.path < b.path; });
		const auto filtered = std::chrono::steady_clock::now();
		const auto finish = [&](std::string summary) {
			const auto ms = [](auto from, auto to) { return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count(); };
			std::ostringstream text;
			text << summary << "; " << carried << " carried over without a loose file, " << deleted << " deleted; left out " << staleFiles
				 << " files of older CLod builds (" << (staleBytes >> 20u) << " MiB)";
			const auto packed = std::chrono::steady_clock::now();
			if (options.removeLooseFiles && !looseFiles.empty()) {
				const std::size_t removed = RemoveLooseFiles(looseFiles);
				text << "; deleted " << removed << " of " << looseFiles.size() << " loose files";
			}
			const auto now = std::chrono::steady_clock::now();
			text << "; " << ms(begin, now) << " ms (walk " << ms(begin, walked) << ", filter " << ms(walked, filtered) << " judging " << judged.load() << " of "
				 << metadata.size() << " metadata files, copy and index " << ms(filtered, packed) << ", loose file removal " << ms(packed, now) << ")";
			outSummary = text.str();
			return true;
		};

		if (!options.rewrite && current) {
			std::string summary;
			switch (UpdateInPlace(*current, sources, changes, summary)) {
			case Update::Done:
				return finish(std::move(summary));
			case Update::Failed:
				outSummary = std::move(summary);
				return false;
			case Update::Rebuild:
				break;
			}
		}

		const std::filesystem::path tempPath = PackPath().wstring() + L".tmp";
		std::vector<Placement> placements;
		std::vector<std::pair<IndexEntry, const Source*>> rows;
		placements.reserve(sources.size());
		rows.reserve(sources.size());
		std::uint64_t cursor = sizeof(Header);
		for (const Source& source : sources) {
			cursor = AlignUp(cursor);
			placements.push_back({ &source, cursor });
			rows.push_back({ IndexEntry{ source.key, cursor, source.size }, &source });
			cursor += source.size;
		}
		Table table;
		if (!MakeTable(std::move(rows), table, outSummary)) return false;
		Header header{};
		header.entryCount = table.index.size();
		header.indexOffset = AlignUp(cursor);
		header.namesOffset = header.indexOffset + table.index.size() * sizeof(IndexEntry);
		header.namesBytes = table.names.size();
		const std::uint64_t totalBytes = header.namesOffset + header.namesBytes;
		const auto fail = [&](std::string message) {
			std::error_code removeError;
			std::filesystem::remove(tempPath, removeError);
			outSummary = std::move(message);
			return false;
		};
		{
			// Sized up front, so the parallel copies write into place without extending the file.
			HANDLE out = CreateFileW(tempPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (out == INVALID_HANDLE_VALUE) return fail("cannot create '" + tempPath.string() + "'");
			HandleCloser closer{ out };
			if (!SetSize(out, totalBytes)) return fail("cannot size '" + tempPath.string() + "'");
		}
		std::string copyError;
		if (!CopyIntoPack(tempPath, placements, copyError)) return fail(copyError);
		{
			HANDLE out = CreateFileW(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (out == INVALID_HANDLE_VALUE) return fail("cannot reopen '" + tempPath.string() + "'");
			HandleCloser closer{ out };
			// The trim drops any zeros the copy wrote past the index's start.
			if (!WriteTable(out, header, table) || !SetSize(out, totalBytes) || !FlushFileBuffers(out)) {
				return fail("failed writing '" + tempPath.string() + "'");
			}
		}
		// Forget the changes only once the new pack is in place: a failed replace leaves the old pack, still stale.
		std::filesystem::rename(tempPath, PackPath(), ec);
		if (ec) {
			return fail("cannot replace '" + PackPath().string() + "': " + ec.message() + " (is the renderer running?)");
		}
		std::filesystem::remove(ChangesPath(), ec);
		std::ostringstream summary;
		summary << table.index.size() << " files, " << (totalBytes >> 20u) << " MiB, rewritten at '" << PackPath().string() << "'";
		return finish(summary.str());
	}
}
