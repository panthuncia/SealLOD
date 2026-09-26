#pragma once

#include <optional>
#include <cstdint>
#include <string>
#include <vector>

namespace BRNiflyClient {

struct Diagnostic {
    std::string level;
    std::string message;
};

struct ServiceInfo {
    std::string executablePath;
    std::string protocolVersion;
    std::string niflyVersion;
    std::string openUsdVersion;
    std::vector<std::string> services;
    std::vector<std::string> supportedGames;
    std::vector<Diagnostic> diagnostics;
};

struct UsdAssetPackage {
    std::string sourcePath;
    std::string sourceIdentifier;
    std::string contentHash;
    std::string rootLayerText;
    std::vector<std::string> dependencies;
    std::vector<std::string> textureSearchRoots;
    std::vector<Diagnostic> diagnostics;
};

struct ClientOptions {
    std::string executablePath;
    int timeoutMilliseconds = 120000;
};

struct TimingStats {
    std::uint64_t describeServicesMs = 0;
    std::uint64_t convertProcessMs = 0;
    std::uint64_t clientPersistentStartMs = 0;
    std::uint64_t clientWriteRequestMs = 0;
    std::uint64_t clientWaitResponseMs = 0;
    std::uint64_t clientWaitFirstByteMs = 0;
    std::uint64_t clientWaitMoreResponseMs = 0;
    std::uint64_t clientReadResponseMs = 0;
    std::uint64_t clientResponseBytes = 0;
    std::uint64_t clientResponseChunks = 0;
    std::uint64_t clientSharedMemoryReadMs = 0;
    std::uint64_t clientParseJsonMs = 0;
    std::uint64_t childLoadNiflyApiMs = 0;
    std::uint64_t childNiflyLoadMs = 0;
    std::uint64_t childGetGameNameMs = 0;
    std::uint64_t childReadNodesMs = 0;
    std::uint64_t childReadShapesMs = 0;
    std::uint64_t childReadExtraDataMs = 0;
    std::uint64_t childDestroyNifMs = 0;
    std::uint64_t childConvertShapesToUsdMs = 0;
    std::uint64_t childUsdExportToStringMs = 0;
    std::uint64_t childHashAndResponseMs = 0;
    std::uint64_t childSharedMemoryCreateMs = 0;
    std::uint64_t childJsonDumpMs = 0;
};

std::optional<std::string> DiscoverExecutable(const ClientOptions& options = {});
std::optional<ServiceInfo> DescribeServices(const ClientOptions& options = {}, std::string* errorMessage = nullptr);
std::optional<UsdAssetPackage> ConvertNifToUsd(const std::string& nifPath, const ClientOptions& options = {}, std::string* errorMessage = nullptr, TimingStats* timingStats = nullptr);

} // namespace BRNiflyClient
