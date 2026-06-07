#pragma once

#include "dsp/SpscRingBuffer.h"
#include "shared/TeeDspParams.h"
#include "shared/TeeDspTelemetry.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Forward-declare CLAP types we hold by pointer so windows.h / the full CLAP
// surface stays out of this header.
struct clap_plugin;
struct clap_plugin_entry;
struct clap_plugin_params;
struct clap_host;

namespace host {

// Minimal single-plugin, in-process CLAP host. Loads teedsp.clap and runs the
// whole TeeDSP DSP chain behind the CLAP boundary.
//
// Threading: setParam() is called from the UI thread and crosses to the audio
// thread through a lock-free SPSC queue drained in process(). Telemetry and the
// param cache are read via atomics, safe from any thread.
class ClapHost
{
public:
    ClapHost();
    ~ClapHost();

    ClapHost(const ClapHost &) = delete;
    ClapHost &operator=(const ClapHost &) = delete;

    // Loads teedsp.clap (next to the exe, then the standard CLAP dirs) and
    // instantiates the plugin. Returns false and sets error() on failure.
    bool load();
    bool isLoaded() const { return m_plugin != nullptr; }
    const std::string &error() const { return m_error; }

    // Prepare/teardown for a given stream format. maxFrames is fixed internally;
    // process() chunks larger blocks so any packet size is safe.
    bool activate(double sampleRate);
    void deactivate();

    // Audio thread. Processes `frames` of `channels`-interleaved float in place.
    void process(float *interleaved, uint32_t frames, uint32_t channels);

    // UI thread. Queues a parameter change for the audio thread and updates the
    // host-side cache used by paramValue().
    void setParam(uint32_t id, double value);
    float paramValue(uint32_t id) const;

    // Any thread. Copies the plugin's live metering snapshot.
    void readTelemetry(teedsp_telemetry_data &out) const;

private:
    bool loadFrom(const std::wstring &path);
    void unload();
    // Push the host's full cached param state into the plugin. Called on
    // (re)activation so the plugin matches the UI even though param events that
    // were queued before the first activate() are no longer in flight.
    void flushAllParams();

    // Fixed maximum block handed to the plugin in a single process() call.
    static constexpr uint32_t kMaxBlock = 4096;
    static constexpr uint32_t kMaxEventsPerBlock = 256;

    void                       *m_module = nullptr;  // HMODULE
    const clap_plugin_entry    *m_entry = nullptr;
    const clap_plugin          *m_plugin = nullptr;
    const clap_plugin_params   *m_params = nullptr;
    const teedsp_telemetry     *m_telemetry = nullptr;
    clap_host                  *m_clapHost = nullptr;

    double  m_sampleRate = 48000.0;
    bool    m_active = false;
    bool    m_processing = false;
    std::string m_error;

    // UI -> audio param events, encoded as [idAsFloat, valueAsFloat] pairs.
    dsp::SpscRingBuffer m_paramQueue;

    // Host-side last-set value per param (atomics; read on the audio thread for
    // e.g. input-trim metering).
    std::atomic<float> m_paramCache[teedsp::kParamCount];

    // Planar scratch for de-interleave/process/re-interleave (2 ch x kMaxBlock).
    std::vector<float> m_planar[2];
};

} // namespace host
