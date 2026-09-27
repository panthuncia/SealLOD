#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace budget {

struct ProportionalBudgetItem {
    std::string_view id;
    uint64_t idealBytes = 0u;
    uint64_t minBytes = 0u;
    uint64_t maxBytes = (std::numeric_limits<uint64_t>::max)();
    uint64_t quantumBytes = 1u;
};

struct ProportionalBudgetAllocation {
    std::string_view id;
    uint64_t idealBytes = 0u;
    uint64_t allocatedBytes = 0u;
};

struct ProportionalBudgetResult {
    std::vector<ProportionalBudgetAllocation> allocations;
    uint64_t requestedTotalBytes = 0u;
    uint64_t allocatedTotalBytes = 0u;
    uint64_t minimumTotalBytes = 0u;
    bool fitsRequestedBudget = true;
    bool satisfiesMinimumBudget = true;

    const ProportionalBudgetAllocation& Find(std::string_view id) const
    {
        const auto it = std::find_if(
            allocations.begin(),
            allocations.end(),
            [id](const ProportionalBudgetAllocation& allocation) {
                return allocation.id == id;
            });
        if (it == allocations.end()) {
            throw std::runtime_error("Requested proportional budget allocation was not found.");
        }

        return *it;
    }
};

namespace detail {

inline uint64_t RoundDownToQuantum(uint64_t value, uint64_t quantum)
{
    return quantum == 0u ? value : (value / quantum) * quantum;
}

inline uint64_t RoundUpToQuantum(uint64_t value, uint64_t quantum)
{
    if (value == 0u || quantum == 0u) {
        return value;
    }

    const uint64_t remainder = value % quantum;
    return remainder == 0u ? value : value + (quantum - remainder);
}

} // namespace detail

inline ProportionalBudgetResult AllocateProportionalBudget(
    const std::vector<ProportionalBudgetItem>& items,
    uint64_t totalBudgetBytes)
{
    struct NormalizedItem {
        std::string_view id;
        uint64_t idealBytes = 0u;
        uint64_t minBytes = 0u;
        uint64_t quantumBytes = 1u;
        uint64_t allocatedBytes = 0u;
        long double targetExtraBytes = 0.0L;
    };

    std::vector<NormalizedItem> normalizedItems;
    normalizedItems.reserve(items.size());

    ProportionalBudgetResult result;
    for (const ProportionalBudgetItem& item : items) {
        const uint64_t quantumBytes = item.quantumBytes == 0u ? 1u : item.quantumBytes;
        const uint64_t minBytes = detail::RoundUpToQuantum(item.minBytes, quantumBytes);
        const uint64_t maxBytes = item.maxBytes == (std::numeric_limits<uint64_t>::max)()
            ? item.maxBytes
            : std::max(minBytes, detail::RoundDownToQuantum(item.maxBytes, quantumBytes));
        const uint64_t idealBytes = std::clamp(
            detail::RoundDownToQuantum(item.idealBytes, quantumBytes),
            minBytes,
            maxBytes);

        normalizedItems.push_back(NormalizedItem{
            .id = item.id,
            .idealBytes = idealBytes,
            .minBytes = minBytes,
            .quantumBytes = quantumBytes,
            .allocatedBytes = minBytes,
        });

        result.requestedTotalBytes += idealBytes;
        result.minimumTotalBytes += minBytes;
    }

    if (totalBudgetBytes >= result.requestedTotalBytes) {
        result.allocatedTotalBytes = result.requestedTotalBytes;
        result.allocations.reserve(normalizedItems.size());
        for (const NormalizedItem& item : normalizedItems) {
            result.allocations.push_back(ProportionalBudgetAllocation{
                .id = item.id,
                .idealBytes = item.idealBytes,
                .allocatedBytes = item.idealBytes,
            });
        }
        return result;
    }

    result.satisfiesMinimumBudget = totalBudgetBytes >= result.minimumTotalBytes;
    if (!result.satisfiesMinimumBudget) {
        result.allocatedTotalBytes = result.minimumTotalBytes;
        result.fitsRequestedBudget = false;
        result.allocations.reserve(normalizedItems.size());
        for (const NormalizedItem& item : normalizedItems) {
            result.allocations.push_back(ProportionalBudgetAllocation{
                .id = item.id,
                .idealBytes = item.idealBytes,
                .allocatedBytes = item.minBytes,
            });
        }
        return result;
    }

    uint64_t totalHeadroomBytes = 0u;
    for (const NormalizedItem& item : normalizedItems) {
        totalHeadroomBytes += item.idealBytes - item.minBytes;
    }

    const uint64_t distributableBytes = totalBudgetBytes - result.minimumTotalBytes;
    uint64_t remainingBudgetBytes = distributableBytes;

    if (distributableBytes > 0u && totalHeadroomBytes > 0u) {
        for (NormalizedItem& item : normalizedItems) {
            const uint64_t headroomBytes = item.idealBytes - item.minBytes;
            if (headroomBytes == 0u) {
                continue;
            }

            const long double targetExtraBytes =
                (static_cast<long double>(distributableBytes) * static_cast<long double>(headroomBytes)) /
                static_cast<long double>(totalHeadroomBytes);
            const uint64_t roundedExtraBytes = std::min(
                headroomBytes,
                detail::RoundDownToQuantum(static_cast<uint64_t>(targetExtraBytes), item.quantumBytes));
            item.targetExtraBytes = targetExtraBytes;
            item.allocatedBytes += roundedExtraBytes;
            remainingBudgetBytes -= roundedExtraBytes;
        }

        while (remainingBudgetBytes > 0u) {
            NormalizedItem* bestItem = nullptr;
            long double bestScore = -1.0L;
            for (NormalizedItem& item : normalizedItems) {
                const uint64_t stepBytes = item.quantumBytes;
                if (stepBytes > remainingBudgetBytes) {
                    continue;
                }
                if (item.allocatedBytes + stepBytes > item.idealBytes) {
                    continue;
                }

                const long double allocatedExtraBytes = static_cast<long double>(item.allocatedBytes - item.minBytes);
                const long double missingExtraBytes = item.targetExtraBytes - allocatedExtraBytes;
                const long double score = missingExtraBytes / static_cast<long double>(stepBytes);
                if (score > bestScore) {
                    bestScore = score;
                    bestItem = &item;
                }
            }

            if (!bestItem) {
                break;
            }

            bestItem->allocatedBytes += bestItem->quantumBytes;
            remainingBudgetBytes -= bestItem->quantumBytes;
        }
    }

    result.allocatedTotalBytes = 0u;
    result.fitsRequestedBudget = false;
    result.allocations.reserve(normalizedItems.size());
    for (const NormalizedItem& item : normalizedItems) {
        result.allocatedTotalBytes += item.allocatedBytes;
        result.allocations.push_back(ProportionalBudgetAllocation{
            .id = item.id,
            .idealBytes = item.idealBytes,
            .allocatedBytes = item.allocatedBytes,
        });
    }

    return result;
}

} // namespace budget