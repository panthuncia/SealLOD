#include <BasicRenderer/Assets/CachePaths.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <windows.h>

std::wstring s2ws(const std::string_view& utf8)
{
	if (utf8.empty()) return {};
	int needed = ::MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		utf8.data(),
		static_cast<int>(utf8.size()),
		nullptr,
		0
	);
	if (needed == 0)
		throw std::system_error(::GetLastError(), std::system_category(),
			"MultiByteToWideChar(size)");

	std::wstring out(needed, L'\0');

	int written = ::MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		utf8.data(),
		static_cast<int>(utf8.size()),
		out.data(),
		needed
	);
	if (written == 0)
		throw std::system_error(::GetLastError(), std::system_category(),
			"MultiByteToWideChar(data)");

	return out;
}

std::string ws2s(const std::wstring_view& wide)
{
	if (wide.empty()) return {};

	int needed = ::WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		wide.data(),
		static_cast<int>(wide.size()),
		nullptr,
		0,
		nullptr, nullptr
	);
	if (needed == 0)
		throw std::system_error(::GetLastError(), std::system_category(),
			"WideCharToMultiByte(size)");

	std::string out(needed, '\0');

	int written = ::WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		wide.data(),
		static_cast<int>(wide.size()),
		out.data(),
		needed,
		nullptr, nullptr
	);
	if (written == 0)
		throw std::system_error(::GetLastError(), std::system_category(),
			"WideCharToMultiByte(data)");

	return out;
}

namespace {
std::optional<std::filesystem::path> GetExecutableDirectory()
{
	std::vector<wchar_t> modulePath(MAX_PATH);
	while (true) {
		const DWORD written = ::GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
		if (written == 0) {
			break;
		}
		if (written < modulePath.size()) {
			std::filesystem::path path(modulePath.data());
			std::error_code ec;
			path = std::filesystem::weakly_canonical(path, ec);
			if (ec) {
				path = std::filesystem::path(modulePath.data());
			}
			return path.parent_path();
		}
		modulePath.resize(modulePath.size() * 2u);
	}
	return std::nullopt;
}

std::filesystem::path RemapBuildOutputAssetPath(const std::filesystem::path& canonicalPath)
{
	std::vector<std::filesystem::path> parts;
	for (const auto& part : canonicalPath) {
		parts.push_back(part);
	}

	for (std::size_t outIndex = 0; outIndex < parts.size(); ++outIndex) {
		if (parts[outIndex] != "out" || outIndex == 0u) {
			continue;
		}
		for (std::size_t assetRootIndex = outIndex + 1u; assetRootIndex < parts.size(); ++assetRootIndex) {
			if (parts[assetRootIndex] != "models" && parts[assetRootIndex] != "textures") {
				continue;
			}

			std::filesystem::path sourceRoot;
			for (std::size_t i = 0; i < outIndex; ++i) {
				sourceRoot /= parts[i];
			}
			std::filesystem::path candidate = sourceRoot;
			for (std::size_t i = assetRootIndex; i < parts.size(); ++i) {
				candidate /= parts[i];
			}

			std::error_code ec;
			if (std::filesystem::exists(candidate, ec)) {
				return candidate;
			}
		}
	}
	return canonicalPath;
}
}

namespace {
	std::mutex g_cacheRootMutex;
	std::optional<std::filesystem::path> g_cacheRootOverride;
	bool g_cacheRootResolved = false;
}

std::filesystem::path GetCacheRootPath()
{
	// Cache root selection is process-wide configuration. Resolving the module
	// path and querying the environment for every artifact was a substantial
	// source of contention during parallel scene preparation.
	static const std::filesystem::path cacheRoot = [] {
		std::lock_guard lock(g_cacheRootMutex);
		g_cacheRootResolved = true;
		if (g_cacheRootOverride) {
			return *g_cacheRootOverride;
		}
		const DWORD envLength = ::GetEnvironmentVariableA("SARP_CACHE_ROOT", nullptr, 0);
		if (envLength > 1) {
			std::vector<char> envRoot(envLength);
			if (::GetEnvironmentVariableA("SARP_CACHE_ROOT", envRoot.data(), envLength) != 0 && envRoot.front() != '\0') {
				return std::filesystem::path(envRoot.data());
			}
		}

		if (auto executableDirectory = GetExecutableDirectory()) {
			return *executableDirectory / L"cache";
		}

		return std::filesystem::current_path() / L"cache";
	}();
	return cacheRoot;
}

