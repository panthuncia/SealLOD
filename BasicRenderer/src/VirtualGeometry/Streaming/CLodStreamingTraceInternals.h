#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <BasicTelemetry/Telemetry.h>

namespace CLodStreamingInternal
{
inline const std::string& CLodRequestTraceOutputPath()
{
	static const std::string path = [] {
		char* value = nullptr;
		size_t length = 0u;
		if (_dupenv_s(
				&value,
				&length,
				"SARP_CLOD_REQUEST_TRACE_OUTPUT") != 0 ||
			value == nullptr) {
			return std::string{};
		}
		std::string result(value);
		std::free(value);
		return result;
	}();
	return path;
}

inline bool CLodRequestTraceEnabled()
{
	return !CLodRequestTraceOutputPath().empty();
}

inline uint32_t CLodStagedPayloadGroupLimit()
{
	static const uint32_t limit = [] {
		uint32_t result = 1536u;
		char* value = nullptr;
		size_t length = 0u;
		if (_dupenv_s(
				&value,
				&length,
				"SARP_CLOD_STAGED_PAYLOAD_GROUP_LIMIT") == 0 &&
			value != nullptr) {
			char* end = nullptr;
			const unsigned long parsed = std::strtoul(value, &end, 10);
			if (end != value && parsed > 0u) {
				result = std::clamp<uint32_t>(
					static_cast<uint32_t>(parsed), 1u, 16384u);
			}
		}
		std::free(value);
		return result;
	}();
	return limit;
}

inline uint32_t CLodPageCreditRetryBudget()
{
	static const uint32_t budget = [] {
		uint32_t result = 2048u;
		char* value = nullptr;
		size_t length = 0u;
		if (_dupenv_s(
				&value,
				&length,
				"SARP_CLOD_PAGE_CREDIT_RETRY_BUDGET") == 0 &&
			value != nullptr) {
			char* end = nullptr;
			const unsigned long parsed = std::strtoul(value, &end, 10);
			if (end != value && parsed > 0u) {
				result = std::clamp<uint32_t>(
					static_cast<uint32_t>(parsed), 1u, 4096u);
			}
		}
		std::free(value);
		return result;
	}();
	return budget;
}

inline uint64_t CLodRequestTraceNowNs()
{
	return basic_telemetry::NowNs();
}
}

using CLodStreamingInternal::CLodPageCreditRetryBudget;
using CLodStreamingInternal::CLodRequestTraceEnabled;
using CLodStreamingInternal::CLodRequestTraceNowNs;
using CLodStreamingInternal::CLodRequestTraceOutputPath;
using CLodStreamingInternal::CLodStagedPayloadGroupLimit;
