#pragma once

// Timing helpers of the static import pipeline.
//
// They live in the nested `support` namespace, and the pipeline brings the ones it
// uses unqualified into br::staticimport by using-declarations, so a host that
// defines its own ElapsedUs next to a using-directive for br::staticimport does not
// collide with them.

#include <chrono>
#include <cstdint>

namespace br::staticimport::support
{
	inline std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now())
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
	}

	inline std::uint64_t ElapsedUs(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now())
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
	}

	inline std::uint64_t UsToMs(std::uint64_t value)
	{
		return (value + 999u) / 1000u;
	}
}

namespace br::staticimport
{
	using support::ElapsedUs;
	using support::UsToMs;
}
