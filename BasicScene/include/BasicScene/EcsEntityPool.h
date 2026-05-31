#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <flecs.h>

namespace br::ecs {

struct EcsEntityPoolStats {
    std::uint64_t created{ 0 };
    std::uint64_t acquired{ 0 };
    std::uint64_t released{ 0 };
    std::uint64_t reused{ 0 };
    std::uint64_t trimmed{ 0 };
    std::uint64_t free{ 0 };
};

class EcsEntityPool {
public:
    EcsEntityPool() = default;

    void Attach(flecs::world& world, const char* rootName)
    {
        m_world = &world;
        m_root = world.entity(rootName ? rootName : "ECS Entity Pool");
        m_free.clear();
        m_freeIds.clear();
        m_stats = {};
    }

    bool IsAttached() const
    {
        return m_world != nullptr && m_root.is_alive();
    }

    void Reserve(std::size_t count)
    {
        EnsureAttached();
        while (m_free.size() < count) {
            auto entity = CreateShell();
            if (!m_freeIds.insert(entity.id()).second) {
                throw std::runtime_error("EcsEntityPool reserve duplicate entity id");
            }
            m_free.push_back(entity);
        }
        m_stats.free = m_free.size();
    }

    flecs::entity Acquire()
    {
        EnsureAttached();
        ++m_stats.acquired;

        while (!m_free.empty()) {
            auto entity = m_free.back();
            m_free.pop_back();
            m_freeIds.erase(entity.id());
            if (entity.is_alive()) {
                ++m_stats.reused;
                m_stats.free = m_free.size();
                return entity;
            }
        }

        m_stats.free = 0;
        return CreateShell();
    }

    void Release(flecs::entity entity, const std::function<void(flecs::entity)>& cleanup)
    {
        EnsureAttached();
        if (!entity || !entity.is_alive()) {
            return;
        }

        if (cleanup) {
            cleanup(entity);
        }

        if (!m_freeIds.insert(entity.id()).second) {
            throw std::runtime_error("EcsEntityPool double release");
        }
        entity.child_of(m_root);
        m_free.push_back(entity);
        ++m_stats.released;
        m_stats.free = m_free.size();
    }

    void Trim(std::size_t maxFree)
    {
        EnsureAttached();
        while (m_free.size() > maxFree) {
            auto entity = m_free.back();
            m_free.pop_back();
            m_freeIds.erase(entity.id());
            if (entity.is_alive()) {
                entity.destruct();
                ++m_stats.trimmed;
            }
        }
        m_stats.free = m_free.size();
    }

    void Clear()
    {
        for (auto entity : m_free) {
            if (entity.is_alive()) {
                entity.destruct();
            }
        }
        m_free.clear();
        m_freeIds.clear();
        m_root = {};
        m_world = nullptr;
        m_stats.free = 0;
    }

    EcsEntityPoolStats GetStats() const
    {
        auto stats = m_stats;
        stats.free = m_free.size();
        return stats;
    }

private:
    flecs::entity CreateShell()
    {
        auto entity = m_world->entity();
        entity.child_of(m_root);
        ++m_stats.created;
        if (m_freeIds.contains(entity.id())) {
            throw std::runtime_error("EcsEntityPool created duplicate entity id");
        }
        return entity;
    }

    void EnsureAttached() const
    {
        if (!IsAttached()) {
            throw std::runtime_error("EcsEntityPool used before Attach");
        }
    }

    flecs::world* m_world{ nullptr };
    flecs::entity m_root{};
    std::vector<flecs::entity> m_free;
    std::unordered_set<flecs::entity_t> m_freeIds;
    EcsEntityPoolStats m_stats{};
};

} // namespace br::ecs
