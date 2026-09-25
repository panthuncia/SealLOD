#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

class Skeleton;
class SkeletonManager;

namespace br::render {

// Serialized scene-ingestion operation for registering pose instances. The
// scene and mesh instances retain this narrow contract rather than borrowing
// the pose storage implementation.
class PoseInstanceRegistrationService {
public:
    void Configure(SkeletonManager* storage) noexcept;
    [[nodiscard]] bool Available() const noexcept { return m_storage != nullptr; }
    std::uint32_t Acquire(const std::shared_ptr<Skeleton>& instance) const;
    void Release(Skeleton* instance) const;
    [[nodiscard]] std::weak_ptr<std::atomic_bool> GetLifetimeToken() const noexcept;

private:
    SkeletonManager* m_storage = nullptr;
};

} // namespace br::render
