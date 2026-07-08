#include "Utilities/CachePathUtilities.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
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

std::filesystem::path GetCacheRootPath()
{
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
}

std::wstring GetCacheFilePath(const std::wstring& fileName, const std::wstring& directory) {
	const std::filesystem::path cacheRoot = GetCacheRootPath();
	std::filesystem::path cacheDir = cacheRoot / directory;

	// Avoid repeated OS syscalls: only call create_directories once per unique
	// directory path.  The set persists for the lifetime of the process.
	{
		static std::mutex s_ensuredMutex;
		static std::unordered_set<std::wstring> s_ensuredDirs;
		std::wstring cacheDirStr = cacheDir.wstring();
		std::lock_guard<std::mutex> lock(s_ensuredMutex);
		if (s_ensuredDirs.find(cacheDirStr) == s_ensuredDirs.end()) {
			std::filesystem::create_directories(cacheDir);
			s_ensuredDirs.insert(std::move(cacheDirStr));
		}
	}

	std::filesystem::path filePath = cacheDir / fileName;
	return filePath.wstring();
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
	auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
	if (ec) return path; // if canonicalisation fails, use original
	return RemapBuildOutputAssetPath(canonical).generic_string(); // forward-slash, absolute
}
