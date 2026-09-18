// Covers the ownership discipline that MaterialManager's two domains rely on.
//
// These rules were introduced when m_materialMutationMutex (one recursive mutex
// over everything) was split into a non-recursive registry lock plus authoring
// state owned by TaskDomain::MaterialAcceptance. Breaking either rule does not
// fail loudly on its own: the first throws std::system_error out of whatever
// artifact producer happens to be running, and the second deadlocks. In the
// first live run after the split, a single violation of rule 2 failed 4,538
// material usage batches and rendered every material black, while the existing
// test suite passed, because nothing covered MaterialManager at all.
//
// MaterialManager itself needs a GPU device and cannot be constructed here, so
// what is tested is the guard the manager now routes every acquisition through.

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

#include "Managers/MaterialOwnershipGuards.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++g_failures;
}

unsigned int ViolationsDuring(const std::function<void()>& body) {
    br::materials::t_ownershipViolations = 0;
    body();
    return br::materials::t_ownershipViolations;
}

// Rule 1: the registry lock is not recursive.
void TestRecursiveAcquisitionIsReported() {
    std::mutex registry;
    const auto violations = ViolationsDuring([&] {
        br::materials::RegistryLock outer(registry);
        Check(br::materials::RegistryLockHeld(), "lock reports held while owned");
        // The real case was ReserveMaterialUsage's commit calling a public
        // entry point that re-locked. Detected rather than thrown from deep
        // inside a producer.
        outer.Unlock();
    });
    Check(violations == 0, "a single acquisition is not a violation");

    std::mutex second;
    const auto nested = ViolationsDuring([&] {
        br::materials::RegistryLock outer(registry);
        br::materials::RegistryLock inner(second);
    });
    Check(nested == 1, "acquiring the registry lock while already holding it is reported");
    Check(!br::materials::RegistryLockHeld(), "depth unwinds after both guards release");
}

// Rule 2: posted mutations are never drained while the registry lock is held,
// because the mutations take that lock themselves. This is the one that shipped.
void TestDrainUnderRegistryLockIsReported() {
    std::mutex registry;
    const auto clean = ViolationsDuring([&] {
        br::materials::RequireRegistryLockNotHeld("drain");
    });
    Check(clean == 0, "draining with no lock held is allowed");

    const auto held = ViolationsDuring([&] {
        br::materials::RegistryLock lock(registry);
        br::materials::RequireRegistryLockNotHeld("drain");
    });
    Check(held == 1, "draining posted mutations under the registry lock is reported");
}

// Authoring state must only be touched by the acceptance domain.
void TestAcceptanceDomainMarker() {
    Check(!br::materials::OnMaterialAcceptance(), "a plain thread does not own the domain");
    const auto offDomain = ViolationsDuring([] {
        br::materials::RequireAcceptanceDomain("authoring write");
    });
    Check(offDomain == 1, "an authoring write off the domain is reported");

    const auto onDomain = ViolationsDuring([] {
        br::materials::AcceptanceScope acceptance;
        br::materials::RequireAcceptanceDomain("authoring write");
    });
    Check(onDomain == 0, "an authoring write on the domain is allowed");
    Check(!br::materials::OnMaterialAcceptance(), "the marker unwinds with its scope");

    // Nesting is legitimate: the usage reservation's commit applies a row from
    // inside a task that already owns the domain.
    const auto nested = ViolationsDuring([] {
        br::materials::AcceptanceScope outer;
        {
            br::materials::AcceptanceScope inner;
            br::materials::RequireAcceptanceDomain("nested authoring write");
        }
        Check(br::materials::OnMaterialAcceptance(), "outer scope still owns the domain");
    });
    Check(nested == 0, "nested acceptance scopes are not a violation");
}

// The guards are thread_local: one thread holding the lock must not make
// another thread look like it holds it.
void TestGuardsArePerThread() {
    std::mutex registry;
    std::atomic_bool locked{ false };
    std::atomic_bool observed{ false };
    std::atomic_uint otherViolations{ 0 };

    std::thread holder([&] {
        br::materials::RegistryLock lock(registry);
        locked.store(true, std::memory_order_release);
        while (!observed.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!locked.load(std::memory_order_acquire)) std::this_thread::yield();

    std::thread other([&] {
        br::materials::t_ownershipViolations = 0;
        Check(!br::materials::RegistryLockHeld(), "another thread's lock is not seen as held");
        // Would be rule 2's deadlock if the depth were shared.
        br::materials::RequireRegistryLockNotHeld("drain on a thread holding nothing");
        otherViolations.store(br::materials::t_ownershipViolations, std::memory_order_release);
    });
    other.join();
    observed.store(true, std::memory_order_release);
    holder.join();

    Check(otherViolations.load(std::memory_order_acquire) == 0,
        "a drain on a thread that holds no lock is allowed while another thread holds it");
}

} // namespace

int main() {
    TestRecursiveAcquisitionIsReported();
    TestDrainUnderRegistryLockIsReported();
    TestAcceptanceDomainMarker();
    TestGuardsArePerThread();
    if (g_failures != 0) {
        std::fprintf(stderr, "MaterialOwnershipGuardTests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("MaterialOwnershipGuardTests: all checks passed\n");
    return 0;
}
