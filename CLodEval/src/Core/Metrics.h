#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The one result format everything produces. Because a metric is plain data, comparison, budgets
// and export work on any new metric with no extra code.
namespace clodeval {

enum class Unit { Count, Bytes, Ratio, Pixels, MeshUnits, Milliseconds, Texels, Radians, None };
enum class Better { Lower, Higher, Neutral };

std::string_view ToString(Unit unit);
std::string_view ToString(Better better);
Unit ParseUnit(std::string_view text);
Better ParseBetter(std::string_view text);

using Tags = std::map<std::string, std::string>;

// How a per-mesh value rolls up to its asset, recorded under the mesh name "*".
enum class Rollup { None, Sum, Max };

inline constexpr std::string_view kAssetRollupMesh = "*";

struct MetricRecord {
	std::string id;
	double value = 0.0;
	Unit unit = Unit::None;
	Better better = Better::Neutral;
	std::string analyzer;
	uint32_t analyzerVersion = 0;
	std::string build;
	std::string asset;
	std::string mesh;
	Tags tags;

	// What identifies "the same measurement" in two result files. The build is left out on purpose.
	std::string Key() const;
};

class MetricSink {
public:
	struct Scope {
		std::string analyzer;
		uint32_t analyzerVersion = 0;
		std::string build;
		std::string asset;
		std::string mesh;
	};

	void SetScope(Scope scope) { m_scope = std::move(scope); }

	void Emit(std::string id, double value, Unit unit, Better better, Tags tags = {}, Rollup rollup = Rollup::None);

	// A non-finite value is meaningless to compare, so it is reported and dropped rather than stored.
	const std::vector<std::string>& DroppedNonFinite() const { return m_droppedNonFinite; }

	// Takes another sink's records and pending rollups, as if they had been emitted here. Lets meshes be measured in
	// parallel, each into its own sink, and joined in a fixed order.
	void Absorb(MetricSink&& other);

	// Records so far, with the asset rollups appended.
	std::vector<MetricRecord> Finish();

private:
	Scope m_scope;
	std::vector<MetricRecord> m_records;
	struct PendingRollup {
		MetricRecord record;
		Rollup rollup = Rollup::None;
	};
	std::map<std::string, PendingRollup> m_rollups;
	std::vector<std::string> m_droppedNonFinite;
};

// Emits <id> with stat=mean/p50/p95/max tags for a set of samples. Sorts the samples.
void EmitDistribution(MetricSink& sink, const std::string& id, std::vector<double>& samples, Unit unit, Better better, Tags tags = {});

}
