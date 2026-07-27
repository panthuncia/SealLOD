#include "Telemetry/Timing.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace br::telemetry::timing {
namespace {

constexpr size_t RetainedSamplesPerMetric = 16384u;

struct Series {
    std::vector<uint64_t> samplesNs;
    uint64_t count = 0u;
    uint64_t totalNs = 0u;
    uint64_t maxNs = 0u;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::string, Series> timings;
    std::unordered_map<std::string, uint64_t> counters;
    std::unordered_map<std::string, uint64_t> gauges;
};

Registry& GetRegistry()
{
    // Intentionally process-lifetime: the exit reporter runs after ordinary
    // function-local statics may have been destroyed.
    static Registry* registry = new Registry();
    return *registry;
}

const std::string& OutputPath()
{
    static const std::string* path = new std::string([] {
        char* value = nullptr;
        size_t length = 0u;
        if (_dupenv_s(
                &value,
                &length,
                "SARP_TIMING_REPORT_OUTPUT") != 0 ||
            value == nullptr) {
            return std::string{};
        }
        std::string result(value);
        std::free(value);
        return result;
    }());
    return *path;
}

nlohmann::json Summarize(const Series& series)
{
    nlohmann::json result{
        {"count", series.count},
        {"retained_samples", series.samplesNs.size()},
        {"total_us", series.totalNs / 1000u},
        {"mean_us", series.count == 0u
            ? 0.0
            : static_cast<double>(series.totalNs) /
                (1000.0 * static_cast<double>(series.count))},
        {"p50_us", 0.0},
        {"p95_us", 0.0},
        {"p99_us", 0.0},
        {"max_us", static_cast<double>(series.maxNs) / 1000.0},
    };
    if (series.samplesNs.empty()) {
        return result;
    }

    auto sorted = series.samplesNs;
    std::sort(sorted.begin(), sorted.end());
    const auto percentileUs = [&sorted](uint32_t percentile) {
        const size_t index = std::min<size_t>(
            sorted.size() - 1u,
            ((sorted.size() - 1u) * percentile + 99u) / 100u);
        return static_cast<double>(sorted[index]) / 1000.0;
    };
    result["p50_us"] = percentileUs(50u);
    result["p95_us"] = percentileUs(95u);
    result["p99_us"] = percentileUs(99u);
    return result;
}

struct ReportAtExit {
    ~ReportAtExit() { WriteReport(); }
};

ReportAtExit g_reportAtExit;

} // namespace

bool Enabled() noexcept
{
    return !OutputPath().empty();
}

uint64_t NowNs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Record(std::string_view name, uint64_t elapsedNs)
{
    if (!Enabled() || name.empty()) {
        return;
    }
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    auto& series = registry.timings[std::string(name)];
    ++series.count;
    series.totalNs += elapsedNs;
    series.maxNs = std::max(series.maxNs, elapsedNs);
    if (series.samplesNs.size() < RetainedSamplesPerMetric) {
        series.samplesNs.push_back(elapsedNs);
    }
    else {
        series.samplesNs[
            (series.count - 1u) % RetainedSamplesPerMetric] =
            elapsedNs;
    }
}

void AddCounter(std::string_view name, uint64_t value)
{
    if (!Enabled() || name.empty()) {
        return;
    }
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    registry.counters[std::string(name)] += value;
}

void SetGauge(std::string_view name, uint64_t value)
{
    if (!Enabled() || name.empty()) {
        return;
    }
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    registry.gauges[std::string(name)] = value;
}

void MaxGauge(std::string_view name, uint64_t value)
{
    if (!Enabled() || name.empty()) {
        return;
    }
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    auto& gauge = registry.gauges[std::string(name)];
    gauge = std::max(gauge, value);
}

void WriteReport()
{
    const auto& outputPath = OutputPath();
    if (outputPath.empty()) {
        return;
    }

    nlohmann::json timings = nlohmann::json::object();
    nlohmann::json counters = nlohmann::json::object();
    nlohmann::json gauges = nlohmann::json::object();
    {
        auto& registry = GetRegistry();
        std::lock_guard lock(registry.mutex);
        for (const auto& [name, series] : registry.timings) {
            timings[name] = Summarize(series);
        }
        for (const auto& [name, value] : registry.counters) {
            counters[name] = value;
        }
        for (const auto& [name, value] : registry.gauges) {
            gauges[name] = value;
        }
    }

    const std::filesystem::path path(outputPath);
    std::error_code directoryError;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(
            path.parent_path(),
            directoryError);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        spdlog::error(
            "Timing telemetry: failed to open report '{}'",
            outputPath);
        return;
    }
    output << nlohmann::json{
        {"schema_version", 1u},
        {"clock", "steady_clock_nanoseconds"},
        {"retained_samples_per_metric", RetainedSamplesPerMetric},
        {"timings", std::move(timings)},
        {"counters", std::move(counters)},
        {"gauges", std::move(gauges)},
    }.dump(2) << '\n';
}

} // namespace br::telemetry::timing
