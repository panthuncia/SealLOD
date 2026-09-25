#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <flecs.h>

#include "Render/RenderPhase.h"

namespace br::render {

// Renderer-scoped mutation boundary for the ECS-backed ingestion adapter.
// Publications never retain this store; it exists only while converting one
// ordered source batch into artifact requests.
class SceneSourceStateStore {
public:
    using PhaseMap = std::unordered_map<RenderPhase, flecs::entity, RenderPhase::Hasher>;

    class WriteLease {
    public:
        WriteLease(std::unique_lock<std::mutex> lock, flecs::world& world,
            const PhaseMap& phases, std::uint64_t revision,
            std::uint64_t fingerprint, std::uint64_t* committedRevision,
            std::uint64_t* committedFingerprint, bool replay) noexcept;
        WriteLease(WriteLease&&) noexcept = default;
        WriteLease& operator=(WriteLease&&) noexcept = default;
        WriteLease(const WriteLease&) = delete;
        WriteLease& operator=(const WriteLease&) = delete;
        [[nodiscard]] flecs::world& World() const noexcept { return *m_world; }
        [[nodiscard]] const PhaseMap& Phases() const noexcept { return *m_phases; }
        [[nodiscard]] std::uint64_t Revision() const noexcept { return m_revision; }
        [[nodiscard]] bool IsReplay() const noexcept { return m_replay; }
        void Commit();
    private:
        std::unique_lock<std::mutex> m_lock;
        flecs::world* m_world = nullptr;
        const PhaseMap* m_phases = nullptr;
        std::uint64_t m_revision = 0;
        std::uint64_t m_fingerprint = 0;
        std::uint64_t* m_committedRevision = nullptr;
        std::uint64_t* m_committedFingerprint = nullptr;
        bool m_replay = false;
        bool m_committed = false;
    };

    void Configure(flecs::world& world, const PhaseMap& phases) noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool Available() const noexcept;
    // Unversioned access for owner-thread queries and maintenance operations.
    [[nodiscard]] WriteLease AcquireWrite();
    // Opens an ingestion transaction. A zero external revision is assigned the
    // next internal monotonic revision. The caller commits only after source
    // mutations and artifact intents have all been accepted.
    [[nodiscard]] WriteLease BeginBatch(std::uint64_t sourceRevision,
        std::uint64_t fingerprint);

private:
    mutable std::mutex m_mutex;
    flecs::world* m_world = nullptr;
    const PhaseMap* m_phases = nullptr;
    std::uint64_t m_lastRevision = 0;
    std::uint64_t m_lastFingerprint = 0;
};

}
