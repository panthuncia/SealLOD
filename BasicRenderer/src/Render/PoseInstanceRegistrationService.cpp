#include "Render/PoseInstanceRegistrationService.h"

#include "Managers/SkeletonManager.h"

namespace br::render {

void PoseInstanceRegistrationService::Configure(SkeletonManager* storage) noexcept {
    m_storage = storage;
}

std::uint32_t PoseInstanceRegistrationService::Acquire(
    const std::shared_ptr<Skeleton>& instance) const {
    return m_storage != nullptr ? m_storage->AcquireSkinningInstance(instance) : 0xFFFFFFFFu;
}

void PoseInstanceRegistrationService::Release(Skeleton* instance) const {
    if (m_storage != nullptr)
        m_storage->ReleaseSkinningInstance(instance);
}

std::weak_ptr<std::atomic_bool> PoseInstanceRegistrationService::GetLifetimeToken() const noexcept {
    return m_storage != nullptr
        ? m_storage->GetLifetimeToken()
        : std::weak_ptr<std::atomic_bool>{};
}

} // namespace br::render
