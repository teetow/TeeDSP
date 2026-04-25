#pragma once

#include <QString>

#include <atomic>
#include <functional>
#include <thread>

namespace host {

// Runs a WASAPI loopback capture thread on a render endpoint.
// Converts every incoming packet to interleaved float32 before invoking the
// callback. Callback runs on the capture thread — do not block in it.
class WasapiLoopbackCapture
{
public:
    using Callback = std::function<void(const float *interleaved,
                                        int numFrames,
                                        int numChannels,
                                        int sampleRate)>;

    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture();

    WasapiLoopbackCapture(const WasapiLoopbackCapture &) = delete;
    WasapiLoopbackCapture &operator=(const WasapiLoopbackCapture &) = delete;

    // Returns empty string on success; otherwise a human-readable error.
    QString start(const QString &renderDeviceId, Callback cb);
    void stop();

    bool isRunning() const { return m_running.load(); }

private:
    void threadMain(QString deviceId, Callback cb);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    QString m_lastError;
};

} // namespace host
