#pragma once

// User-session client for the APO shared block. The UI opens the Global\
// section the APO created (the UI cannot create it — that needs the service's
// SeCreateGlobalPrivilege), writes its dsp::ChainParams snapshot, and pulses a
// heartbeat so the APO knows the UI is alive (and bypasses when it isn't).
//
// Holding the mapping open keeps the section alive across audio-stream gaps, so
// the next APO instance attaches to it and preserves the UI's params.

#include "shared/TeeDspApoShared.h"

#include <vector>

namespace host {

class ApoSharedClient {
public:
    ApoSharedClient() = default;
    ~ApoSharedClient();
    ApoSharedClient(const ApoSharedClient &) = delete;
    ApoSharedClient &operator=(const ApoSharedClient &) = delete;

    bool isOpen() const { return m_shm != nullptr; }

    // Snapshot of the APO's live state, for the UI status line.
    struct ApoStatus {
        bool     open = false;        // the shared section is mapped
        uint32_t locked = 0;          // APO is bound into an active stream
        uint32_t channels = 0;
        uint32_t sampleRate = 0;
        uint32_t bytesPerFrame = 0;
        uint32_t uiAlive = 0;         // APO sees our heartbeat
        unsigned long long processCalls = 0; // climbing => actively processing
        unsigned long long framesProcessed = 0;
        unsigned long long lastBufferFlags = 0; // most recent APO_CONNECTION_PROPERTY flags
        uint32_t paramGen = 0;        // UI's committed-change counter
        uint32_t appliedGen = 0;      // paramGen the APO has actually consumed
        uint32_t meterOwner = 0;      // instance id elected to write meters (0 = none)
        unsigned long long uiHeartbeat = 0; // raw heartbeat counter (diagnostics only)
        // Compile-time stamp of the DSP code the currently-loaded APO instance
        // was built from (see TeeDspApoShared.h); empty until the APO writes it.
        char dspBuildStamp[32] = {};
    };
    bool readStatus(ApoStatus &out) const;  // returns out.open

    // Live meters published by the APO (dBFS / dB). Defaults are silence / no GR.
    struct ApoMeters {
        float inPeakDbfs[2]  = { -120.0f, -120.0f };
        float outPeakDbfs[2] = { -120.0f, -120.0f };
        float outRmsDbfs     = -120.0f;
        float compGrDb       = 0.0f;
        float levelerGainDb  = 0.0f;
        float spectralGainDb[4] = { 0, 0, 0, 0 };
        float outLevelerGainDb = 0.0f;
        float bandGrDb[5]    = { 0, 0, 0, 0, 0 };
        float outLufsCh[2]   = { -120.0f, -120.0f };
        float outLufsM       = -120.0f;
    };
    bool readMeters(ApoMeters &out) const;  // false if the section isn't open

    // Drain mono pre/post samples published by the APO since the last call, in
    // order, for the spectrum analyzer. Clears the outputs if nothing is open.
    void drainAudio(std::vector<float> &pre, std::vector<float> &post);

    // Try to open + validate the section. No-op (returns true) if already open;
    // returns false if the section doesn't exist yet (no audio has hit the
    // APO endpoint since boot) — call again later.
    bool tryOpen();
    void close();

    void writeParams(const dsp::ChainParams &p);
    void heartbeat();

private:
    void              *m_handle = nullptr;  // HANDLE
    teedsp::ApoShared *m_shm = nullptr;
    unsigned long long m_audioReadPos = 0;  // spectrum drain cursor
};

} // namespace host
