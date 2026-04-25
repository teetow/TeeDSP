#pragma once

#include <QString>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace host {

// Event-driven shared-mode WASAPI renderer. Another thread pushes interleaved
// float samples via write(); the render thread drains them into WASAPI buffers
// whenever the engine requests more audio.
class WasapiRender
{
public:
    WasapiRender();
    ~WasapiRender();

    WasapiRender(const WasapiRender &) = delete;
    WasapiRender &operator=(const WasapiRender &) = delete;

    // Returns empty string on success, otherwise human-readable error.
    // On success the negotiated format is available via channels()/sampleRate().
    QString start(const QString &renderDeviceId);
    void stop();

    bool isRunning() const { return m_running.load(); }

    int channels() const { return m_channels.load(); }
    int sampleRate() const { return m_sampleRate.load(); }
    bool isFloat() const { return m_isFloat.load(); }

    // Producer: push interleaved float samples. Excess samples beyond the
    // internal buffer's capacity are dropped (clean degradation under load).
    void write(const float *interleaved, int numFrames, int numChannels);

private:
    void threadMain(QString deviceId);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    std::atomic<int> m_channels{0};
    std::atomic<int> m_sampleRate{0};
    std::atomic<bool> m_isFloat{false};

    // Ring buffer of interleaved samples (not frames). Protected by m_mutex.
    // Keep this simple for MVP; if latency/perf demands, swap for an SPSC ring.
    std::mutex m_mutex;
    std::vector<float> m_ring;
    size_t m_writePos = 0;
    size_t m_readPos = 0;
    size_t m_available = 0; // number of samples held
};

} // namespace host
