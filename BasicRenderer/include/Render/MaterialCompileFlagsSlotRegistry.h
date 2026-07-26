#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Materials/TechniqueDescriptor.h"

class MaterialCompileFlagsSlotRegistry {
public:
    struct AcquireResult {
        unsigned int slot = 0;
        bool createdSlot = false;
        bool becameActive = false;
    };

    AcquireResult Acquire(MaterialCompileFlags flags, unsigned int count = 1u)
    {
        if (count == 0u) {
            return {};
        }

        unsigned int slot = 0;
        bool createdSlot = false;
        const auto existing = m_slotMapping.find(flags);
        if (existing != m_slotMapping.end()) {
            slot = existing->second;
        } else {
            createdSlot = true;
            if (!m_freeSlots.empty()) {
                slot = m_freeSlots.back();
                m_freeSlots.pop_back();
            } else {
                slot = m_nextSlot++;
                ++m_slotsUsed;
                m_usageCounts.push_back(0u);
            }
            m_slotMapping.emplace(flags, slot);
        }

        const bool becameActive = m_usageCounts[slot] == 0u;
        m_usageCounts[slot] += count;
        if (becameActive) {
            m_activeSlots.push_back(slot);
            m_activeFlags.push_back(flags);
        }
        return { slot, createdSlot, becameActive };
    }

    bool Release(MaterialCompileFlags flags, unsigned int count = 1u)
    {
        if (count == 0u) {
            return false;
        }
        const auto existing = m_slotMapping.find(flags);
        if (existing == m_slotMapping.end()) {
            return false;
        }

        const unsigned int slot = existing->second;
        if (slot >= m_usageCounts.size() || m_usageCounts[slot] < count) {
            return false;
        }

        m_usageCounts[slot] -= count;
        if (m_usageCounts[slot] != 0u) {
            return true;
        }

        m_activeSlots.erase(
            std::remove(m_activeSlots.begin(), m_activeSlots.end(), slot),
            m_activeSlots.end());
        m_activeFlags.erase(
            std::remove(m_activeFlags.begin(), m_activeFlags.end(), flags),
            m_activeFlags.end());
        m_slotMapping.erase(existing);
        m_freeSlots.push_back(slot);
        return true;
    }

    bool TryGet(MaterialCompileFlags flags, unsigned int& slot) const
    {
        const auto existing = m_slotMapping.find(flags);
        if (existing == m_slotMapping.end()) {
            return false;
        }
        slot = existing->second;
        return true;
    }

    unsigned int GetUsageCount(MaterialCompileFlags flags) const
    {
        unsigned int slot = 0;
        return TryGet(flags, slot) && slot < m_usageCounts.size() ? m_usageCounts[slot] : 0u;
    }

    unsigned int GetSlotsUsed() const { return m_slotsUsed; }
    const std::vector<unsigned int>& GetActiveSlots() const { return m_activeSlots; }
    const std::vector<MaterialCompileFlags>& GetActiveFlags() const { return m_activeFlags; }

private:
    std::unordered_map<MaterialCompileFlags, unsigned int> m_slotMapping;
    unsigned int m_nextSlot = 1u;
    unsigned int m_slotsUsed = 1u;
    std::vector<unsigned int> m_freeSlots;
    std::vector<unsigned int> m_usageCounts = { 0u };
    std::vector<unsigned int> m_activeSlots;
    std::vector<MaterialCompileFlags> m_activeFlags;
};
