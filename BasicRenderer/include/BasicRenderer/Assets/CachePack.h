#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// One read-only file holding the per-mesh cache artifacts under cache/clod. A world load
// otherwise opens tens of thousands of small files, whose cost also varies by more than an
// order of magnitude between runs. A loose file the pack holds is usually deleted once packed
// (BuildOptions::removeLooseFiles), so the pack is then the only copy; a loose file it does not
// hold, or one written since and noted (NoteFileChanged), takes precedence.
namespace CachePack {

	struct Entry {
		const std::byte* data = nullptr; // In the pack's mapped view, valid for the process lifetime.
		std::uint64_t offset = 0u;       // Within the pack file.
		std::uint64_t size = 0u;
	};

	// False when there is no pack, it does not hold this file, or the file was rewritten after the pack was built.
	bool Find(const std::filesystem::path& cachePath, Entry& outEntry);

	// Every file the pack serves under a directory of cache/clod (as Find would serve it), with its full path.
	void ForEachEntryUnder(const std::filesystem::path& directory, const std::function<void(const std::filesystem::path&, const Entry&)>& visit);

	const std::wstring& PackFilePath();

	// A process that rewrites cache files and then rebuilds the pack must not hold it mapped.
	void DisableReadsForThisProcess();

	// Writers call this before changing or deleting a cache file, so a stale entry is never served, and the next build
	// drops an entry whose file was deleted. Persistent.
	void NoteFileChanged(const std::filesystem::path& cachePath);

	struct BuildOptions {
		bool rewrite = false;          // Rewrite the pack whole, in an order that keeps each mesh's files together.
		bool removeLooseFiles = false; // Delete every loose artifact once the pack is in place: packed ones and older builds.
	};

	// Packs every current artifact under cache/clod, with the entries of the existing pack whose loose files are gone
	// (unless a noted change says they were deleted), and forgets noted changes. Older CLod builds are left out. An
	// existing pack is updated where it lies: new and rewritten files are appended and the rest keep their place, or
	// nothing is written when nothing changed. It is rewritten whole when there is none, or when bytes it no longer
	// indexes would pass a quarter of it.
	bool Build(std::string& outSummary);
	bool Build(std::string& outSummary, bool rewrite);
	bool Build(std::string& outSummary, const BuildOptions& options);
}
