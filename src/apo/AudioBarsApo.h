#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <BaseAudioProcessingObject.h>   // CBaseAudioProcessingObject
#include <audioenginebaseapo.h>          // IAudioSystemEffects / IAudioSystemEffects2
#include <audioengineextensionapo.h>     // IAudioSystemEffects3 / AUDIO_SYSTEMEFFECT

#include <atomic>

#include "ApoGuids.h"
#include "../dsp/ProcessorChain.h"
#include "../dsp/ApoShared.h"

// TeeDSP APO — Windows MFX APO for the system playback graph.
//
// We inherit from CBaseAudioProcessingObject for the standard
// IAudioProcessingObject/RT/Configuration plumbing (Initialize, format
// validation, lock/unlock bookkeeping) — this is what Microsoft's reference
// APOs (e.g. SwapAPO) do and what the Windows 11 audio proxy expects.
//
// On top of that we layer IAudioSystemEffects3 so the modern audio UI can
// enumerate the effect, plus our own shared-memory integration with the
// TeeDSP configuration application.
class TeeDspApo final
    : public CBaseAudioProcessingObject
    , public IAudioSystemEffects3
    , public IAudioProcessingObjectNotifications
{
public:
    static HRESULT CreateInstance(IUnknown *pOuter, REFIID riid, void **ppv);

    // IUnknown
    STDMETHODIMP         QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // CBaseAudioProcessingObject overrides we care about
    STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections,
                                 APO_CONNECTION_PROPERTY **ppInputConnections,
                                 UINT32 u32NumOutputConnections,
                                 APO_CONNECTION_PROPERTY **ppOutputConnections) override;

    STDMETHOD(LockForProcess)(UINT32 u32NumInputConnections,
                              APO_CONNECTION_DESCRIPTOR **ppInputConnections,
                              UINT32 u32NumOutputConnections,
                              APO_CONNECTION_DESCRIPTOR **ppOutputConnections) override;
    STDMETHOD(UnlockForProcess)() override;

    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE *pbyData) override;
    STDMETHOD(Reset)() override;

    // Override the base class's format support to trace + be permissive about
    // mix-stage formats Realtek / HDA drivers actually offer (int16, int24,
    // 44.1 kHz, etc. — base only accepts float32).
    STDMETHOD(IsInputFormatSupported)(IAudioMediaType *pOutputFormat,
                                      IAudioMediaType *pRequestedInputFormat,
                                      IAudioMediaType **ppSupportedInputFormat) override;
    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType *pInputFormat,
                                       IAudioMediaType *pRequestedOutputFormat,
                                       IAudioMediaType **ppSupportedOutputFormat) override;
    STDMETHOD(GetInputChannelCount)(UINT32 *pu32ChannelCount) override;

    STDMETHOD(GetLatency)(HNSTIME *pTime) override;
    STDMETHOD(GetRegistrationProperties)(APO_REG_PROPERTIES **ppRegProps) override;
    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 u32OutputFrameCount) override;
    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 u32InputFrameCount) override;

protected:
    // Override base class format-validation hook so any reasonable PCM/float
    // format is accepted (base's default only accepts float32/48k).
    HRESULT __fastcall ValidateDefaultAPOFormat(UNCOMPRESSEDAUDIOFORMAT &audioFormat,
                                                bool bIsInput) override;

    // IAudioSystemEffects / 2 / 3
    STDMETHODIMP GetEffectsList(LPGUID *ppEffectsIds,
                                UINT *pcEffects,
                                HANDLE Event) override;
    STDMETHODIMP GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT **effects,
                                                  UINT *numEffects,
                                                  HANDLE event) override;
    STDMETHODIMP SetAudioSystemEffectState(GUID effectId,
                                           AUDIO_SYSTEMEFFECT_STATE state) override;

    // IAudioProcessingObjectNotifications (Win11 — audiodg requires this,
    // even as a stub: if we return E_NOINTERFACE it silently drops us before
    // LockForProcess).
    STDMETHODIMP GetApoNotificationRegistrationInfo(
        APO_NOTIFICATION_DESCRIPTOR **apoNotifications, DWORD *count) override;
    STDMETHODIMP_(void) HandleNotification(APO_NOTIFICATION *apoNotification) override;

private:
    TeeDspApo();
    virtual ~TeeDspApo();

    void openOrCreateSharedMemory();
    void applyParamsIfChanged();

    std::atomic<ULONG>  m_refCount{1};
    UINT32              m_channels{0};
    UINT32              m_sampleRate{0};

    dsp::ProcessorChain m_chain;

    // Shared memory with TeeDSP config app
    HANDLE              m_mapHandle{nullptr};
    dsp::SharedBlock   *m_shared{nullptr};
    uint64_t            m_lastParamSeq{UINT64_MAX};
    dsp::ChainParams    m_localParams{};
};

// Exported from ApoFactory.cpp
extern long    g_lockCount;
extern HMODULE g_hModule;
void ApoTrace(const wchar_t *message);
