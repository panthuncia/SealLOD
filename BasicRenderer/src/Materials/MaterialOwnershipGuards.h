#pragma once

#include <mutex>

#include <spdlog/spdlog.h>

// Ownership discipline for MaterialManager's two domains.
//
// MaterialManager splits its state in two. The slot registry (ID->slot mapping,
// free list, usage counts, compile-flag slots, raster buckets) is guarded by a
// plain, non-recursive mutex, because import workers need an answer
// synchronously. Everything else is authoring state owned exclusively by
// TaskDomain::MaterialAcceptance, which has limit 1, and takes no lock at all;
// off-domain writers post a mutation that the domain applies later.
//
// Two rules make that safe, and both were broken by hand at least once:
//
//  1. The registry lock is never acquired recursively. It replaced a
//     std::recursive_mutex, so any path that used to nest is now a
//     "resource deadlock would occur" exception thrown out of whatever producer
//     happened to be running.
//  2. Posted mutations are never drained while the registry lock is held,
//     because the mutations themselves take it (a posted first use flushes the
//     material, which allocates a slot).
//
// Rule 2 has no natural symptom: breaking it either deadlocks or, with a
// checking mutex, throws far from the cause. These guards make both rules
// report themselves at the point of violation, and are cheap enough to leave on
// in release builds.
namespace br::materials {

// Depth of registry-lock acquisition on this thread. Zero means not held.
inline thread_local unsigned int t_registryLockDepth = 0;
// True while this thread runs a task on TaskDomain::MaterialAcceptance.
inline thread_local bool t_onMaterialAcceptance = false;

[[nodiscard]] inline bool RegistryLockHeld() noexcept { return t_registryLockDepth != 0; }
[[nodiscard]] inline bool OnMaterialAcceptance() noexcept { return t_onMaterialAcceptance; }

// Set by the guards when they detect a violation, so a test can observe one
// without relying on an abort.
inline thread_local unsigned int t_ownershipViolations = 0;

inline void ReportOwnershipViolation(const char* what) {
    ++t_ownershipViolations;
    spdlog::error("MaterialManager ownership violation: {}", what);
}

// RAII guard for the registry mutex that refuses to nest.
class RegistryLock {
public:
    explicit RegistryLock(std::mutex& mutex) : m_mutex(&mutex) {
        if (RegistryLockHeld()) {
            // Locking here would throw std::system_error from deep inside an
            // artifact producer; say what actually happened instead.
            ReportOwnershipViolation(
                "registry lock acquired recursively; call the *Locked overload instead");
        }
        m_mutex->lock();
        m_owns = true;
        ++t_registryLockDepth;
    }
    ~RegistryLock() { Unlock(); }
    RegistryLock(const RegistryLock&) = delete;
    RegistryLock& operator=(const RegistryLock&) = delete;

    void Unlock() noexcept {
        if (!m_owns) return;
        m_owns = false;
        if (t_registryLockDepth != 0) --t_registryLockDepth;
        m_mutex->unlock();
    }

private:
    std::mutex* m_mutex = nullptr;
    bool m_owns = false;
};

// Marks the calling thread as the owner of the material acceptance domain.
class AcceptanceScope {
public:
    AcceptanceScope() : m_previous(t_onMaterialAcceptance) { t_onMaterialAcceptance = true; }
    ~AcceptanceScope() { t_onMaterialAcceptance = m_previous; }
    AcceptanceScope(const AcceptanceScope&) = delete;
    AcceptanceScope& operator=(const AcceptanceScope&) = delete;

private:
    bool m_previous;
};

// Checked at every point that applies authoring state.
inline void RequireAcceptanceDomain(const char* what) {
    if (OnMaterialAcceptance()) return;
    ReportOwnershipViolation(what);
}

// Checked where posted mutations are applied: they take the registry lock, so
// draining while it is held is rule 2's deadlock.
inline void RequireRegistryLockNotHeld(const char* what) {
    if (!RegistryLockHeld()) return;
    ReportOwnershipViolation(what);
}

} // namespace br::materials
