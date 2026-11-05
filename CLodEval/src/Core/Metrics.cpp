#include "Core/Metrics.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

namespace clodeval {

std::string_view ToString(Unit unit)
{
	switch (unit) {
	case Unit::Count: return "count";
	case Unit::Bytes: return "bytes";
	case Unit::Ratio: return "ratio";
	case Unit::Pixels: return "pixels";
	case Unit::MeshUnits: return "mesh_units";
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
	for (Unit unit : { Unit::Count, Unit::Bytes, Unit::Ratio, Unit::Pixels, Unit::MeshUnits, Unit::Milliseconds, Unit::Texels, Unit::Radians }) {
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

void MetricSink::Emit(std::string id, double value, Unit unit, Better better, Tags tags, Rollup rollup)
{
	if (!std::isfinite(value)) {
		m_droppedNonFinite.push_back(std::format("{} on {}/{}", id, m_scope.asset, m_scope.mesh));
		return;
	}
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

	if (rollup != Rollup::None) {
		MetricRecord rolled = record;
		rolled.mesh = std::string(kAssetRollupMesh);
		auto [entry, inserted] = m_rollups.try_emplace(rolled.Key(), PendingRollup{ rolled, rollup });
		if (!inserted) {
			double& total = entry->second.record.value;
			total = rollup == Rollup::Sum ? total + value : std::max(total, value);
		}
	}
	m_records.push_back(std::move(record));
}

std::vector<MetricRecord> MetricSink::Finish()
{
	std::vector<MetricRecord> records = std::move(m_records);
	for (auto& [key, pending] : m_rollups) {
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
