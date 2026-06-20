#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>

#include "dsp/ProcessorChain.h"
#include "dsp/ChainParamsApply.h"
#include "dsp/LufsMeter.h"
#include "shared/TeeDspApoShared.h"

#include <atomic>

namespace teedsp::apo {

// Pass-through APO implementing the minimum interface set required for
// the Windows audio engine to load us into audiodg.exe and pipe a render
// stream through our APOProcess callback.
//
// Stage 1 deliberately does no DSP — APOProcess just memcpys input to
// output (or marks the connection silent / non-silent verbatim). This
// proves:
//   - the COM registration is valid
//   - the APO loads into the chosen endpoint's MFX slot
//   - format negotiation succeeds for whatever the endpoint hands us
//   - audio plays through unchanged
//
// Stage 3 will swap the memcpy for a dsp::ProcessorChain instance.
//
// Threading:
//   - Initialize / IsXxxFormatSupported / LockForProcess / UnlockForProcess
//     all run on a config thread inside audiodg.exe. They may allocate.
//   - APOProcess runs on the audio engine's real-time thread. It must
//     not allocate, lock, or call any blocking API.
class TeeDspApo final
    : public IAudioProcessingObjectRT
    , public IAudioProcessingObject
    , public IAudioProcessingObjectConfiguration
    , public IAudioSystemEffects   // marker the engine QIs for to recognize an sAPO
{
public:
    explicit TeeDspApo(IUnknown *pUnkOuter);
    ~TeeDspApo();

    TeeDspApo(const TeeDspApo &) = delete;
    TeeDspApo &operator=(const TeeDspApo &) = delete;

    // IUnknown (delegating) — forwards to the controlling unknown so the audio
    // engine can aggregate us. When not aggregated, the controlling unknown is
    // our own inner IUnknown, so these still behave correctly.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // Non-delegating IUnknown — the real refcount/interface implementation.
    // The aggregating outer object owns and drives these via the inner unknown.
    HRESULT NonDelegatingQueryInterface(REFIID riid, void **ppv);
    ULONG   NonDelegatingAddRef();
    ULONG   NonDelegatingRelease();

    // Inner IUnknown the class factory hands back to an aggregating creator.
    IUnknown *innerUnknown() noexcept { return &m_inner; }

    // IAudioProcessingObject
    HRESULT STDMETHODCALLTYPE Initialize(UINT32 cbDataSize, BYTE *pbyData) override;
    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(
        IAudioMediaType *pOutputFormat,
        IAudioMediaType *pRequestedInputFormat,
        IAudioMediaType **ppSupportedInputFormat) override;
    HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(
        IAudioMediaType *pInputFormat,
        IAudioMediaType *pRequestedOutputFormat,
        IAudioMediaType **ppSupportedOutputFormat) override;
    HRESULT STDMETHODCALLTYPE GetLatency(HNSTIME *pTime) override;
    HRESULT STDMETHODCALLTYPE GetRegistrationProperties(APO_REG_PROPERTIES **ppRegProps) override;
    HRESULT STDMETHODCALLTYPE Reset() override;
    HRESULT STDMETHODCALLTYPE GetInputChannelCount(UINT32 *pu32ChannelCount) override;

    // IAudioProcessingObjectConfiguration
    HRESULT STDMETHODCALLTYPE LockForProcess(
        UINT32 u32NumInputConnections,
        APO_CONNECTION_DESCRIPTOR **ppInputConnections,
        UINT32 u32NumOutputConnections,
        APO_CONNECTION_DESCRIPTOR **ppOutputConnections) override;
    HRESULT STDMETHODCALLTYPE UnlockForProcess() override;

    // IAudioProcessingObjectRT
    void  STDMETHODCALLTYPE APOProcess(
        UINT32 u32NumInputConnections,
        APO_CONNECTION_PROPERTY **ppInputConnections,
        UINT32 u32NumOutputConnections,
        APO_CONNECTION_PROPERTY **ppOutputConnections) override;
    UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 u32OutputFrameCount) override;
    UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 u32InputFrameCount) override;

private:
    void openTelemetry();
    void closeTelemetry();
    // RT path: compute in/out peak + RMS and read chain GR/leveler into the
    // shared block. inBuf/outBuf may be null (silence).
    void publishMeters(const float *inBuf, const float *outBuf, UINT32 frames);

    // Inner non-delegating IUnknown handed to an aggregating outer object.
    struct Inner final : IUnknown {
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
        ULONG   STDMETHODCALLTYPE AddRef() override;
        ULONG   STDMETHODCALLTYPE Release() override;
        TeeDspApo *owner = nullptr;
    } m_inner;

    IUnknown          *m_pOuter = nullptr;   // controlling unknown (==&m_inner if not aggregated)
    std::atomic<ULONG> m_refCount{1};

    // Format committed in LockForProcess. Read on the RT path.
    bool        m_locked = false;
    UINT32      m_channels = 0;
    UINT32      m_sampleRate = 0;
    UINT32      m_bytesPerFrame = 0;

    // The DSP chain. prepare() is called on the config thread in
    // LockForProcess; process() runs on the RT thread in APOProcess.
    dsp::ProcessorChain m_chain;

    // Post-chain loudness meter (momentary LUFS) for the UI's LUFS readouts.
    dsp::LufsMeter      m_lufs;

    // Cross-process shared block: telemetry out, params/heartbeat in.
    HANDLE              m_shmHandle = nullptr;
    teedsp::ApoShared  *m_shm = nullptr;
    bool                m_createdShared = false;

    // Live-control state (RT thread).
    uint32_t            m_lastAppliedGen = 0;       // last paramGen applied
    uint64_t            m_lastHeartbeat = 0;        // last UI heartbeat seen
    uint64_t            m_framesSinceHeartbeat = 0; // frames since it last advanced
};

} // namespace teedsp::apo
