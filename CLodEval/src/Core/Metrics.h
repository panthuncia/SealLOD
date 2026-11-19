#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The one result format everything produces. Because a metric is plain data, comparison, budgets
// and export work on any new metric with no extra code.
namespace clodeval {

enum class Unit { Count, Bytes, Ratio, Pixels, MeshUnits, MeshArea, Milliseconds, Texels, Radians, None };
enum class Better { Lower, Higher, Neutral };

std::string_view ToString(Unit unit);
std::string_view ToString(Better better);
Unit ParseUnit(std::string_view text);
Better ParseBetter(std::string_view text);

using Tags = std::map<std::string, std::string>;

// How a per-mesh value rolls up to its asset, recorded under the mesh name "*". Ratio is only for EmitRatio: the
// rollup is the sum of numerators over the sum of denominators, so a fraction of the asset rather than a mean of meshes.
// Quantiles is only for EmitQuantiles: the meshes' histograms are added and the asset's quantiles read from the sum.
enum class Rollup { None, Sum, Max, Ratio, Quantiles };

inline constexpr std::string_view kAssetRollupMesh = "*";

// A weighted histogram of non-negative values in logarithmic bins, so weighted quantiles (area-weighted, say) can be
// rolled up across meshes exactly as far as the bins resolve. Values under `low`, zero included, fall in one bin that
// reads as 0; values over `high` in one that reads as `high`.
class LogHistogram {
public:
	LogHistogram() = default;
	LogHistogram(double low, double high, uint32_t binsPerOctave);

	void Add(double value, double weight);
	// Adds another histogram with the same layout; one that is empty or laid out differently is ignored.
	void Merge(const LogHistogram& other);
	void Scale(double factor);
	double Total() const { return m_total; }
	bool Empty() const { return !(m_total > 0.0); }
	// The value under which `fraction` of the weight lies, interpolated within its bin in log space. 0 when empty.
	double Quantile(double fraction) const;

private:
	double m_low = 1.0;
	double m_high = 1.0;
	double m_binsPerOctave = 1.0;
	std::vector<double> m_bins;  // [0] under low, [1..n] log bins, [n + 1] over high
	double m_total = 0.0;
};

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
	// numerator / denominator for this mesh, rolled up as a ratio of sums (Rollup::Ratio). Nothing is emitted for a
	// zero denominator, since the ratio has no value there; the rollup still counts the mesh's (zero) numerator.
	void EmitRatio(std::string id, double numerator, double denominator, Unit unit, Better better, Tags tags = {});
	// As Emit and EmitRatio, but into the asset rollup only, with no record for this mesh: for per-mesh values too
	// numerous to be worth a record each, such as a sweep over many cuts.
	void EmitRollupOnly(std::string id, double value, Unit unit, Better better, Tags tags, Rollup rollup);
	void EmitRatioRollupOnly(std::string id, double numerator, double denominator, Unit unit, Better better, Tags tags = {});
	// <id> with stat=p50/p95/p99 tags, read from a weighted histogram. The asset rollup adds the meshes' histograms
	// (Rollup::Quantiles), so its quantiles are the asset's, not a mean of meshes'. meshRecords=false emits only that.
	void EmitQuantiles(std::string id, const LogHistogram& histogram, Unit unit, Better better, Tags tags = {}, bool meshRecords = true);

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
		double numerator = 0.0;  // Rollup::Ratio
		double denominator = 0.0;
		LogHistogram histogram;  // Rollup::Quantiles
	};
	MetricRecord MakeRecord(std::string id, double value, Unit unit, Better better, Tags tags) const;
	void AddRollup(const PendingRollup& pending);
	std::map<std::string, PendingRollup> m_rollups;
	std::vector<std::string> m_droppedNonFinite;
};

// Emits <id> with stat=mean/p50/p95/max tags for a set of samples. Sorts the samples.
void EmitDistribution(MetricSink& sink, const std::string& id, std::vector<double>& samples, Unit unit, Better better, Tags tags = {});

}