bool SetCacheRootPath(const std::filesystem::path& root)
{
	{
		std::lock_guard lock(g_cacheRootMutex);
		if (!g_cacheRootResolved) {
			g_cacheRootOverride = std::filesystem::absolute(root);
			return true;
		}
	}
	std::error_code error;
	if (std::filesystem::equivalent(GetCacheRootPath(), root, error)) {
		return true;
	}
	return std::filesystem::weakly_canonical(GetCacheRootPath(), error).generic_string() ==
		std::filesystem::weakly_canonical(root, error).generic_string();
}

std::wstring BuildCacheFilePath(const std::wstring& fileName, const std::wstring& directory) {
	return (GetCacheRootPath() / directory / fileName).wstring();
}

namespace {
	std::mutex g_sceneCacheRootsMutex;
	std::unordered_map<std::string, std::filesystem::path> g_sceneCacheRoots;
}

void SetSceneCacheRoot(const std::string& sourceIdentifier, const std::filesystem::path& root)
{
	std::lock_guard lock(g_sceneCacheRootsMutex);
	g_sceneCacheRoots[sourceIdentifier] = std::filesystem::absolute(root);
}

std::filesystem::path GetSceneCacheRoot(const std::string& sourceIdentifier)
{
	{
		std::lock_guard lock(g_sceneCacheRootsMutex);
		if (const auto found = g_sceneCacheRoots.find(sourceIdentifier); found != g_sceneCacheRoots.end()) return found->second;
	}
	return GetCacheRootPath();
}

std::wstring BuildCacheFilePathUnder(const std::filesystem::path& root, const std::wstring& fileName, const std::wstring& directory) {
	return (root / directory / fileName).wstring();
}

std::wstring GetCacheFilePath(const std::wstring& fileName, const std::wstring& directory) {
	return GetCacheFilePathUnder(GetCacheRootPath(), fileName, directory);
}

std::wstring GetCacheFilePathUnder(const std::filesystem::path& cacheRoot, const std::wstring& fileName, const std::wstring& directory) {
	std::filesystem::path cacheDir = cacheRoot / directory;

	// Avoid repeated OS syscalls: only call create_directories once per unique
	// directory path.  The set persists for the lifetime of the process. The
	// call itself runs outside the lock: under a virtual filesystem one takes
	// milliseconds, and holding the lock queued every other caller behind it.
	{
		static std::mutex s_ensuredMutex;
		static std::unordered_set<std::wstring> s_ensuredDirs;
		std::wstring cacheDirStr = cacheDir.wstring();
		{
			std::lock_guard<std::mutex> lock(s_ensuredMutex);
			if (s_ensuredDirs.contains(cacheDirStr)) {
				return (cacheDir / fileName).wstring();
			}
		}
		std::filesystem::create_directories(cacheDir);
		std::lock_guard<std::mutex> lock(s_ensuredMutex);
		s_ensuredDirs.insert(std::move(cacheDirStr));
	}

	return (cacheDir / fileName).wstring();
}

std::string NormalizeCacheSourcePath(const std::string& path) {
	if (path.empty()) return path;

	constexpr std::string_view sarpNifScheme = "sarp-nif://";
	if (path.size() >= sarpNifScheme.size() &&
		std::equal(sarpNifScheme.begin(), sarpNifScheme.end(), path.begin(), [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		})) {
		std::string normalized = "sarp-nif://";
		normalized.reserve(path.size());
		for (std::size_t i = sarpNifScheme.size(); i < path.size(); ++i) {
			const char ch = path[i] == '\\' ? '/' : path[i];
			normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
		}
		return normalized;
	}

	std::error_code ec;
	const std::filesystem::path source(path);
	std::filesystem::path canonical;
	// A relative path here is a game path, which does not exist beneath the working
	// directory, so only its directories can affect the result. Canonicalizing walks every
	// component through the filesystem; thousands of textures share a few hundred folders.
	if (source.is_relative() && source.has_parent_path() && source.has_filename() &&
		source.filename() != "." && source.filename() != "..") {
		static std::shared_mutex parentsMutex;
		static std::unordered_map<std::wstring, std::filesystem::path> parents;
		const auto parent = source.parent_path();
		{
			std::shared_lock lock(parentsMutex);
			if (const auto known = parents.find(parent.native()); known != parents.end()) canonical = known->second;
		}
		if (canonical.empty()) {
			canonical = std::filesystem::weakly_canonical(parent, ec);
			if (ec) return path;
			std::unique_lock lock(parentsMutex);
			parents.insert_or_assign(parent.native(), canonical);
		}
		canonical /= source.filename();
	} else {
		canonical = std::filesystem::weakly_canonical(source, ec);
		if (ec) return path; // if canonicalisation fails, use original
	}
	return RemapBuildOutputAssetPath(canonical).generic_string(); // forward-slash, absolute
}
