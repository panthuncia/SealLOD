#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace clodeval {

struct ProcessRequest {
	std::filesystem::path executable;
	std::vector<std::string> arguments;
	std::filesystem::path workingDirectory;
	std::filesystem::path logPath;  // stdout and stderr go here
	// Inherited variables starting with one of these are removed first, so what the child sees is
	// only what the caller asked for and a stray setting in the user's shell cannot change a build.
	std::vector<std::string> clearEnvironmentPrefixes;
	std::map<std::string, std::string> environment;
	std::vector<std::filesystem::path> pathPrepend;
};

// Returns the exit code, or -1 with outError set if the process could not start.
int RunProcess(const ProcessRequest& request, std::string& outError);

// The directory holding the running executable.
std::filesystem::path GetExecutableDirectory();

}
