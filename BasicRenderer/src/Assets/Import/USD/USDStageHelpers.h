#pragma once

#include "Assets/Import/USD/USDImportState.h"
#include <cctype>
#include <pxr/usd/usdShade/material.h>

namespace USDLoader {
using namespace pxr;

	namespace {

		std::string NormalizeHeuristicName(const std::string& value) {
			std::string normalized;
			normalized.reserve(value.size());
			for (unsigned char ch : value) {
				if (std::isalnum(ch)) {
					normalized.push_back(static_cast<char>(std::tolower(ch)));
				}
			}
			return normalized;
		}

		std::vector<std::string> TokenizeHeuristicName(const std::string& value) {
			std::vector<std::string> tokens;
			std::string current;
			for (unsigned char ch : value) {
				if (std::isalnum(ch)) {
					current.push_back(static_cast<char>(std::tolower(ch)));
				}
				else if (!current.empty()) {
					tokens.push_back(std::move(current));
					current.clear();
				}
			}
			if (!current.empty()) {
				tokens.push_back(std::move(current));
			}
			return tokens;
		}

		bool NameSuggestsDoubleSided(const std::string& value) {
			const std::string normalized = NormalizeHeuristicName(value);
			if (normalized.find("doublesided") != std::string::npos ||
				normalized.find("doubleside") != std::string::npos ||
				normalized.find("twosided") != std::string::npos ||
				normalized.find("twoside") != std::string::npos ||
				normalized.find("2sided") != std::string::npos ||
				normalized.find("2side") != std::string::npos) {
				return true;
			}

			const std::vector<std::string> tokens = TokenizeHeuristicName(value);
			for (size_t tokenIndex = 0; tokenIndex + 1 < tokens.size(); ++tokenIndex) {
				const std::string& first = tokens[tokenIndex];
				const std::string& second = tokens[tokenIndex + 1];
				const bool firstMatches = first == "double" || first == "two" || first == "2";
				const bool secondMatches = second == "side" || second == "sided";
				if (firstMatches && secondMatches) {
					return true;
				}
			}

			return false;
		}

		bool ShouldForceDoubleSidedByName(
			const UsdShadeMaterial& material,
			const std::optional<UsdGeomSubset>& subset,
			const ImportSettings& settings) {
			if (!settings.enableDoubleSidedNameHeuristic) {
				return false;
			}

			if (subset && NameSuggestsDoubleSided(subset->GetPrim().GetName().GetString())) {
				return true;
			}

			if (material && NameSuggestsDoubleSided(material.GetPrim().GetName().GetString())) {
				return true;
			}

			return false;
		}

	}

	static inline UsdTimeCode GetUsdGeometrySampleTime(const UsdStageRefPtr& stage) {
		if (stage && stage->HasAuthoredTimeCodeRange()) {
			return UsdTimeCode(stage->GetStartTimeCode());
		}

		return UsdTimeCode::Default();
	}

}
