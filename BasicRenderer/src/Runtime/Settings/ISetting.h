#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#include <BasicRenderer/Extensions/SettingAccess.h>

// A copy of every setting's value, taken at one moment (SettingsManager::Snapshot). While a SettingsSnapshotScope is active
// on a thread, every getter on that thread reads the snapshot instead of the live value. A graph build running on a worker
// therefore sees one consistent set of settings, while the render thread keeps writing the live ones.
class SettingsSnapshot {
public:
    // One copied value, owned by the snapshot.
    struct Value {
        virtual ~Value() = default;
        virtual void* Get() noexcept = 0;
    };

    // The copy of the setting registered with this index, or null when the snapshot has none (registered after it was taken).
    void* Find(std::size_t index) const noexcept {
        return index < m_values.size() && m_values[index] ? m_values[index]->Get() : nullptr;
    }
    // SettingsManager::Revision when the snapshot was taken.
    uint64_t Revision() const noexcept { return m_revision; }

private:
    friend class SettingsManager;
    std::vector<std::unique_ptr<Value>> m_values;
    uint64_t m_revision = 0;
};

namespace br::settings_detail {
    // The snapshot this thread's getters read, or null for the live values.
    inline thread_local const SettingsSnapshot* t_activeSnapshot = nullptr;
    // Settings this thread wrote while a snapshot was active: the snapshot's readers would not see those writes, so a
    // graph build that writes settings is reported (Renderer::FinishFrameBuild logs them).
    inline thread_local std::vector<std::string> t_writesUnderSnapshot;
    // Nonzero while a SettingsWriteWatch is active: writes are recorded as under a snapshot, but reads stay live.
    inline thread_local int t_writeWatch = 0;
    // Set on the frame-graph build thread (FrameBuildWorker). A getter that reads a live value there (no snapshot active, or
    // a setting the snapshot has no copy of) is reported with BASICRENDERER_CHECK_BUILD_SETTINGS=1 (decision M8-D4): the
    // render thread may be writing it.
    inline thread_local bool t_buildThread = false;
    // Logs the first live read of each setting on the build thread, with a stack trace (SettingAccess.cpp).
    void ReportLiveBuildRead(std::size_t snapshotIndex);
}

// Records the settings this thread writes until the scope ends, without changing what getters read. For finding the writes
// a graph build makes before its reads move to a snapshot. The names are in br::settings_detail::t_writesUnderSnapshot.
class SettingsWriteWatch {
public:
    SettingsWriteWatch() { ++br::settings_detail::t_writeWatch; }
    ~SettingsWriteWatch() { --br::settings_detail::t_writeWatch; }
    SettingsWriteWatch(const SettingsWriteWatch&) = delete;
    SettingsWriteWatch& operator=(const SettingsWriteWatch&) = delete;
};

// Makes this thread's setting getters read `snapshot` until the scope ends. Scopes nest.
class SettingsSnapshotScope {
public:
    explicit SettingsSnapshotScope(std::shared_ptr<const SettingsSnapshot> snapshot)
        : m_snapshot(std::move(snapshot)), m_previous(br::settings_detail::t_activeSnapshot) {
        br::settings_detail::t_activeSnapshot = m_snapshot.get();
    }
    ~SettingsSnapshotScope() { br::settings_detail::t_activeSnapshot = m_previous; }
    SettingsSnapshotScope(const SettingsSnapshotScope&) = delete;
    SettingsSnapshotScope& operator=(const SettingsSnapshotScope&) = delete;

private:
    std::shared_ptr<const SettingsSnapshot> m_snapshot;
    const SettingsSnapshot* m_previous;
};

class ISetting {
public:
    virtual ~ISetting() = default;

    // Virtual method to retrieve the type of the setting
    virtual const std::type_info& getType() const = 0;

    // Function to retrieve a callable for setting the value
    virtual std::function<void(void*)> getSetter() = 0;

    // Function to retrieve a callable for getting the value. Inside a SettingsSnapshotScope it reads the snapshot's copy.
    virtual std::function<void* (void)> getGetter() = 0;

    // The live value, ignoring any snapshot scope (for change checks before a write).
    virtual void* liveValue() noexcept = 0;

    // A copy of the live value, for a snapshot.
    virtual std::unique_ptr<SettingsSnapshot::Value> copyValue() const = 0;

    // By-name access (SettingAccess.h): the value as a plain SettingValue, monostate when the type has no such form.
    virtual br::extensions::SettingValue readValue() const = 0;
    // Converts and writes it, running the observers; false, writing nothing, when the setting already holds it. Throws
    // std::runtime_error when the value does not fit.
    virtual bool writeValue(const br::extensions::SettingValue& value) = 0;
    virtual std::string typeName() const = 0;

    // The setting's slot in a snapshot. SettingsManager assigns it at registration.
    std::size_t snapshotIndex = 0;
};
