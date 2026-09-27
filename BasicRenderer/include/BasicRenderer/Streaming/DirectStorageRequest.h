#pragma once

#include <memory>

namespace br {

class DirectStorageManager;

class DirectStorageAsyncRequestHandle {
public:
    DirectStorageAsyncRequestHandle() = default;

    bool IsValid() const noexcept;

private:
    struct State;

    explicit DirectStorageAsyncRequestHandle(std::shared_ptr<State> state);

    std::shared_ptr<State> m_state;

    friend class DirectStorageManager;
};


} // namespace br

using DirectStorageAsyncRequestHandle = br::DirectStorageAsyncRequestHandle;
