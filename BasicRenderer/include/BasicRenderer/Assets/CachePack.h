#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// One read-only file holding the per-mesh cache artifacts under cache/clod. A world load
// otherwise opens tens of thousands of small files, whose cost also varies by more than an
// order of magnitude between runs. The loose files stay authoritative for anything the pack
// does not hold, so a missing pack only costs time.
namespace CachePack {

	struct Entry {
		const std::byte* data = nullptr; // In the pack's mapped view, valid for the process lifetime.
		std::uint64_t offset = 0u;       // Within the pack file.
		std::uint64_t size = 0u;
	};

	// False when there is no pack, it does not hold this file, or the file was rewritten after the pack was built.
	bool Find(const std::filesystem::path& cachePath, Entry& outEntry);

	const std::wstring& PackFilePath();

	// A process that rewrites cache files and then rebuilds the pack must not hold it mapped.
	void DisableReadsForThisProcess();

	// Writers call this after changing or deleting a cache file, so a stale entry is never served. Persistent.
	void NoteFileChanged(const std::filesystem::path& cachePath);

	// Packs every artifact under cache/clod, replacing the pack and forgetting noted changes.
	bool Build(std::string& outSummary);
}
