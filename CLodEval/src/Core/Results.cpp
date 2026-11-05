#include "Core/Results.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <unordered_map>

namespace clodeval {

namespace {
	constexpr int kResultsFormatVersion = 1;

	nlohmann::json ToJson(const MetricRecord& record)
	{
		nlohmann::json json{
			{ "metric", record.id },
			{ "value", record.value },
			{ "unit", ToString(record.unit) },
			{ "better", ToString(record.better) },
			{ "analyzer", record.analyzer },
			{ "analyzer_version", record.analyzerVersion },
			{ "asset", record.asset },
			{ "mesh", record.mesh },
		};
		if (!record.tags.empty()) {
			json["tags"] = record.tags;
		}
		return json;
	}

	MetricRecord RecordFromJson(const nlohmann::json& json, const std::string& build)
	{
		MetricRecord record;
		record.id = json.at("metric").get<std::string>();
		record.value = json.at("value").get<double>();
		record.unit = ParseUnit(json.at("unit").get<std::string>());
		record.better = ParseBetter(json.at("better").get<std::string>());
		record.analyzer = json.at("analyzer").get<std::string>();
		record.analyzerVersion = json.at("analyzer_version").get<uint32_t>();
		record.asset = json.at("asset").get<std::string>();
		record.mesh = json.at("mesh").get<std::string>();
		record.build = build;
		if (const auto tags = json.find("tags"); tags != json.end()) {
			record.tags = tags->get<Tags>();
		}
		return record;
	}

	std::string CsvField(const std::string& value)
	{
		if (value.find_first_of(",\"\n") == std::string::npos) {
			return value;
		}
		std::string quoted = "\"";
		for (char character : value) {
			quoted += character;
			if (character == '"') {
				quoted += '"';
			}
		}
		return quoted + "\"";
	}

	bool BudgetMatches(const Budget& budget, const MetricRecord& record)
	{
		if (budget.metric != record.id || (!budget.asset.empty() && budget.asset != record.asset) || (!budget.mesh.empty() && budget.mesh != record.mesh)) {
			return false;
		}
		return std::ranges::all_of(budget.tags, [&](const auto& tag) {
			const auto found = record.tags.find(tag.first);
			return found != record.tags.end() && found->second == tag.second;
		});
	}

