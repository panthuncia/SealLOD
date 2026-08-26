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
            slot = m_nextSlot++;
            ++m_slotsUsed;
            m_usageCounts.push_back(0u);
            m_slotMapping.emplace(flags, slot);
            // Compile-flag slots are written into GPU-visible mesh records and
            // therefore have interned, lifetime-stable meaning. A released CPU
            // owner does not prove that every published or in-flight record has
            // stopped referring to the slot.
            m_activeSlots.push_back(slot);
            m_activeFlags.push_back(flags);
        }

        const bool becameActive = createdSlot;
        m_usageCounts[slot] += count;
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
        // Preserve the mapping and evaluation dispatch for the renderer
        // lifetime. Reclamation requires a publication/fence retirement proof,
        // which this ownership counter intentionally does not provide.
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
    std::vector<unsigned int> m_usageCounts = { 0u };
    std::vector<unsigned int> m_activeSlots;
    std::vector<MaterialCompileFlags> m_activeFlags;
};
