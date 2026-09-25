#include "Telemetry/StatisticalSampler.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <boost/math/distributions/students_t.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace br::telemetry::sampling {
namespace {

using json = nlohmann::json;

uint8_t ParseMetricType(const std::string& value)
{
    if (value == "counter") return 0;
    if (value == "ratio") return 1;
    if (value == "throughput") return 2;
    throw std::runtime_error("unknown NVPerf metric type: " + value);
}

uint8_t ParseRollup(const std::string& value)
{
    if (value == "avg") return 0;
    if (value == "max") return 1;
    if (value == "min") return 2;
    if (value == "sum") return 3;
    throw std::runtime_error("unknown NVPerf rollup: " + value);
}

uint16_t ParseSubmetric(const std::string& value)
{
    static const std::unordered_map<std::string, uint16_t> values{
        { "none", 0 }, { "per_cycle_active", 10 }, { "per_cycle_elapsed", 11 },
        { "per_second", 14 }, { "pct_of_peak_sustained_active", 15 },
        { "pct_of_peak_sustained_elapsed", 16 }, { "pct", 20 }, { "ratio", 21 }
    };
    const auto it = values.find(value);
    if (it == values.end()) {
        throw std::runtime_error("unknown NVPerf submetric: " + value);
    }
    return it->second;
}

double Percentile(std::vector<double> values, double probability)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double index = probability * static_cast<double>(values.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(index));
    const size_t upper = static_cast<size_t>(std::ceil(index));
    if (lower == upper) return values[lower];
    const double fraction = index - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

const MetricConfiguration* FindMetric(const Configuration& config, const std::string& id)
{
    const auto it = std::ranges::find_if(config.metrics, [&](const auto& metric) {
        return metric.request.id == id;
    });
    return it == config.metrics.end() ? nullptr : &*it;
}

const PassConfiguration* FindPass(const Configuration& config, const std::string& id)
{
    const auto it = std::ranges::find_if(config.passes, [&](const auto& pass) {
        return pass.id == id;
    });
    return it == config.passes.end() ? nullptr : &*it;
}

void CheckSqlite(int result, sqlite3* db, const char* operation)
{
    if (result == SQLITE_OK || result == SQLITE_DONE || result == SQLITE_ROW) return;
    throw std::runtime_error(std::string(operation) + ": " + (db ? sqlite3_errmsg(db) : "SQLite error"));
}

void Exec(sqlite3* db, const char* sql)
{
    char* error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error ? error : "SQLite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql)
    {
        CheckSqlite(sqlite3_prepare_v2(db, sql, -1, &m_statement, nullptr), db, "sqlite3_prepare_v2");
    }
    ~Statement() { sqlite3_finalize(m_statement); }
    sqlite3_stmt* get() const { return m_statement; }
    void reset()
    {
        sqlite3_reset(m_statement);
        sqlite3_clear_bindings(m_statement);
    }
private:
    sqlite3_stmt* m_statement = nullptr;
};

std::optional<std::string> EnvironmentValue(const char* name)
{
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) return std::nullopt;
    std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
    if (length <= 1) return std::nullopt;
    return std::string(value);
}

void Bind(sqlite3_stmt* statement, int index, const std::string& value)
{
    sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

} // namespace

std::string ReadinessSnapshot::ToJson() const
{
    json output = json::object();
    for (const auto& [key, value] : values) output[key] = value;
    return output.dump();
}