	std::string TagsToString(const Tags& tags)
	{
		std::string text;
		for (const auto& [name, value] : tags) {
			text += std::format("{}{}={}", text.empty() ? "" : " ", name, value);
		}
		return text;
	}
}

bool WriteResults(const std::filesystem::path& path, const ResultSet& results, std::string& outError)
{
	std::error_code directoryError;
	std::filesystem::create_directories(path.parent_path(), directoryError);
	std::filesystem::path temporary = path;
	temporary += L".tmp";
	{
		std::ofstream output(temporary, std::ios::trunc);
		output << nlohmann::json{
			{ "kind", "clod_eval_results" },
			{ "format_version", kResultsFormatVersion },
			{ "build", results.build },
			{ "build_key", results.buildKey },
			{ "provenance", results.provenance },
			{ "leaf_geometry", results.leafGeometry },
		}.dump() << '\n';
		for (const MetricRecord& record : results.records) {
			output << ToJson(record).dump() << '\n';
		}
		output.flush();
		if (!output) {
			outError = std::format("failed to write '{}'", temporary.string());
			return false;
		}
	}
	std::error_code renameError;
	std::filesystem::rename(temporary, path, renameError);
	if (renameError) {
		outError = std::format("failed to move results to '{}': {}", path.string(), renameError.message());
		return false;
	}
	return true;
}

bool ReadResults(const std::filesystem::path& path, ResultSet& outResults, std::string& outError)
{
	std::ifstream input(path);
	if (!input) {
		outError = std::format("cannot open '{}'", path.string());
		return false;
	}
	try {
		std::string line;
		if (!std::getline(input, line)) {
			outError = std::format("'{}' is empty", path.string());
			return false;
		}
		const auto header = nlohmann::json::parse(line);
		if (header.value("kind", "") != "clod_eval_results") {
			outError = std::format("'{}' is not a CLodEval results file", path.string());
			return false;
		}
		if (header.at("format_version").get<int>() != kResultsFormatVersion) {
			outError = std::format("'{}' has results format {} but this tool reads {}", path.string(), header.at("format_version").get<int>(), kResultsFormatVersion);
			return false;
		}
		outResults.build = header.at("build").get<std::string>();
		outResults.buildKey = header.at("build_key").get<std::string>();
		outResults.provenance = header.at("provenance");
		outResults.leafGeometry = header.at("leaf_geometry").get<std::map<std::string, std::string>>();
		while (std::getline(input, line)) {
			if (!line.empty()) {
				outResults.records.push_back(RecordFromJson(nlohmann::json::parse(line), outResults.build));
			}
		}
	}
	catch (const std::exception& error) {
		outError = std::format("'{}': {}", path.string(), error.what());
		return false;
	}
	return true;
}

bool WriteResultsCsv(const std::filesystem::path& path, const ResultSet& results, std::string& outError)
{
	std::ofstream output(path, std::ios::trunc);
	output << "build,asset,mesh,metric,tags,value,unit,better,analyzer,analyzer_version\n";
	for (const MetricRecord& record : results.records) {
		output << CsvField(results.build) << ',' << CsvField(record.asset) << ',' << CsvField(record.mesh) << ',' << CsvField(record.id) << ','
			<< CsvField(TagsToString(record.tags)) << ',' << std::format("{}", record.value) << ',' << ToString(record.unit) << ','
			<< ToString(record.better) << ',' << CsvField(record.analyzer) << ',' << record.analyzerVersion << '\n';
	}
	output.flush();
	if (!output) {
		outError = std::format("failed to write '{}'", path.string());
		return false;
	}
	return true;
}

std::vector<Budget> ParseBudgets(const nlohmann::json& budgets)
{
	std::vector<Budget> parsed;
	for (const nlohmann::json& json : budgets) {
		Budget budget;
		budget.metric = json.at("metric").get<std::string>();
		budget.asset = json.value("asset", "");
		budget.mesh = json.value("mesh", std::string(kAssetRollupMesh));
		if (const auto tags = json.find("tags"); tags != json.end()) {
			budget.tags = tags->get<Tags>();
		}
		budget.op = json.value("op", "<=");
		if (budget.op != "<=" && budget.op != ">=") {
			throw std::runtime_error(std::format("budget on '{}' has unknown op '{}'", budget.metric, budget.op));
		}
		budget.value = json.value("value", 0.0);
		budget.baselineFactor = json.value("baseline_factor", 0.0);
		if (!json.contains("value") && budget.baselineFactor == 0.0) {
			throw std::runtime_error(std::format("budget on '{}' needs a value or a baseline_factor", budget.metric));
		}
		parsed.push_back(std::move(budget));
	}
	return parsed;
}

std::vector<BudgetFailure> CheckBudgets(const ResultSet& results, const std::vector<Budget>& budgets, const ResultSet* baseline)
{
	std::vector<BudgetFailure> failures;
	std::unordered_map<std::string, const MetricRecord*> baselineRecords;
	if (baseline != nullptr) {
		for (const MetricRecord& record : baseline->records) {
			baselineRecords.emplace(record.Key(), &record);
		}
	}
	for (const Budget& budget : budgets) {
		bool matched = false;
		for (const MetricRecord& record : results.records) {
			if (!BudgetMatches(budget, record)) {
				continue;
			}
			double limit = budget.value;
			if (budget.baselineFactor != 0.0) {
				if (baseline == nullptr) {
					continue;  // a relative budget says nothing without a baseline
				}
				const auto found = baselineRecords.find(record.Key());
				if (found == baselineRecords.end()) {
					continue;
				}
				limit = found->second->value * budget.baselineFactor;
			}
			matched = true;
			const bool passes = budget.op == "<=" ? record.value <= limit : record.value >= limit;
			if (!passes) {
				failures.push_back({ std::format(
					"{} [{}] {}/{}: {} is not {} {}", record.id, TagsToString(record.tags), record.asset, record.mesh, record.value, budget.op, limit) });
			}
		}
		// A budget that matches nothing would pass forever after a rename, so that is itself a failure.
		if (!matched && (budget.baselineFactor == 0.0 || baseline != nullptr)) {
			failures.push_back({ std::format("budget on '{}' matched no metric", budget.metric) });
		}
	}
	return failures;
}

Comparison Compare(const ResultSet& a, const ResultSet& b, double threshold, const std::vector<Budget>& budgets)
{
	Comparison comparison;
	for (const auto& [mesh, hash] : a.leafGeometry) {
		const auto found = b.leafGeometry.find(mesh);
		if (found != b.leafGeometry.end() && found->second != hash) {
			comparison.notComparable.push_back(std::format("leaf geometry of {} differs ({} vs {}), so these are not builds of the same source", mesh, hash, found->second));
		}
	}

	std::unordered_map<std::string, const MetricRecord*> recordsB;
	recordsB.reserve(b.records.size());
	for (const MetricRecord& record : b.records) {
		recordsB.emplace(record.Key(), &record);
	}
	std::unordered_map<std::string, bool> seen;
	for (const MetricRecord& recordA : a.records) {
		const std::string key = recordA.Key();
		const auto found = recordsB.find(key);
		if (found == recordsB.end()) {
			comparison.onlyInA.push_back(key);
			continue;
		}
		seen[key] = true;
		const MetricRecord& recordB = *found->second;
		if (recordA.analyzerVersion != recordB.analyzerVersion) {
			comparison.notComparable.push_back(std::format(
				"{} comes from analyzer {} version {} and {}", recordA.id, recordA.analyzer, recordA.analyzerVersion, recordB.analyzerVersion));
			continue;
		}
		++comparison.compared;
		const double delta = recordB.value - recordA.value;
		const double relative = recordA.value != 0.0 ? delta / std::abs(recordA.value) : (delta != 0.0 ? 1.0 : 0.0);
		if (delta == 0.0 || std::abs(relative) < threshold) {
			continue;
		}
		MetricDifference difference{ recordA, recordB, delta, relative };
		if (recordA.better != Better::Neutral) {
			const bool lowerIsBetter = recordA.better == Better::Lower;
			difference.improved = lowerIsBetter ? delta < 0.0 : delta > 0.0;
			difference.regressed = !difference.improved;
		}
		comparison.differences.push_back(std::move(difference));
	}
	for (const MetricRecord& record : b.records) {
		std::string key = record.Key();
		if (!seen.contains(key)) {
			comparison.onlyInB.push_back(std::move(key));
		}
	}
	comparison.budgetFailures = CheckBudgets(b, budgets, &a);
	return comparison;
}

int ComparisonExitCode(const Comparison& comparison)
{
	if (!comparison.notComparable.empty()) {
		return 2;
	}
	return comparison.differences.empty() && comparison.budgetFailures.empty() ? 0 : 1;
}

nlohmann::json ToJson(const Comparison& comparison)
{
	nlohmann::json differences = nlohmann::json::array();
	for (const MetricDifference& difference : comparison.differences) {
		differences.push_back({
			{ "metric", difference.a.id },
			{ "asset", difference.a.asset },
			{ "mesh", difference.a.mesh },
			{ "tags", difference.a.tags },
			{ "a", difference.a.value },
			{ "b", difference.b.value },
			{ "delta", difference.delta },
			{ "relative", difference.relative },
			{ "unit", ToString(difference.a.unit) },
			{ "regressed", difference.regressed },
			{ "improved", difference.improved },
		});
	}
	nlohmann::json budgetFailures = nlohmann::json::array();
	for (const BudgetFailure& failure : comparison.budgetFailures) {
		budgetFailures.push_back(failure.description);
	}
	return {
		{ "exit_code", ComparisonExitCode(comparison) },
		{ "not_comparable", comparison.notComparable },
		{ "compared", comparison.compared },
		{ "differences", std::move(differences) },
		{ "only_in_a", comparison.onlyInA },
		{ "only_in_b", comparison.onlyInB },
		{ "budget_failures", std::move(budgetFailures) },
	};
}

}
