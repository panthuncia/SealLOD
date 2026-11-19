#include "Core/Metrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numeric>
#include <utility>

namespace clodeval {

std::string_view ToString(Unit unit)
{
	switch (unit) {
	case Unit::Count: return "count";
	case Unit::Bytes: return "bytes";
	case Unit::Ratio: return "ratio";
	case Unit::Pixels: return "pixels";
	case Unit::MeshUnits: return "mesh_units";
	case Unit::MeshArea: return "mesh_units2";
	case Unit::Milliseconds: return "ms";
	case Unit::Texels: return "texels";
	case Unit::Radians: return "radians";
	case Unit::None: return "none";
	}
	return "none";
}

std::string_view ToString(Better better)
{
	switch (better) {
	case Better::Lower: return "lower";
	case Better::Higher: return "higher";
	case Better::Neutral: return "neutral";
	}
	return "neutral";
}

Unit ParseUnit(std::string_view text)
{
	for (Unit unit : { Unit::Count, Unit::Bytes, Unit::Ratio, Unit::Pixels, Unit::MeshUnits, Unit::MeshArea, Unit::Milliseconds, Unit::Texels, Unit::Radians }) {
		if (ToString(unit) == text) {
			return unit;
		}
	}
	return Unit::None;
}

Better ParseBetter(std::string_view text)
{
	if (text == "lower") {
		return Better::Lower;
	}
	return text == "higher" ? Better::Higher : Better::Neutral;
}

std::string MetricRecord::Key() const
{
	std::string key = std::format("{}|{}|{}", id, asset, mesh);
	for (const auto& [name, value] : tags) {
		key += std::format("|{}={}", name, value);
	}
	return key;
}

LogHistogram::LogHistogram(double low, double high, uint32_t binsPerOctave)
	: m_low(low)
	, m_high(high)
	, m_binsPerOctave(static_cast<double>(std::max(binsPerOctave, 1u)))
{
	const size_t count = static_cast<size_t>(std::ceil(std::log2(high / low) * m_binsPerOctave));
	m_bins.assign(count + 2u, 0.0);
}

void LogHistogram::Add(double value, double weight)
{
	if (m_bins.empty() || !(weight > 0.0) || std::isnan(value)) {
		return;
	}
	size_t bin = 0;
	if (value >= m_high) {
		bin = m_bins.size() - 1u;
	}
	else if (value >= m_low) {
		bin = 1u + std::min(static_cast<size_t>(std::log2(value / m_low) * m_binsPerOctave), m_bins.size() - 3u);
	}
	m_bins[bin] += weight;
	m_total += weight;
}

void LogHistogram::Merge(const LogHistogram& other)
{
	if (other.Empty()) {
		return;
	}
	if (m_bins.empty()) {
		*this = other;
		return;
	}
	if (other.m_bins.size() != m_bins.size() || other.m_low != m_low || other.m_binsPerOctave != m_binsPerOctave) {
		return;
	}
	for (size_t bin = 0; bin < m_bins.size(); ++bin) {
		m_bins[bin] += other.m_bins[bin];
	}
	m_total += other.m_total;
}

void LogHistogram::Scale(double factor)
{
	for (double& bin : m_bins) {
		bin *= factor;
	}
	m_total *= factor;
}

double LogHistogram::Quantile(double fraction) const
{
	if (Empty()) {
		return 0.0;
	}
	const double target = std::clamp(fraction, 0.0, 1.0) * m_total;
	double below = 0.0;
	for (size_t bin = 0; bin < m_bins.size(); ++bin) {
		if (m_bins[bin] <= 0.0 || below + m_bins[bin] < target) {
			below += m_bins[bin];
			continue;
		}
		if (bin == 0u) {
			return 0.0;
		}
		if (bin == m_bins.size() - 1u) {
			return m_high;
		}
		const double within = std::clamp((target - below) / m_bins[bin], 0.0, 1.0);
		return std::min(m_low * std::exp2((static_cast<double>(bin - 1u) + within) / m_binsPerOctave), m_high);
	}
	return m_high;
}

MetricRecord MetricSink::MakeRecord(std::string id, double value, Unit unit, Better better, Tags tags) const
{
	MetricRecord record;
	record.id = std::move(id);
	record.value = value;
	record.unit = unit;
	record.better = better;
	record.analyzer = m_scope.analyzer;
	record.analyzerVersion = m_scope.analyzerVersion;
	record.build = m_scope.build;
	record.asset = m_scope.asset;
	record.mesh = m_scope.mesh;
	record.tags = std::move(tags);
	return record;
}

void MetricSink::Emit(std::string id, double value, Unit unit, Better better, Tags tags, Rollup rollup)
{
	if (!std::isfinite(value)) {
		m_droppedNonFinite.push_back(std::format("{} on {}/{}", id, m_scope.asset, m_scope.mesh));
		return;
	}
	MetricRecord record = MakeRecord(std::move(id), value, unit, better, std::move(tags));
	if (rollup != Rollup::None && rollup != Rollup::Ratio && rollup != Rollup::Quantiles) {
		MetricRecord rolled = record;
		rolled.mesh = std::string(kAssetRollupMesh);
		AddRollup(PendingRollup{ std::move(rolled), rollup });
	}
	m_records.push_back(std::move(record));
}

void MetricSink::EmitRollupOnly(std::string id, double value, Unit unit, Better better, Tags tags, Rollup rollup)
{
	if (!std::isfinite(value)) {
		m_droppedNonFinite.push_back(std::format("{} on {}/{}", id, m_scope.asset, m_scope.mesh));
		return;
	}
	if (rollup == Rollup::None || rollup == Rollup::Ratio || rollup == Rollup::Quantiles) {
		return;
	}
	MetricRecord record = MakeRecord(std::move(id), value, unit, better, std::move(tags));
	record.mesh = std::string(kAssetRollupMesh);
	AddRollup(PendingRollup{ std::move(record), rollup });
}

void MetricSink::EmitRatio(std::string id, double numerator, double denominator, Unit unit, Better better, Tags tags)
{
	if (!std::isfinite(numerator) || !std::isfinite(denominator)) {
		m_droppedNonFinite.push_back(std::format("{} on {}/{}", id, m_scope.asset, m_scope.mesh));
		return;
	}
	MetricRecord record = MakeRecord(std::move(id), denominator > 0.0 ? numerator / denominator : 0.0, unit, better, std::move(tags));
	record.mesh = std::string(kAssetRollupMesh);
	AddRollup(PendingRollup{ record, Rollup::Ratio, numerator, denominator });
	if (denominator > 0.0) {
		record.mesh = m_scope.mesh;
		m_records.push_back(std::move(record));
	}
}

void MetricSink::EmitRatioRollupOnly(std::string id, double numerator, double denominator, Unit unit, Better better, Tags tags)
{
	if (!std::isfinite(numerator) || !std::isfinite(denominator)) {
		m_droppedNonFinite.push_back(std::format("{} on {}/{}", id, m_scope.asset, m_scope.mesh));
		return;
	}
	MetricRecord record = MakeRecord(std::move(id), 0.0, unit, better, std::move(tags));
	record.mesh = std::string(kAssetRollupMesh);
	AddRollup(PendingRollup{ std::move(record), Rollup::Ratio, numerator, denominator });
}

namespace {
	constexpr std::array<std::pair<const char*, double>, 3> kQuantileStats{ { { "p50", 0.50 }, { "p95", 0.95 }, { "p99", 0.99 } } };
	// Tags the pending histogram rollup so its key cannot collide with a record's; Finish replaces it.
	constexpr const char* kQuantilesPending = "quantiles";
}

void MetricSink::EmitQuantiles(std::string id, const LogHistogram& histogram, Unit unit, Better better, Tags tags, bool meshRecords)
{
	if (meshRecords && !histogram.Empty()) {
		for (const auto& [stat, fraction] : kQuantileStats) {
			Tags statTags = tags;
			statTags["stat"] = stat;
			m_records.push_back(MakeRecord(id, histogram.Quantile(fraction), unit, better, std::move(statTags)));
		}
	}
	tags["stat"] = kQuantilesPending;
	MetricRecord record = MakeRecord(std::move(id), 0.0, unit, better, std::move(tags));
	record.mesh = std::string(kAssetRollupMesh);
	PendingRollup pending{ std::move(record), Rollup::Quantiles };
	pending.histogram = histogram;
	AddRollup(pending);
}

void MetricSink::AddRollup(const PendingRollup& pending)
{
	auto [entry, inserted] = m_rollups.try_emplace(pending.record.Key(), pending);
	if (inserted) {
		return;
	}
	PendingRollup& total = entry->second;
	switch (pending.rollup) {
	case Rollup::Sum: total.record.value += pending.record.value; break;
	case Rollup::Max: total.record.value = std::max(total.record.value, pending.record.value); break;
	case Rollup::Ratio:
		total.numerator += pending.numerator;
		total.denominator += pending.denominator;
		break;
	case Rollup::Quantiles: total.histogram.Merge(pending.histogram); break;
	case Rollup::None: break;
	}
}

void MetricSink::Absorb(MetricSink&& other)
{
	m_records.insert(m_records.end(), std::make_move_iterator(other.m_records.begin()), std::make_move_iterator(other.m_records.end()));
	for (auto& [key, pending] : other.m_rollups) {
		AddRollup(pending);
	}
	m_droppedNonFinite.insert(m_droppedNonFinite.end(), other.m_droppedNonFinite.begin(), other.m_droppedNonFinite.end());
	other.m_records.clear();
	other.m_rollups.clear();
	other.m_droppedNonFinite.clear();
}

std::vector<MetricRecord> MetricSink::Finish()
{
	std::vector<MetricRecord> records = std::move(m_records);
	for (auto& [key, pending] : m_rollups) {
		if (pending.rollup == Rollup::Quantiles) {
			if (pending.histogram.Empty()) {
				continue;
			}
			for (const auto& [stat, fraction] : kQuantileStats) {
				MetricRecord record = pending.record;
				record.tags["stat"] = stat;
				record.value = pending.histogram.Quantile(fraction);
				records.push_back(std::move(record));
			}
			continue;
		}
		if (pending.rollup == Rollup::Ratio) {
			if (pending.denominator <= 0.0) {
				continue;
			}
			pending.record.value = pending.numerator / pending.denominator;
		}
		records.push_back(std::move(pending.record));
	}
	m_records.clear();
	m_rollups.clear();
	return records;
}

void EmitDistribution(MetricSink& sink, const std::string& id, std::vector<double>& samples, Unit unit, Better better, Tags tags)
{
	if (samples.empty()) {
		return;
	}
	std::sort(samples.begin(), samples.end());
	const auto emit = [&](const char* stat, double value) {
		Tags statTags = tags;
		statTags["stat"] = stat;
		sink.Emit(id, value, unit, better, std::move(statTags));
	};
	const auto percentile = [&](double fraction) {
		return samples[static_cast<size_t>(fraction * static_cast<double>(samples.size() - 1u) + 0.5)];
	};
	emit("mean", std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size()));
	emit("p50", percentile(0.50));
	emit("p95", percentile(0.95));
	emit("max", samples.back());
}

}
