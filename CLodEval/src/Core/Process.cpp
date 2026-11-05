#include "Core/Process.h"

#include <algorithm>
#include <cwctype>
#include <format>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace clodeval {

namespace {
	std::wstring Widen(const std::string& text)
	{
		if (text.empty()) {
			return {};
		}
		const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		std::wstring wide(static_cast<size_t>(length), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
		return wide;
	}

	// The quoting rules CommandLineToArgvW and the CRT undo.
	void AppendQuoted(std::wstring& commandLine, const std::wstring& argument)
	{
		commandLine += L'"';
		size_t backslashes = 0;
		for (wchar_t character : argument) {
			if (character == L'\\') {
				++backslashes;
				continue;
			}
			if (character == L'"') {
				commandLine.append(backslashes * 2u + 1u, L'\\');
			}
			else {
				commandLine.append(backslashes, L'\\');
			}
			backslashes = 0;
			commandLine += character;
		}
		commandLine.append(backslashes * 2u, L'\\');
		commandLine += L'"';
	}

	std::wstring BuildEnvironmentBlock(const ProcessRequest& request)
	{
		// Names compare case-insensitively on Windows, so the map is keyed on the uppercased name.
		std::map<std::wstring, std::wstring> variables;
		const auto upper = [](std::wstring text) {
			std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towupper(character)); });
			return text;
		};
		std::vector<std::wstring> clearPrefixes;
		for (const std::string& prefix : request.clearEnvironmentPrefixes) {
			clearPrefixes.push_back(upper(Widen(prefix)));
		}

		wchar_t* inherited = GetEnvironmentStringsW();
		for (const wchar_t* entry = inherited; *entry != L'\0'; entry += wcslen(entry) + 1u) {
			const std::wstring text(entry);
			const size_t separator = text.find(L'=', 1u);  // entries like "=C:=C:\dir" start with '='
			if (separator == std::wstring::npos) {
				continue;
			}
			const std::wstring name = upper(text.substr(0u, separator));
			if (std::ranges::any_of(clearPrefixes, [&](const std::wstring& prefix) { return name.starts_with(prefix); })) {
				continue;
			}
			variables[name] = text.substr(separator + 1u);
		}
		FreeEnvironmentStringsW(inherited);

		for (const auto& [name, value] : request.environment) {
			variables[upper(Widen(name))] = Widen(value);
		}
		std::wstring path;
		for (const std::filesystem::path& directory : request.pathPrepend) {
			path += directory.wstring() + L";";
		}
		variables[L"PATH"] = path + variables[L"PATH"];

		std::wstring block;
		for (const auto& [name, value] : variables) {
			block += name + L"=" + value;
			block += L'\0';
		}
		block += L'\0';
		return block;
	}
}

int RunProcess(const ProcessRequest& request, std::string& outError)
{
	std::wstring commandLine;
	AppendQuoted(commandLine, request.executable.wstring());
	for (const std::string& argument : request.arguments) {
		commandLine += L' ';
		AppendQuoted(commandLine, Widen(argument));
	}

	SECURITY_ATTRIBUTES inheritable{};
	inheritable.nLength = sizeof(inheritable);
	inheritable.bInheritHandle = TRUE;
	HANDLE log = CreateFileW(request.logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (log == INVALID_HANDLE_VALUE) {
		outError = std::format("could not create log '{}'", request.logPath.string());
		return -1;
	}

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startup.hStdOutput = log;
	startup.hStdError = log;

	std::wstring environment = BuildEnvironmentBlock(request);
	PROCESS_INFORMATION process{};
	const BOOL started = CreateProcessW(
		request.executable.c_str(),
		commandLine.data(),
		nullptr,
		nullptr,
		TRUE,
		CREATE_UNICODE_ENVIRONMENT,
		environment.data(),
		request.workingDirectory.empty() ? nullptr : request.workingDirectory.c_str(),
		&startup,
		&process);
	const DWORD startError = GetLastError();
	CloseHandle(log);
	if (!started) {
		outError = std::format("could not start '{}' (error {})", request.executable.string(), startError);
		return -1;
	}

	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD exitCode = 1;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return static_cast<int>(exitCode);
}

std::filesystem::path GetExecutableDirectory()
{
	std::wstring path(32768u, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	path.resize(length);
	return std::filesystem::path(path).parent_path();
}

}
