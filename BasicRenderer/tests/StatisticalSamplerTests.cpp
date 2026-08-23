#include "Telemetry/StatisticalSampler.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

br::telemetry::sampling::SampleRecord MakeSample(uint32_t ordinal, double value)
{
    br::telemetry::sampling::SampleRecord sample;
    sample.ordinal = ordinal;
    sample.accepted = true;
    sample.measurements.push_back({ "clod_voxel_software_rasterize_pass1", "gpu_time", value });
    return sample;
}

} // namespace

int main()
{
    try {
        auto configuration = br::telemetry::sampling::LoadConfiguration(STATISTICAL_SAMPLER_TEST_CONFIG);
        Require(configuration.minimumSamples == 20, "configuration minimum sample count");
        Require(configuration.passes.front().name == "HierarchicalCullingPass1", "pass parsing");
        Require(configuration.metrics.front().source == br::telemetry::sampling::MeasurementSource::NvPerf,
            "default NVPerf source parsing");
        Require(configuration.metrics.back().source == br::telemetry::sampling::MeasurementSource::RenderGraphGpuTime,
            "render-graph timestamp source parsing");

        br::telemetry::nvperf::CaptureResult capture;
        capture.success = true;
        capture.metrics.push_back(configuration.metrics.front().request);
        capture.ranges.push_back({
            "Graphics",
            "HierarchicalCullingPass1",
            0,
            0,
            { 125.0 }
        });
        auto selectionConfig = configuration;
        selectionConfig.passes.front().metricIds = { "gpu_time" };
        selectionConfig.passes.resize(1);
        selectionConfig.metrics.resize(1);
        std::string rejection;
        const auto measurements = br::telemetry::sampling::SelectMeasurements(selectionConfig, capture, rejection);
        Require(rejection.empty() && measurements.size() == 1 && measurements.front().value == 125.0,
            "pass occurrence measurement selection");

        std::vector<br::telemetry::sampling::SampleRecord> constantSamples;
        for (uint32_t index = 0; index < 20; ++index) constantSamples.push_back(MakeSample(index + 1, 100.0));
        const auto constant = br::telemetry::sampling::ComputeSummaries(selectionConfig, constantSamples);
        Require(constant.size() == 1, "constant summary count");
        Require(constant.front().mean == 100.0 && constant.front().confidenceLow == 100.0,
            "constant confidence interval");
        Require(constant.front().converged, "constant sequence convergence");

        std::vector<br::telemetry::sampling::SampleRecord> correlatedSamples;
        double value = 0.0;
        for (uint32_t index = 0; index < 80; ++index) {
            value = 0.95 * value + std::sin(static_cast<double>(index) * 0.17);
            correlatedSamples.push_back(MakeSample(index + 1, 100.0 + value));
        }
        const auto correlated = br::telemetry::sampling::ComputeSummaries(selectionConfig, correlatedSamples);
        Require(correlated.front().effectiveSampleCount <= correlated.front().rawSampleCount,
            "effective sample count clamped to raw count");
        Require(correlated.front().lag1Autocorrelation > 0.0, "autocorrelation detected");

        auto databaseConfig = selectionConfig;
        databaseConfig.rawJson = "{}";
        databaseConfig.databasePath =
            std::filesystem::temp_directory_path() / "basicrenderer_sampling_tests.sqlite";
        std::error_code ignored;
        std::filesystem::remove(databaseConfig.databasePath, ignored);
        std::filesystem::remove(databaseConfig.databasePath.string() + "-wal", ignored);
        std::filesystem::remove(databaseConfig.databasePath.string() + "-shm", ignored);
        {
            br::telemetry::sampling::Database database;
            database.Open(databaseConfig);
            const auto firstExperiment = database.BeginExperiment(databaseConfig);
            database.RecordSample(firstExperiment, constantSamples.front());
            database.ReplaceSummaries(firstExperiment, constant);
            database.FinishExperiment(firstExperiment, "complete", "first");
            const auto secondExperiment = database.BeginExperiment(databaseConfig);
            Require(secondExperiment > firstExperiment, "sequential experiment identifiers");
            database.RecordSample(secondExperiment, constantSamples.back());
            database.ReplaceSummaries(secondExperiment, constant);
            database.FinishExperiment(secondExperiment, "complete", "second");

            const auto immutableReport =
                std::filesystem::temp_directory_path() / "basicrenderer_sampling_tests_experiment.md";
            std::filesystem::remove(immutableReport, ignored);
            br::telemetry::sampling::WriteRunSummary(
                databaseConfig,
                immutableReport,
                secondExperiment,
                10,
                1,
                constantSamples,
                constant,
                "test");
            Require(std::filesystem::file_size(immutableReport) > 0, "immutable report creation");
            std::filesystem::remove(immutableReport, ignored);
        }
        Require(std::filesystem::file_size(databaseConfig.databasePath) > 0, "SQLite database creation");
        std::filesystem::remove(databaseConfig.databasePath, ignored);

        std::cout << "BasicRenderer statistical sampler tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "BasicRenderer statistical sampler tests failed: " << exception.what() << '\n';
        return 1;
    }
}
