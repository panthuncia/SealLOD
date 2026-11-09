#pragma once

// Minimal utility functions needed by CLOD cache I/O code.

#include <filesystem>
#include <string>
#include <string_view>

std::string ws2s(const std::wstring_view& wstr);
std::wstring s2ws(const std::string_view& str);
std::filesystem::path GetCacheRootPath();
// Chooses the process's cache root, which is otherwise SARP_CACHE_ROOT or <exe>/cache. The root is resolved once, on
// first use: before that this always succeeds; after, it succeeds only if root is the root already in use.
bool SetCacheRootPath(const std::filesystem::path& root);
// A scene cache kept apart from the cache root (a scene manifest's, opened once the root was already in use): the CLod
// data of the scene with this source identifier is read and written under root instead.
void SetSceneCacheRoot(const std::string& sourceIdentifier, const std::filesystem::path& root);
// The root SetSceneCacheRoot gave the scene, or the cache root.
std::filesystem::path GetSceneCacheRoot(const std::string& sourceIdentifier);
// Construct a cache path without touching the filesystem. Use this for reads
// where the cache directory must already exist.
std::wstring BuildCacheFilePath(const std::wstring& fileName, const std::wstring& directory);
std::wstring GetCacheFilePath(const std::wstring& fileName, const std::wstring& directory);
// The same under another root.
std::wstring BuildCacheFilePathUnder(const std::filesystem::path& root, const std::wstring& fileName, const std::wstring& directory);
std::wstring GetCacheFilePathUnder(const std::filesystem::path& root, const std::wstring& fileName, const std::wstring& directory);

/// Canonicalise a file path so that cache identities hash identically
/// regardless of whether the caller supplied a relative or absolute path.
/// Returns weakly_canonical().generic_string(), which uses forward slashes
/// and resolves .., symlinks, etc.
std::string NormalizeCacheSourcePath(const std::string& path);
