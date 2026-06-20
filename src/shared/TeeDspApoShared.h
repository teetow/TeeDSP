#pragma once

// Cross-process shared block between the TeeDSP APO (running inside
// audiodg.exe) and observers/controllers in the user session.
//
// Stage now: APO -> observer telemetry only (proves APOProcess runs and
// reports the live format). Stage next: a seqlock-guarded dsp::ChainParams
// region (UI -> APO) is appended below the telemetry fields so the existing
// UI can drive the system-wide chain.
//
// POD only — it lives in a file mapping shared across processes. No pointers,
// no virtuals. 64-bit counters are written with Interlocked* on the RT thread.

#include <cstdint>

namespace teedsp {

// Versioned name so an older reader never misinterprets a newer layout.
inline constexpr wchar_t  kApoSharedName[]  = L"Global\\TeeDspApoSharedV1";
inline constexpr uint32_t kApoSharedMagic   = 0x50534454u; // 'TDSP'
inline constexpr uint32_t kApoSharedVersion = 1u;

struct ApoShared {
    uint32_t magic;          // kApoSharedMagic once initialised
    uint32_t version;        // kApoSharedVersion
    uint32_t channels;       // committed in LockForProcess
    uint32_t sampleRate;     // committed in LockForProcess
    uint32_t bytesPerFrame;  // committed in LockForProcess
    uint32_t locked;         // 1 between LockForProcess / UnlockForProcess
    uint64_t processCalls;   // ++ each APOProcess (Interlocked)
    uint64_t framesProcessed;// += valid frames each APOProcess (Interlocked)
    uint64_t lastBufferFlags;// most recent APO_CONNECTION_PROPERTY flags seen
};

} // namespace teedsp
