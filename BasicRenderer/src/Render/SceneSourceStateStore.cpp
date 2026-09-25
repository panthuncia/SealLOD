#include "Render/SceneSourceStateStore.h"

#include <stdexcept>

namespace br::render {

SceneSourceStateStore::WriteLease::WriteLease(std::unique_lock<std::mutex> lock,
    flecs::world& world, const PhaseMap& phases, std::uint64_t revision,
    std::uint64_t fingerprint, std::uint64_t* committedRevision,
    std::uint64_t* committedFingerprint, bool replay) noexcept
    : m_lock(std::move(lock)), m_world(&world), m_phases(&phases), m_revision(revision),
      m_fingerprint(fingerprint), m_committedRevision(committedRevision),
      m_committedFingerprint(committedFingerprint), m_replay(replay) {}

void SceneSourceStateStore::WriteLease::Commit() {
    if (m_replay || m_committed || !m_committedRevision || !m_committedFingerprint) return;
    *m_committedRevision = m_revision;
    *m_committedFingerprint = m_fingerprint;
    m_committed = true;
}

void SceneSourceStateStore::Configure(flecs::world& world, const PhaseMap& phases) noexcept {
    std::lock_guard lock(m_mutex);
    m_world = &world;
    m_phases = &phases;
    m_lastRevision = 0;
    m_lastFingerprint = 0;
}

void SceneSourceStateStore::Reset() noexcept {
    std::lock_guard lock(m_mutex);
    m_world = nullptr;
    m_phases = nullptr;
    m_lastRevision = 0;
    m_lastFingerprint = 0;
}

bool SceneSourceStateStore::Available() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_world && m_phases;
}

SceneSourceStateStore::WriteLease SceneSourceStateStore::AcquireWrite() {
    std::unique_lock lock(m_mutex);
    if (!m_world || !m_phases) throw std::logic_error("SceneSourceStateStore is not configured");
    return WriteLease(std::move(lock), *m_world, *m_phases, m_lastRevision,
        0, nullptr, nullptr, false);
}

SceneSourceStateStore::WriteLease SceneSourceStateStore::BeginBatch(
    std::uint64_t sourceRevision, std::uint64_t fingerprint) {
    std::unique_lock lock(m_mutex);
    if (!m_world || !m_phases) throw std::logic_error("SceneSourceStateStore is not configured");
    const bool externallyVersioned = sourceRevision != 0;
    if (!externallyVersioned) sourceRevision = m_lastRevision + 1;
    if (sourceRevision != 0 && sourceRevision < m_lastRevision) {
        throw std::logic_error("SceneSourceStateStore rejected an out-of-order ingestion batch");
    }
    const bool replay = externallyVersioned && sourceRevision == m_lastRevision;
    if (replay && fingerprint != m_lastFingerprint) {
        throw std::logic_error("SceneSourceStateStore rejected conflicting reuse of a committed revision");
    }
    return WriteLease(std::move(lock), *m_world, *m_phases, sourceRevision,
        fingerprint, &m_lastRevision, &m_lastFingerprint, replay);
}

}
