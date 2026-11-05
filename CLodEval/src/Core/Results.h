#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/Metrics.h"

namespace clodeval {

// A results file is JSON lines: one header, then one object per metric.
struct ResultSet {
	std::string build;
	std::string buildKey;
	nlohmann::json provenance = nlohmann::json::object();
	// mesh key ("asset|mesh") to leaf geometry hash. Two builds whose leaves differ are different sources.
	std::map<std::string, std::string> leafGeometry;
	std::vector<MetricRecord> records;
};

bool WriteResults(const std::filesystem::path& path, const ResultSet& results, std::string& outError);
bool ReadResults(const std::filesystem::path& path, ResultSet& outResults, std::string& outError);
bool WriteResultsCsv(const std::filesystem::path& path, const ResultSet& results, std::string& outError);

struct Budget {
	std::string metric;
	std::string asset;  // empty: any
	std::string mesh = std::string(kAssetRollupMesh);  // empty: any
	Tags tags;
	std::string op = "<=";  // <=, >=
	double value = 0.0;
	double baselineFactor = 0.0;  // nonzero: compare against baseline value * factor instead of value
};

std::vector<Budget> ParseBudgets(const nlohmann::json& budgets);

struct MetricDifference {
	MetricRecord a;
	MetricRecord b;
	double delta = 0.0;
	double relative = 0.0;  // delta / |a|; 0 when a is 0
	bool regressed = false;
	bool improved = false;
};

struct BudgetFailure {
	std::string description;
};

struct Comparison {
	std::vector<std::string> notComparable;  // non-empty means the numbers should not be trusted
	std::vector<MetricDifference> differences;
	std::vector<std::string> onlyInA;
	std::vector<std::string> onlyInB;
	std::vector<BudgetFailure> budgetFailures;
	size_t compared = 0;
};

// threshold: a relative change smaller than this does not count as a difference.
Comparison Compare(const ResultSet& a, const ResultSet& b, double threshold, const std::vector<Budget>& budgets);

// Budgets applied to one result set, with no baseline.
std::vector<BudgetFailure> CheckBudgets(const ResultSet& results, const std::vector<Budget>& budgets, const ResultSet* baseline);

// Exit codes follow sarp_compare_captures.py: 0 match, 1 differs or over budget, 2 not comparable.
int ComparisonExitCode(const Comparison& comparison);

nlohmann::json ToJson(const Comparison& comparison);

}
