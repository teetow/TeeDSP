#pragma once

#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>

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
    TeeDspApo();
    ~TeeDspApo();

    TeeDspApo(const TeeDspApo &) = delete;
    TeeDspApo &operator=(const TeeDspApo &) = delete;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

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
    std::atomic<ULONG> m_refCount{1};

    // Format committed in LockForProcess. Read on the RT path.
    bool        m_locked = false;
    UINT32      m_channels = 0;
    UINT32      m_sampleRate = 0;
    UINT32      m_bytesPerFrame = 0;
};

} // namespace teedsp::apo
