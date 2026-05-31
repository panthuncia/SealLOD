#pragma once

#include <optional>
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
};

std::optional<std::string> DiscoverExecutable(const ClientOptions& options = {});
std::optional<ServiceInfo> DescribeServices(const ClientOptions& options = {}, std::string* errorMessage = nullptr);
std::optional<UsdAssetPackage> ConvertNifToUsd(const std::string& nifPath, const ClientOptions& options = {}, std::string* errorMessage = nullptr, TimingStats* timingStats = nullptr);

} // namespace BRNiflyClient
