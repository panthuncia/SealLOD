#pragma once

// Generation-checked handles (BRScriptHandle): index + 1 in the low 32 bits, generation in the high 32. A removed slot's
// generation advances, so an old handle never reaches the slot's next occupant. 0 is never a handle.

#include <cstdint>
#include <optional>
#include <vector>

#include "BasicRendererScripting/ScriptAbi.h"

namespace br::scripting::detail {

    template <typename T>
    class HandleTable {
    public:
        BRScriptHandle Add(T value)
        {
            uint32_t index;
            if (!m_free.empty()) {
                index = m_free.back();
                m_free.pop_back();
            } else {
                index = static_cast<uint32_t>(m_slots.size());
                m_slots.emplace_back();
            }
            m_slots[index].value.emplace(std::move(value));
            return MakeHandle(index, m_slots[index].generation);
        }

        T* Find(BRScriptHandle handle)
        {
            Slot* slot = SlotOf(handle);
            return slot ? &*slot->value : nullptr;
        }

        bool Remove(BRScriptHandle handle)
        {
            Slot* slot = SlotOf(handle);
            if (!slot) return false;
            slot->value.reset();
            ++slot->generation;
            m_free.push_back(IndexOf(handle));
            return true;
        }

        // fn(BRScriptHandle, T&) for every live entry, in slot order. fn may not add or remove.
        template <typename Fn>
        void ForEach(Fn&& fn)
        {
            for (uint32_t index = 0; index < m_slots.size(); ++index) {
                auto& slot = m_slots[index];
                if (slot.value) fn(MakeHandle(index, slot.generation), *slot.value);
            }
        }

        size_t Size() const { return m_slots.size() - m_free.size(); }

    private:
        struct Slot {
            uint32_t generation = 1;
            std::optional<T> value;
        };

        static BRScriptHandle MakeHandle(uint32_t index, uint32_t generation)
        {
            return (static_cast<uint64_t>(generation) << 32) | static_cast<uint64_t>(index + 1);
        }
        static uint32_t IndexOf(BRScriptHandle handle) { return static_cast<uint32_t>(handle & 0xFFFFFFFFu) - 1; }

        Slot* SlotOf(BRScriptHandle handle)
        {
            if ((handle & 0xFFFFFFFFu) == 0) return nullptr;
            const uint32_t index = IndexOf(handle);
            if (index >= m_slots.size()) return nullptr;
            auto& slot = m_slots[index];
            if (!slot.value || slot.generation != static_cast<uint32_t>(handle >> 32)) return nullptr;
            return &slot;
        }

        std::vector<Slot> m_slots;
        std::vector<uint32_t> m_free;
    };

} // namespace br::scripting::detail