Configuration LoadConfiguration(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("failed to open sampling configuration: " + path.string());
    json document;
    stream >> document;

    Configuration config;
    config.rawJson = document.dump();
    config.name = document.value("name", config.name);
    const auto statistics = document.value("statistics", json::object());
    config.minimumSamples = statistics.value("minimum_samples", config.minimumSamples);
    config.maximumSamples = statistics.value("maximum_samples", config.maximumSamples);
    config.confidenceLevel = statistics.value("confidence_level", config.confidenceLevel);
    const double defaultRelativeHalfWidth = statistics.value("relative_half_width", 0.05);
    const auto readiness = document.value("readiness", json::object());
    config.readyFrames = readiness.value("stable_frames", config.readyFrames);
    config.settlingFrames = readiness.value("settling_frames", config.settlingFrames);
    config.readinessTimeoutMs = readiness.value("timeout_ms", config.readinessTimeoutMs);
    const auto output = document.value("output", json::object());
    config.databasePath = output.value("database", config.databasePath.string());
    config.summaryPath = output.value("last_run_summary", config.summaryPath.string());
    if (const auto databaseOverride = EnvironmentValue("BASICRENDERER_SAMPLING_DATABASE")) {
        config.databasePath = *databaseOverride;
    }
    if (const auto summaryOverride = EnvironmentValue("BASICRENDERER_SAMPLING_SUMMARY")) {
        config.summaryPath = *summaryOverride;
    }

    std::set<std::string> metricIds;
    for (const auto& item : document.at("metrics")) {
        MetricConfiguration metric;
        metric.request.id = item.at("id").get<std::string>();
        metric.request.name = item.at("name").get<std::string>();
        const std::string source = item.value("source", "nvperf");
        if (source == "nvperf") {
            metric.source = MeasurementSource::NvPerf;
        } else if (source == "render_graph_gpu_time") {
            metric.source = MeasurementSource::RenderGraphGpuTime;
        } else {
            throw std::runtime_error("unknown sampling metric source: " + source);
        }
        metric.request.outputName = item.value("output_name", metric.request.id);
        metric.request.unit = item.value("unit", "");
        metric.request.metricType = ParseMetricType(item.value("type", "counter"));
        metric.request.rollupOp = ParseRollup(item.value("rollup", "sum"));
        metric.request.submetric = ParseSubmetric(item.value("submetric", "none"));
        metric.request.required = item.value("required", true);
        metric.relativeHalfWidth = item.value("relative_half_width", defaultRelativeHalfWidth);
        if (item.contains("absolute_half_width")) metric.absoluteHalfWidth = item.at("absolute_half_width").get<double>();
        if (!metricIds.insert(metric.request.id).second) throw std::runtime_error("duplicate metric id: " + metric.request.id);
        config.metrics.push_back(std::move(metric));
    }

    std::set<std::string> passIds;
    for (const auto& item : document.at("passes")) {
        PassConfiguration pass;
        pass.id = item.at("id").get<std::string>();
        pass.name = item.at("name").get<std::string>();
        pass.queue = item.value("queue", "Graphics");
        pass.occurrence = item.value("occurrence", 0u);
        pass.required = item.value("required", true);
        pass.metricIds = item.at("metrics").get<std::vector<std::string>>();
        if (!passIds.insert(pass.id).second) throw std::runtime_error("duplicate pass id: " + pass.id);
        for (const auto& metricId : pass.metricIds) {
            if (!metricIds.contains(metricId)) throw std::runtime_error("pass references unknown metric id: " + metricId);
        }
        config.passes.push_back(std::move(pass));
    }

    if (config.metrics.empty() || config.passes.empty()) throw std::runtime_error("sampling requires metrics and passes");
    if (config.minimumSamples < 1 || config.maximumSamples < config.minimumSamples) {
        throw std::runtime_error("invalid sampling minimum/maximum sample counts");
    }
    if (!(config.confidenceLevel > 0.0 && config.confidenceLevel < 1.0)) {
        throw std::runtime_error("confidence_level must be between zero and one");
    }
    return config;
}

std::vector<Measurement> SelectMeasurements(
    const Configuration& configuration,
    const br::telemetry::nvperf::CaptureResult& capture,
    std::string& rejectionReason)
{
    std::unordered_map<std::string, size_t> metricIndices;
    for (size_t index = 0; index < capture.metrics.size(); ++index) {
        metricIndices[capture.metrics[index].id] = index;
    }
    std::vector<Measurement> result;
    for (const auto& pass : configuration.passes) {
        const auto range = std::ranges::find_if(capture.ranges, [&](const auto& candidate) {
            return candidate.passName == pass.name &&
                (pass.queue.empty() || candidate.queue == pass.queue) &&
                candidate.occurrence == pass.occurrence;
        });
        if (range == capture.ranges.end()) {
            if (pass.required) rejectionReason = "required pass was not captured: " + pass.name;
            continue;
        }
        for (const auto& metricId : pass.metricIds) {
            const auto* metric = FindMetric(configuration, metricId);
            if (metric && metric->source != MeasurementSource::NvPerf) continue;
            const auto metricIndex = metricIndices.find(metricId);
            if (metricIndex == metricIndices.end() || metricIndex->second >= range->values.size()) {
                const auto* metric = FindMetric(configuration, metricId);
                if (metric && metric->request.required) rejectionReason = "required metric was not captured: " + metricId;
                continue;
            }
            const double value = range->values[metricIndex->second];
            if (!std::isfinite(value)) {
                rejectionReason = "non-finite metric value: " + metricId;
                continue;
            }
            result.push_back({ pass.id, metricId, value });
        }
    }
    return result;
}

