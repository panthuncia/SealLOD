#pragma once

#include <cstdint>
#include <string_view>

namespace br::telemetry::timing {

// Setting SARP_TIMING_REPORT_OUTPUT enables collection and writes a bounded
// percentile report at normal process exit. Disabled calls avoid clocks,
// locks, and allocation.
[[nodiscard]] bool Enabled() noexcept;
[[nodiscard]] uint64_t NowNs() noexcept;
void Record(std::string_view name, uint64_t elapsedNs);
void AddCounter(std::string_view name, uint64_t value = 1u);
void SetGauge(std::string_view name, uint64_t value);
void MaxGauge(std::string_view name, uint64_t value);
void WriteReport();

class Scope {
public:
    explicit Scope(std::string_view name) noexcept
        : m_name(name)
        , m_startNs(Enabled() ? NowNs() : 0u) {}

    ~Scope() {
        if (m_startNs != 0u) {
            Record(m_name, NowNs() - m_startNs);
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    std::string_view m_name;
    uint64_t m_startNs;
};

} // namespace br::telemetry::timing

#define BR_TIMING_JOIN_IMPL(a, b) a##b
#define BR_TIMING_JOIN(a, b) BR_TIMING_JOIN_IMPL(a, b)
#define BR_TIMING_SCOPE(name) \
    ::br::telemetry::timing::Scope BR_TIMING_JOIN(_brTimingScope, __LINE__)(name)
