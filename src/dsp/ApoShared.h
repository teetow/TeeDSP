#pragma once

// Shared-memory layout between the APO DLL (running in audiodg.exe, session 0)
// and the TeeDSP UI process (user session).
//
// The APO creates the named mapping with a NULL DACL so both sessions can access it.
// TeeDSP opens it read-write to push param updates and read meter samples.
//
// Mapping name: L"Global\\TeeDspApo_v1"

#include "ChainParams.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace dsp {

// How many float samples the meter ring can hold (stereo @ 48 kHz = 1 second).
// Power of two so we can mask instead of mod.
inline constexpr uint32_t kMeterRingCapacity = 65536;   // 65536 / 2ch / 48000 ≈ 0.68 s

inline constexpr wchar_t kSharedMemName[] = L"Global\\TeeDspApo_v1";
inline constexpr uint32_t kSharedMagic   = 0x54445053u; // 'TDPS'
inline constexpr uint32_t kSharedVersion = 1;

// Seqlock: the writer increments seq to an odd value, writes, then increments
// to even. Readers spin while seq is odd or changes between two loads.
struct alignas(64) ParamSeqlock {
    std::atomic<uint64_t> seq{0};
    ChainParams           params{};

    // Call from the UI thread.
    void store(const ChainParams &src) noexcept
    {
        seq.fetch_add(1, std::memory_order_relaxed); // -> odd
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&params, &src, sizeof(params));
        std::atomic_thread_fence(std::memory_order_release);
        seq.fetch_add(1, std::memory_order_release); // -> even
    }

    // Call from the APO real-time thread.
    // Returns false if nothing changed or if a write-in-progress does not
    // clear within kMaxSpins iterations (guards against a dead UI process
    // holding seq at an odd value and hanging the RT thread indefinitely).
    bool load(ChainParams &dst, uint64_t &lastSeq) const noexcept
    {
        constexpr int kMaxSpins = 64;
        for (int spin = 0; spin < kMaxSpins; ++spin) {
            const uint64_t s = seq.load(std::memory_order_acquire);
            if (s & 1u)
                continue;               // write in progress — spin
            if (s == lastSeq)
                return false;           // nothing changed
            std::memcpy(&dst, &params, sizeof(params));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s) {
                lastSeq = s;
                return true;
            }
        }
        return false; // gave up — leave lastSeq unchanged so we retry next callback
    }
};

// Lock-free SPSC ring: APO writes post-DSP samples; TeeDSP reads for UI meters.
// Indices are monotonic (never wrap in the logical sense); mask into the array.
struct alignas(64) MeterRing {
    std::atomic<uint64_t> writeIdx{0};
    std::atomic<uint64_t> readIdx{0};
    uint32_t              capacity{kMeterRingCapacity};
    std::atomic<uint32_t> channels{2};     // written only from LockForProcess (non-RT)
    std::atomic<uint32_t> sampleRate{48000};
    uint32_t              _pad{0};
    float                 samples[kMeterRingCapacity]{};

    void write(const float *interleaved, uint32_t frameCount, uint32_t ch) noexcept
    {
        // channels is set authoritatively in LockForProcess; do not write it here.
        const uint32_t total = frameCount * ch;
        const uint32_t mask  = capacity - 1u;
        uint64_t       head  = writeIdx.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < total; ++i)
            samples[(head + i) & mask] = interleaved[i];
        writeIdx.store(head + total, std::memory_order_release);
    }

    // Returns number of frames read.
    uint32_t read(float *dst, uint32_t maxFrames) noexcept
    {
        const uint32_t ch   = channels.load(std::memory_order_relaxed);
        const uint32_t mask = capacity - 1u;
        const uint64_t head = writeIdx.load(std::memory_order_acquire);
        uint64_t       tail = readIdx.load(std::memory_order_relaxed);
        const uint64_t avail = head - tail;
        const uint32_t toRead = static_cast<uint32_t>(
            std::min<uint64_t>(avail / ch, maxFrames)) * ch;
        for (uint32_t i = 0; i < toRead; ++i)
            dst[i] = samples[(tail + i) & mask];
        readIdx.store(tail + toRead, std::memory_order_release);
        return toRead / (ch ? ch : 1u);
    }
};

// The full shared block. Fits in a single 4 MB mapping with room to spare.
struct SharedBlock {
    uint32_t     magic{kSharedMagic};
    uint32_t     version{kSharedVersion};
    uint32_t     blockSize{sizeof(SharedBlock)};
    uint32_t     _reserved{0};
    ParamSeqlock paramSeqlock{};
    MeterRing    meterRing{};
    // APO writes its current gain-reduction here so the UI can display it.
    std::atomic<float> compGainReductionDb{0.0f};
};

} // namespace dsp
