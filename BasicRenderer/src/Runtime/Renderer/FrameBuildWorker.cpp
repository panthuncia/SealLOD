#include "Runtime/Renderer/FrameBuildWorker.h"

#include <exception>
#include <stdexcept>

#include <windows.h>

#include "Runtime/Settings/ISetting.h"

namespace br::render {

FrameBuildWorker::FrameBuildWorker(Run run) : m_run(std::move(run)) {
    m_thread = std::thread([this] { Loop(); });
}

FrameBuildWorker::~FrameBuildWorker() {
    {
        std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void FrameBuildWorker::Start(std::shared_ptr<FrameBuildJob> job) {
    {
        std::scoped_lock lock(m_mutex);
        if (m_job) throw std::logic_error("FrameBuildWorker::Start: a frame build is already in hand");
        m_job = std::move(job);
        m_finished = false;
    }
    m_wake.notify_all();
}

bool FrameBuildWorker::Idle() const {
    std::scoped_lock lock(m_mutex);
    return !m_job;
}

std::shared_ptr<FrameBuildJob> FrameBuildWorker::TakeFinished() {
    std::scoped_lock lock(m_mutex);
    if (!m_job || !m_finished) return nullptr;
    m_finished = false;
    return std::move(m_job);
}

std::shared_ptr<FrameBuildJob> FrameBuildWorker::Wait() {
    std::unique_lock lock(m_mutex);
    if (!m_job) return nullptr;
    m_wake.wait(lock, [this] { return m_finished; });
    m_finished = false;
    return std::move(m_job);
}

void FrameBuildWorker::Loop() {
    SetThreadDescription(GetCurrentThread(), L"BR Frame Graph Build");
    br::settings_detail::t_buildThread = true;
    for (;;) {
        std::shared_ptr<FrameBuildJob> job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || (m_job && !m_finished); });
            if (m_job && !m_finished) job = m_job;
            else if (m_stop) return;
        }
        try {
            m_run(*job);
        } catch (const std::exception& error) {
            job->error = error.what();
        } catch (...) {
            job->error = "unknown exception";
        }
        {
            std::scoped_lock lock(m_mutex);
            m_finished = true;
        }
        m_wake.notify_all();
    }
}

} // namespace br::render
