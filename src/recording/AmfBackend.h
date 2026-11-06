#pragma once

#include "recording/RecordingBackend.h"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace fastrecord::recording {

// Direct AMD AMF H.264/AV1 path. AMF is loaded from the installed AMD driver
// at runtime; the application never ships or links against the driver DLL.
// The encoded bitstream is muxed by Mp4Writer, the same writer used by NVENC.
class AmfBackend final : public IRecordingBackend {
public:
    AmfBackend() = default;
    ~AmfBackend() override;

    AmfBackend(const AmfBackend&) = delete;
    AmfBackend& operator=(const AmfBackend&) = delete;

    bool start(const RecordingSettings& settings, std::wstring& error) override;
    bool pause(std::wstring& error) override;
    bool resume(std::wstring& error) override;
    bool stop(std::wstring& error) override;
    std::filesystem::path outputPath() const override;
    bool running() const noexcept override { return m_running.load(); }

private:
    void recordLoop(RecordingSettings settings, std::promise<std::wstring> startupResult);
    void setWorkerError(std::wstring error);

    std::atomic_bool m_stopRequested{false};
    std::atomic_bool m_paused{false};
    std::atomic_bool m_running{false};
    mutable std::mutex m_mutex;
    std::thread m_worker;
    std::filesystem::path m_outputPath;
    std::wstring m_workerError;
};

} // namespace fastrecord::recording