std::vector<Summary> ComputeSummaries(
    const Configuration& configuration,
    const std::vector<SampleRecord>& samples)
{
    std::vector<Summary> summaries;
    for (const auto& pass : configuration.passes) {
        for (const auto& metricId : pass.metricIds) {
            std::vector<double> values;
            for (const auto& sample : samples) {
                if (!sample.accepted) continue;
                const auto measurement = std::ranges::find_if(sample.measurements, [&](const auto& value) {
                    return value.passId == pass.id && value.metricId == metricId;
                });
                if (measurement != sample.measurements.end()) values.push_back(measurement->value);
            }
            Summary summary;
            summary.passId = pass.id;
            summary.metricId = metricId;
            summary.rawSampleCount = static_cast<uint32_t>(values.size());
            if (values.empty()) {
                summaries.push_back(summary);
                continue;
            }
            const double n = static_cast<double>(values.size());
            summary.mean = std::accumulate(values.begin(), values.end(), 0.0) / n;
            double squared = 0.0;
            for (double value : values) squared += (value - summary.mean) * (value - summary.mean);
            const double sampleVariance = values.size() > 1 ? squared / (n - 1.0) : 0.0;
            summary.standardDeviation = std::sqrt(sampleVariance);
            summary.coefficientOfVariation = summary.mean != 0.0
                ? summary.standardDeviation / std::abs(summary.mean)
                : 0.0;
            summary.median = Percentile(values, 0.5);
            summary.p05 = Percentile(values, 0.05);
            summary.p95 = Percentile(values, 0.95);

            double longRunVariance = values.size() > 1 ? squared / n : 0.0;
            const size_t bandwidth = values.size() > 2
                ? std::min(values.size() - 1, std::max<size_t>(1, static_cast<size_t>(
                    std::floor(4.0 * std::pow(n / 100.0, 2.0 / 9.0)))))
                : 0;
            double lagOneCovariance = 0.0;
            for (size_t lag = 1; lag <= bandwidth; ++lag) {
                double covariance = 0.0;
                for (size_t index = lag; index < values.size(); ++index) {
                    covariance += (values[index] - summary.mean) * (values[index - lag] - summary.mean);
                }
                covariance /= n;
                if (lag == 1) lagOneCovariance = covariance;
                const double weight = 1.0 - static_cast<double>(lag) / static_cast<double>(bandwidth + 1);
                longRunVariance += 2.0 * weight * covariance;
            }
            longRunVariance = std::max(longRunVariance, 0.0);
            const double populationVariance = values.size() > 1 ? squared / n : 0.0;
            summary.lag1Autocorrelation = populationVariance > 0.0 ? lagOneCovariance / populationVariance : 0.0;
            if (longRunVariance > 0.0 && sampleVariance > 0.0) {
                summary.effectiveSampleCount = std::clamp(n * sampleVariance / longRunVariance, 1.0, n);
            } else {
                summary.effectiveSampleCount = n;
            }

            const double standardError = std::sqrt(longRunVariance / n);
            const double degreesOfFreedom = std::max(1.0, summary.effectiveSampleCount - 1.0);
            const boost::math::students_t distribution(degreesOfFreedom);
            const double critical = boost::math::quantile(
                distribution,
                0.5 + configuration.confidenceLevel / 2.0);
            const double halfWidth = critical * standardError;
            summary.confidenceLow = summary.mean - halfWidth;
            summary.confidenceHigh = summary.mean + halfWidth;
            summary.relativeHalfWidth = summary.mean != 0.0
                ? halfWidth / std::abs(summary.mean)
                : (halfWidth == 0.0 ? 0.0 : std::numeric_limits<double>::infinity());

            const double mad = Percentile([&] {
                std::vector<double> deviations;
                deviations.reserve(values.size());
                for (double value : values) deviations.push_back(std::abs(value - summary.median));
                return deviations;
            }(), 0.5);
            if (mad > 0.0) {
                for (double value : values) {
                    if (0.67448975 * std::abs(value - summary.median) / mad > 3.5) ++summary.madOutlierCount;
                }
            }

            const auto* metric = FindMetric(configuration, metricId);
            const bool precisionMet = halfWidth == 0.0 ||
                (metric && metric->absoluteHalfWidth && halfWidth <= *metric->absoluteHalfWidth) ||
                (metric && summary.mean != 0.0 && summary.relativeHalfWidth <= metric->relativeHalfWidth);
            summary.converged =
                summary.rawSampleCount >= configuration.minimumSamples &&
                summary.effectiveSampleCount >= static_cast<double>(configuration.minimumSamples) &&
                precisionMet;
            summaries.push_back(summary);
        }
    }
    return summaries;
}

