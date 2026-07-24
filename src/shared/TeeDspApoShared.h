#pragma once

// Cross-process shared block between the TeeDSP APO (inside audiodg.exe) and
// the TeeDSP UI in the user session.
//
//   APO  -> observers : telemetry (proves APOProcess runs; reports format)
//   UI   -> APO        : a seqlock-guarded dsp::ChainParams snapshot + heartbeat
//
// The APO (a session-0 service) is the only party that can CREATE the Global\
// section (it holds SeCreateGlobalPrivilege); the UI opens it. As long as the
// UI keeps its handle, the section persists across stream gaps.
//
// POD only — lives in a file mapping. No pointers, no virtuals. 64-bit counters
// are written with std::atomic_ref; `params` is transferred via seqlock memcpy.

#include "dsp/ChainParams.h"
#include "dsp/LevelerCalibration.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace teedsp {

inline constexpr wchar_t  kApoSharedName[]  = L"Global\\TeeDspApoSharedV10";
inline constexpr uint32_t kApoSharedMagic   = 0x50534454u; // 'TDSP'
inline constexpr uint32_t kApoSharedVersion = 10u;

// Mono pre/post sample ring for the UI spectrum analyzer. ~170 ms at 48 kHz —
// far more than the UI's drain interval, so it never underruns between ticks.
inline constexpr uint32_t kApoAudioRing = 8192u;

// Persisted params for always-on operation: the UI writes this file atomically
// on every change; the APO loads it at stream start so the chain runs with the
// last-saved settings even when the app isn't running. File = [magic u32]
// followed by a raw dsp::ChainParams. In ProgramData so the audio service
// (audiodg) can read what a normal-user UI writes.
inline constexpr wchar_t  kApoParamsPath[]  = L"C:\\ProgramData\\TeeDSP\\params.bin";
inline constexpr uint32_t kApoParamsMagic   = 0x50524454u; // 'TDRP'

struct ApoShared {
    // --- header ---
    uint32_t magic;
    uint32_t version;

    // --- telemetry: APO -> observers ---
    uint32_t channels;
    uint32_t sampleRate;
    uint32_t bytesPerFrame;
    uint32_t activeStreams;   // count of live instances (Lock..Unlock); >0 = active.
                              // An SFX is per-stream, so concurrent streams (e.g. a
                              // Zoom call + media) each get their own APO instance.
    uint64_t processCalls;    // ++ each APOProcess — cumulative across all instances
    uint64_t framesProcessed; // += valid frames each APOProcess
    uint64_t lastBufferFlags; // most recent APO_CONNECTION_PROPERTY flags
    uint32_t appliedGen;      // paramGen the APO has consumed (loop-closure proof)
    uint32_t uiAlive;         // APO's view: 1 if the UI heartbeat is fresh
    uint32_t meterOwner;      // instance id elected to write meters/ring (0 = none),
                              // so concurrent instances don't interleave the meters

    // --- control: UI -> APO ---
    uint32_t paramSeq;        // seqlock: even = stable, odd = write in progress
    uint32_t paramGen;        // UI bumps on each committed change
    uint64_t uiHeartbeat;     // UI bumps periodically; APO bypasses if stale

    // --- meters: APO -> observers (dBFS / dB) ---
    // Written each block by the single elected `meterOwner` instance; read
    // without a lock (single writer, float granularity — slight tearing is fine).
    float inPeakDbfs[2];      // pre-chain peak, per channel
    float outPeakDbfs[2];     // post-chain peak, per channel
    float outRmsDbfs;         // post-chain RMS (drives VU)
    float compGrDb;           // compressor gain reduction (positive = pulling down)
    float levelerGainDb;      // input leveler applied gain
    float spectralGainDb[10]; // per-band spectral leveler corrections, low to high
    float outLevelerGainDb;   // output leveler applied gain
    float bandGrDb[5];        // per-EQ-band dynamic gain reduction
    float outLufsCh[2];       // per-channel momentary LUFS (BS.1770, post-chain)
    float outLufsM;           // combined momentary LUFS

    // Compile-time stamp ('__DATE__ __TIME__') of whichever APO instance most
    // recently (re)initialized this section — written unconditionally by every
    // instance, not just the section's original creator, so a rebuilt DLL
    // loading into a fresh audiodg.exe always overwrites a stale value left
    // behind by an older one. Lets observers prove which code is actually
    // running right now, not just what's on disk.
    char dspBuildStamp[32];

    // Last learned calibration from the meter-owning stream. The APO is
    // instantiated per application stream, so this snapshot bridges browser
    // pause/resume cycles that destroy one instance and create another.
    uint32_t calibrationSeq;
    dsp::LevelerChainCalibration calibration;

    dsp::ChainParams params;  // the snapshot (guarded by paramSeq)

    // --- audio ring: APO -> UI spectrum analyzer (post-mix mono samples) ---
    // Written by the APO RT thread; the UI drains new samples each tick and
    // feeds its FFT. audioWritePos is the monotonic total sample count.
    uint64_t audioWritePos;
    float    inRing[kApoAudioRing];   // pre-chain mono
    float    outRing[kApoAudioRing];  // post-chain mono
};

// ---- seqlock (single writer = UI; readers = APO/observers) ----

inline void apoWriteParams(ApoShared *s, const dsp::ChainParams &p)
{
    std::atomic_ref<uint32_t> seq(s->paramSeq);
    const uint32_t v = seq.load(std::memory_order_relaxed);
    seq.store(v + 1, std::memory_order_release);          // odd: writing
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&s->params, &p, sizeof(dsp::ChainParams));
    std::atomic_thread_fence(std::memory_order_release);
    seq.store(v + 2, std::memory_order_release);          // even: done
    std::atomic_ref<uint32_t> gen(s->paramGen);
    gen.store(gen.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

inline bool apoReadParams(ApoShared *s, dsp::ChainParams &out)
{
    std::atomic_ref<uint32_t> seq(s->paramSeq);
    for (int i = 0; i < 16; ++i) {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;                            // writer mid-update
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memcpy(&out, &s->params, sizeof(dsp::ChainParams));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_acquire) == s1) return true;
    }
    return false;
}

inline void apoWriteCalibration(ApoShared *s, const dsp::LevelerChainCalibration &value)
{
    std::atomic_ref<uint32_t> seq(s->calibrationSeq);
    const uint32_t v = seq.load(std::memory_order_relaxed);
    seq.store(v + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&s->calibration, &value, sizeof(value));
    std::atomic_thread_fence(std::memory_order_release);
    seq.store(v + 2, std::memory_order_release);
}

inline bool apoReadCalibration(ApoShared *s, dsp::LevelerChainCalibration &out)
{
    std::atomic_ref<uint32_t> seq(s->calibrationSeq);
    for (int i = 0; i < 16; ++i) {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memcpy(&out, &s->calibration, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_acquire) == s1) return true;
    }
    return false;
}

} // namespace teedsp
