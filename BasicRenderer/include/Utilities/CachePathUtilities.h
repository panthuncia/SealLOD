#pragma once

// Minimal utility functions needed by CLod cache I/O code.

#include <filesystem>
#include <string>
#include <string_view>

std::string ws2s(const std::wstring_view& wstr);
std::wstring s2ws(const std::string_view& str);
std::filesystem::path GetCacheRootPath();
// Construct a cache path without touching the filesystem. Use this for reads
// where the cache directory must already exist.
std::wstring BuildCacheFilePath(const std::wstring& fileName, const std::wstring& directory);
std::wstring GetCacheFilePath(const std::wstring& fileName, const std::wstring& directory);

/// Canonicalise a file path so that cache identities hash identically
/// regardless of whether the caller supplied a relative or absolute path.
/// Returns weakly_canonical().generic_string(), which uses forward slashes
/// and resolves .., symlinks, etc.
std::string NormalizeCacheSourcePath(const std::string& path);