bool AllRequiredTargetsConverged(
    const Configuration& configuration,
    const std::vector<Summary>& summaries)
{
    for (const auto& pass : configuration.passes) {
        if (!pass.required) continue;
        for (const auto& metricId : pass.metricIds) {
            const auto* metric = FindMetric(configuration, metricId);
            if (!metric || !metric->request.required) continue;
            const auto summary = std::ranges::find_if(summaries, [&](const auto& candidate) {
                return candidate.passId == pass.id && candidate.metricId == metricId;
            });
            if (summary == summaries.end() || !summary->converged) return false;
        }
    }
    return true;
}

struct Database::Impl {
    sqlite3* db = nullptr;
};

Database::Database() : m_impl(std::make_unique<Impl>()) {}
Database::~Database()
{
    if (m_impl->db) sqlite3_close(m_impl->db);
}

void Database::Open(const Configuration& configuration)
{
    if (configuration.databasePath.has_parent_path()) {
        std::filesystem::create_directories(configuration.databasePath.parent_path());
    }
    CheckSqlite(
        sqlite3_open_v2(
            configuration.databasePath.string().c_str(),
            &m_impl->db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr),
        m_impl->db,
        "sqlite3_open_v2");
    sqlite3_busy_timeout(m_impl->db, 10000);
    Exec(m_impl->db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=NORMAL;");
    Exec(m_impl->db, R"sql(
CREATE TABLE IF NOT EXISTS schema_info(version INTEGER NOT NULL);
INSERT INTO schema_info(version) SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM schema_info);
CREATE TABLE IF NOT EXISTS experiments(
 id INTEGER PRIMARY KEY, name TEXT NOT NULL, started_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 finished_at TEXT, status TEXT NOT NULL, stopping_reason TEXT, configuration_json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS diagnostics(
 experiment_id INTEGER NOT NULL REFERENCES experiments(id), key TEXT NOT NULL, value TEXT NOT NULL,
 PRIMARY KEY(experiment_id,key));
CREATE TABLE IF NOT EXISTS samples(
 id INTEGER PRIMARY KEY, experiment_id INTEGER NOT NULL REFERENCES experiments(id), ordinal INTEGER NOT NULL,
 start_frame INTEGER, end_frame INTEGER, accepted INTEGER NOT NULL, rejection_reason TEXT,
 readiness_before_json TEXT, readiness_after_json TEXT, scheduled_passes INTEGER,
 dropped_ranges INTEGER, dropped_trace_bytes INTEGER, profiler_error TEXT,
 UNIQUE(experiment_id,ordinal));
CREATE TABLE IF NOT EXISTS passes(
 experiment_id INTEGER NOT NULL REFERENCES experiments(id), pass_id TEXT NOT NULL, pass_name TEXT NOT NULL,
 queue TEXT NOT NULL, occurrence INTEGER NOT NULL, required INTEGER NOT NULL,
 PRIMARY KEY(experiment_id,pass_id));
CREATE TABLE IF NOT EXISTS metrics(
 experiment_id INTEGER NOT NULL REFERENCES experiments(id), metric_id TEXT NOT NULL, metric_name TEXT NOT NULL,
 output_name TEXT, unit TEXT, required INTEGER NOT NULL, relative_target REAL, absolute_target REAL,
 PRIMARY KEY(experiment_id,metric_id));
CREATE TABLE IF NOT EXISTS measurements(
 sample_id INTEGER NOT NULL REFERENCES samples(id), pass_id TEXT NOT NULL, metric_id TEXT NOT NULL, value REAL NOT NULL,
 PRIMARY KEY(sample_id,pass_id,metric_id));
CREATE TABLE IF NOT EXISTS summaries(
 experiment_id INTEGER NOT NULL REFERENCES experiments(id), pass_id TEXT NOT NULL, metric_id TEXT NOT NULL,
 raw_sample_count INTEGER, effective_sample_count REAL, mean REAL, standard_deviation REAL,
 coefficient_of_variation REAL, median REAL, p05 REAL, p95 REAL, confidence_low REAL,
 confidence_high REAL, relative_half_width REAL, lag1_autocorrelation REAL,
 mad_outlier_count INTEGER, converged INTEGER,
 PRIMARY KEY(experiment_id,pass_id,metric_id));
CREATE INDEX IF NOT EXISTS idx_samples_experiment_status ON samples(experiment_id,accepted,ordinal);
CREATE INDEX IF NOT EXISTS idx_measurements_metric ON measurements(pass_id,metric_id,value);
CREATE VIEW IF NOT EXISTS v_sample_measurements AS
 SELECT e.id experiment_id,e.name,s.ordinal,s.accepted,p.pass_name,p.queue,p.occurrence,
        m.metric_name,m.output_name,m.unit,x.value
 FROM measurements x JOIN samples s ON s.id=x.sample_id JOIN experiments e ON e.id=s.experiment_id
 JOIN passes p ON p.experiment_id=e.id AND p.pass_id=x.pass_id
 JOIN metrics m ON m.experiment_id=e.id AND m.metric_id=x.metric_id;
CREATE VIEW IF NOT EXISTS v_metric_summary AS
 SELECT e.name,e.started_at,e.status,p.pass_name,p.queue,p.occurrence,m.output_name,m.unit,s.*
 FROM summaries s JOIN experiments e ON e.id=s.experiment_id
 JOIN passes p ON p.experiment_id=e.id AND p.pass_id=s.pass_id
 JOIN metrics m ON m.experiment_id=e.id AND m.metric_id=s.metric_id;
CREATE VIEW IF NOT EXISTS v_latest_run_summary AS
 SELECT * FROM v_metric_summary WHERE experiment_id=(SELECT MAX(id) FROM experiments);
CREATE VIEW IF NOT EXISTS v_rejected_samples AS
 SELECT e.name,s.* FROM samples s JOIN experiments e ON e.id=s.experiment_id WHERE s.accepted=0;
)sql");
    Exec(m_impl->db, R"sql(
UPDATE experiments
SET finished_at=COALESCE(finished_at,CURRENT_TIMESTAMP),
    status='interrupted',
    stopping_reason=COALESCE(stopping_reason,'process ended before the experiment was finalized')
WHERE status='running';
)sql");
}

int64_t Database::BeginExperiment(const Configuration& configuration)
{
    Exec(m_impl->db, "BEGIN IMMEDIATE;");
    try {
        Statement experiment(m_impl->db, "INSERT INTO experiments(name,status,configuration_json) VALUES(?,'running',?);");
        Bind(experiment.get(), 1, configuration.name);
        Bind(experiment.get(), 2, configuration.rawJson);
        CheckSqlite(sqlite3_step(experiment.get()), m_impl->db, "insert experiment");
        const int64_t id = sqlite3_last_insert_rowid(m_impl->db);

        Statement pass(m_impl->db, "INSERT INTO passes VALUES(?,?,?,?,?,?);");
        for (const auto& item : configuration.passes) {
            sqlite3_bind_int64(pass.get(), 1, id);
            Bind(pass.get(), 2, item.id);
            Bind(pass.get(), 3, item.name);
            Bind(pass.get(), 4, item.queue);
            sqlite3_bind_int(pass.get(), 5, static_cast<int>(item.occurrence));
            sqlite3_bind_int(pass.get(), 6, item.required ? 1 : 0);
            CheckSqlite(sqlite3_step(pass.get()), m_impl->db, "insert pass");
            pass.reset();
        }
        Statement metric(m_impl->db, "INSERT INTO metrics VALUES(?,?,?,?,?,?,?,?);");
        for (const auto& item : configuration.metrics) {
            sqlite3_bind_int64(metric.get(), 1, id);
            Bind(metric.get(), 2, item.request.id);
            Bind(metric.get(), 3, item.request.name);
            Bind(metric.get(), 4, item.request.outputName);
            Bind(metric.get(), 5, item.request.unit);
            sqlite3_bind_int(metric.get(), 6, item.request.required ? 1 : 0);
            sqlite3_bind_double(metric.get(), 7, item.relativeHalfWidth);
            if (item.absoluteHalfWidth) sqlite3_bind_double(metric.get(), 8, *item.absoluteHalfWidth);
            else sqlite3_bind_null(metric.get(), 8);
            CheckSqlite(sqlite3_step(metric.get()), m_impl->db, "insert metric");
            metric.reset();
        }
        Exec(m_impl->db, "COMMIT;");
        return id;
    } catch (...) {
        Exec(m_impl->db, "ROLLBACK;");
        throw;
    }
}

void Database::RecordDiagnostic(int64_t experimentId, const std::string& key, const std::string& value)
{
    Statement statement(m_impl->db, "INSERT OR REPLACE INTO diagnostics VALUES(?,?,?);");
    sqlite3_bind_int64(statement.get(), 1, experimentId);
    Bind(statement.get(), 2, key);
    Bind(statement.get(), 3, value);
    CheckSqlite(sqlite3_step(statement.get()), m_impl->db, "insert diagnostic");
}

void Database::RecordSample(int64_t experimentId, const SampleRecord& sample)
{
    Exec(m_impl->db, "BEGIN IMMEDIATE;");
    try {
        Statement row(m_impl->db, R"sql(
INSERT INTO samples(experiment_id,ordinal,start_frame,end_frame,accepted,rejection_reason,
 readiness_before_json,readiness_after_json,scheduled_passes,dropped_ranges,dropped_trace_bytes,profiler_error)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?);)sql");
        sqlite3_bind_int64(row.get(), 1, experimentId);
        sqlite3_bind_int(row.get(), 2, static_cast<int>(sample.ordinal));
        sqlite3_bind_int64(row.get(), 3, static_cast<sqlite3_int64>(sample.startFrame));
        sqlite3_bind_int64(row.get(), 4, static_cast<sqlite3_int64>(sample.endFrame));
        sqlite3_bind_int(row.get(), 5, sample.accepted ? 1 : 0);
        Bind(row.get(), 6, sample.rejectionReason);
        Bind(row.get(), 7, sample.before.ToJson());
        Bind(row.get(), 8, sample.after.ToJson());
        sqlite3_bind_int64(row.get(), 9, static_cast<sqlite3_int64>(sample.capture.scheduledPasses));
        sqlite3_bind_int64(row.get(), 10, static_cast<sqlite3_int64>(sample.capture.droppedRanges));
        sqlite3_bind_int64(row.get(), 11, static_cast<sqlite3_int64>(sample.capture.droppedTraceBytes));
        Bind(row.get(), 12, sample.capture.error);
        CheckSqlite(sqlite3_step(row.get()), m_impl->db, "insert sample");
        const int64_t sampleId = sqlite3_last_insert_rowid(m_impl->db);
        Statement measurement(m_impl->db, "INSERT INTO measurements VALUES(?,?,?,?);");
        for (const auto& item : sample.measurements) {
            sqlite3_bind_int64(measurement.get(), 1, sampleId);
            Bind(measurement.get(), 2, item.passId);
            Bind(measurement.get(), 3, item.metricId);
            sqlite3_bind_double(measurement.get(), 4, item.value);
            CheckSqlite(sqlite3_step(measurement.get()), m_impl->db, "insert measurement");
            measurement.reset();
        }
        Exec(m_impl->db, "COMMIT;");
    } catch (...) {
        Exec(m_impl->db, "ROLLBACK;");
        throw;
    }
}

