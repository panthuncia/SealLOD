#pragma once

// Windows
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// C++ Standard Library
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// Third-party - spdlog (included in 84 TUs, ~12s build cost)
#include <spdlog/spdlog.h>

// DirectX
#include <DirectXMath.h>

// Stable headers most translation units parse (Clang -ftime-trace: tools/build_time_report.py in SARP). Headers under
// active edit (OpenRenderGraph, BasicRenderer) stay out: a change to one would rebuild every translation unit.
#include <boost/container_hash/hash.hpp>
#include <flecs.h>
#include <rhi.h>
#include <rhi_helpers.h>
