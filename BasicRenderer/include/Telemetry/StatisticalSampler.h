#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Telemetry/NvPerfIntegration.h>

namespace br::telemetry::sampling {

enum class MeasurementSource {
    NvPerf,
    RenderGraphGpuTime
};

struct MetricConfiguration {
    br::telemetry::nvperf::MetricRequest request;
    MeasurementSource source = MeasurementSource::NvPerf;
    double relativeHalfWidth = 0.05;
    std::optional<double> absoluteHalfWidth;
};

struct PassConfiguration {
    std::string id;
    std::string name;
    std::string queue = "Graphics";
    uint32_t occurrence = 0;
    std::vector<std::string> metricIds;
    bool required = true;
};

struct Configuration {
    std::vector<MetricConfiguration> metrics;
    std::vector<PassConfiguration> passes;
    uint32_t minimumSamples = 20;
    uint32_t maximumSamples = 100;
    double confidenceLevel = 0.95;
    uint32_t readyFrames = 120;
    uint32_t settlingFrames = 30;
    uint32_t readinessTimeoutMs = 300000;
    std::filesystem::path databasePath = "build/basicrenderer_sampling.sqlite";
    std::filesystem::path summaryPath = "build/basicrenderer_sampling_last_run.md";
    std::string name = "BasicRenderer GPU sampling";
    std::string rawJson;
};

struct ReadinessSnapshot {
    std::map<std::string, int64_t> values;

    std::string ToJson() const;
};

struct Measurement {
    std::string passId;
    std::string metricId;
    double value = 0.0;
};

struct SampleRecord {
    uint32_t ordinal = 0;
    uint64_t startFrame = 0;
    uint64_t endFrame = 0;
    bool accepted = false;
    std::string rejectionReason;
    ReadinessSnapshot before;
    ReadinessSnapshot after;
    br::telemetry::nvperf::CaptureResult capture;
    std::vector<Measurement> measurements;
};

struct Summary {
    std::string passId;
    std::string metricId;
    uint32_t rawSampleCount = 0;
    double effectiveSampleCount = 0.0;
    double mean = 0.0;
    double standardDeviation = 0.0;
    double coefficientOfVariation = 0.0;
    double median = 0.0;
    double p05 = 0.0;
    double p95 = 0.0;
    double confidenceLow = 0.0;
    double confidenceHigh = 0.0;
    double relativeHalfWidth = 0.0;
    double lag1Autocorrelation = 0.0;
    uint32_t madOutlierCount = 0;
    bool converged = false;
};

Configuration LoadConfiguration(const std::filesystem::path& path);
std::vector<Summary> ComputeSummaries(
    const Configuration& configuration,
    const std::vector<SampleRecord>& samples);
bool AllRequiredTargetsConverged(
    const Configuration& configuration,
    const std::vector<Summary>& summaries);
std::vector<Measurement> SelectMeasurements(
    const Configuration& configuration,
    const br::telemetry::nvperf::CaptureResult& capture,
    std::string& rejectionReason);

class Database {
public:
    Database();
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void Open(const Configuration& configuration);
    int64_t BeginExperiment(const Configuration& configuration);
    void RecordDiagnostic(int64_t experimentId, const std::string& key, const std::string& value);
    void RecordSample(int64_t experimentId, const SampleRecord& sample);
    void ReplaceSummaries(int64_t experimentId, const std::vector<Summary>& summaries);
    void FinishExperiment(int64_t experimentId, const std::string& status, const std::string& stoppingReason);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

void WriteLastRunSummary(
    const Configuration& configuration,
    int64_t experimentId,
    uint64_t readinessDurationMs,
    size_t scheduledReplayPasses,
    const std::vector<SampleRecord>& samples,
    const std::vector<Summary>& summaries,
    const std::string& stoppingReason);
void WriteRunSummary(
    const Configuration& configuration,
    const std::filesystem::path& outputPath,
    int64_t experimentId,
    uint64_t readinessDurationMs,
    size_t scheduledReplayPasses,
    const std::vector<SampleRecord>& samples,
    const std::vector<Summary>& summaries,
    const std::string& stoppingReason);

} // namespace br::telemetry::sampling