void Database::ReplaceSummaries(int64_t experimentId, const std::vector<Summary>& summaries)
{
    Exec(m_impl->db, "BEGIN IMMEDIATE;");
    try {
        Statement remove(m_impl->db, "DELETE FROM summaries WHERE experiment_id=?;");
        sqlite3_bind_int64(remove.get(), 1, experimentId);
        CheckSqlite(sqlite3_step(remove.get()), m_impl->db, "delete summaries");
        Statement insert(m_impl->db, "INSERT INTO summaries VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
        for (const auto& item : summaries) {
            sqlite3_bind_int64(insert.get(), 1, experimentId);
            Bind(insert.get(), 2, item.passId);
            Bind(insert.get(), 3, item.metricId);
            sqlite3_bind_int(insert.get(), 4, static_cast<int>(item.rawSampleCount));
            sqlite3_bind_double(insert.get(), 5, item.effectiveSampleCount);
            sqlite3_bind_double(insert.get(), 6, item.mean);
            sqlite3_bind_double(insert.get(), 7, item.standardDeviation);
            sqlite3_bind_double(insert.get(), 8, item.coefficientOfVariation);
            sqlite3_bind_double(insert.get(), 9, item.median);
            sqlite3_bind_double(insert.get(), 10, item.p05);
            sqlite3_bind_double(insert.get(), 11, item.p95);
            sqlite3_bind_double(insert.get(), 12, item.confidenceLow);
            sqlite3_bind_double(insert.get(), 13, item.confidenceHigh);
            sqlite3_bind_double(insert.get(), 14, item.relativeHalfWidth);
            sqlite3_bind_double(insert.get(), 15, item.lag1Autocorrelation);
            sqlite3_bind_int(insert.get(), 16, static_cast<int>(item.madOutlierCount));
            sqlite3_bind_int(insert.get(), 17, item.converged ? 1 : 0);
            CheckSqlite(sqlite3_step(insert.get()), m_impl->db, "insert summary");
            insert.reset();
        }
        Exec(m_impl->db, "COMMIT;");
    } catch (...) {
        Exec(m_impl->db, "ROLLBACK;");
        throw;
    }
}

void Database::FinishExperiment(int64_t experimentId, const std::string& status, const std::string& stoppingReason)
{
    Statement statement(m_impl->db,
        "UPDATE experiments SET finished_at=CURRENT_TIMESTAMP,status=?,stopping_reason=? WHERE id=?;");
    Bind(statement.get(), 1, status);
    Bind(statement.get(), 2, stoppingReason);
    sqlite3_bind_int64(statement.get(), 3, experimentId);
    CheckSqlite(sqlite3_step(statement.get()), m_impl->db, "finish experiment");
}

void WriteLastRunSummary(
    const Configuration& configuration,
    int64_t experimentId,
    uint64_t readinessDurationMs,
    size_t scheduledReplayPasses,
    const std::vector<SampleRecord>& samples,
    const std::vector<Summary>& summaries,
    const std::string& stoppingReason)
{
    WriteRunSummary(
        configuration,
        configuration.summaryPath,
        experimentId,
        readinessDurationMs,
        scheduledReplayPasses,
        samples,
        summaries,
        stoppingReason);
}

void WriteRunSummary(
    const Configuration& configuration,
    const std::filesystem::path& outputPath,
    int64_t experimentId,
    uint64_t readinessDurationMs,
    size_t scheduledReplayPasses,
    const std::vector<SampleRecord>& samples,
    const std::vector<Summary>& summaries,
    const std::string& stoppingReason)
{
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path());
    }
    std::ofstream output(outputPath, std::ios::trunc);
    const size_t accepted = std::ranges::count_if(samples, [](const auto& sample) { return sample.accepted; });
    output << "# " << configuration.name << "\n\n"
           << "- Experiment: " << experimentId << "\n"
           << "- Stopping reason: " << stoppingReason << "\n"
           << "- Readiness duration: " << readinessDurationMs << " ms\n"
           << "- Scheduled NVPerf replay passes per sample: " << scheduledReplayPasses << "\n"
           << "- Samples: " << samples.size() << " attempted, " << accepted << " accepted, "
           << (samples.size() - accepted) << " rejected\n\n"
           << "| Pass | Metric | Mean | " << (configuration.confidenceLevel * 100.0)
           << "% CI | Std dev | Effective n | Rel. half-width | MAD flags | Converged |\n"
           << "|---|---|---:|---:|---:|---:|---:|---:|:---:|\n";
    output << std::setprecision(7);
    for (const auto& summary : summaries) {
        const auto* pass = FindPass(configuration, summary.passId);
        const auto* metric = FindMetric(configuration, summary.metricId);
        output << "| " << (pass ? pass->name : summary.passId)
               << " | " << (metric ? metric->request.outputName : summary.metricId)
               << " | " << summary.mean
               << " | [" << summary.confidenceLow << ", " << summary.confidenceHigh << ']'
               << " | " << summary.standardDeviation
               << " | " << summary.effectiveSampleCount
               << " | " << (summary.relativeHalfWidth * 100.0) << "%"
               << " | " << summary.madOutlierCount
               << " | " << (summary.converged ? "yes" : "no") << " |\n";
    }
}

} // namespace br::telemetry::sampling
