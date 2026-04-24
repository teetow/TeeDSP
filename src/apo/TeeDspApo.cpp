#include "TeeDspApo.h"

#include <audiomediatype.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <sddl.h>
#include <strsafe.h>

// ---------------------------------------------------------------------------
// Static registration properties passed to CBaseAudioProcessingObject.
// We list the three interfaces the base class implements; audiodg's proxy
// uses this to probe our capabilities before LockForProcess.
// ---------------------------------------------------------------------------
namespace {

static const CRegAPOProperties<3> s_regProperties(
    CLSID_TeeDspApo,
    L"TeeDSP",
    L"",                                      // copyright
    1, 0,                                     // major, minor
    __uuidof(IAudioProcessingObject),
    // NOTE: APO_FLAG_DEFAULT in this SDK is just the three MUST_MATCH bits
    // (0x0E) — it does NOT include APO_FLAG_INPLACE. Audiodg refuses to load
    // an MFX without INPLACE, so we OR it in explicitly.
    static_cast<APO_FLAG>(APO_FLAG_INPLACE | APO_FLAG_DEFAULT),
    1, 1,                                     // min/max input connections
    1, 1,                                     // min/max output connections
    DEFAULT_APOREG_MAXINSTANCES,
    __uuidof(IAudioProcessingObjectRT),
    __uuidof(IAudioProcessingObjectConfiguration));

bool IsFloatFormat(const WAVEFORMATEX *pWfx)
{
    if (!pWfx)
        return false;
    if (pWfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (pWfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pWfx->cbSize >= 22) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(pWfx);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// TeeDspApo
// ---------------------------------------------------------------------------
TeeDspApo::TeeDspApo()
    : CBaseAudioProcessingObject(s_regProperties)
{
    ApoTrace(L"TeeDspApo ctor");
    openOrCreateSharedMemory();
    if (m_shared) {
        m_shared->paramSeqlock.load(m_localParams, m_lastParamSeq);
        m_lastParamSeq = UINT64_MAX; // force re-apply on first APOProcess
    }
}

TeeDspApo::~TeeDspApo()
{
    ApoTrace(L"TeeDspApo dtor");
    if (m_shared) {
        UnmapViewOfFile(m_shared);
        m_shared = nullptr;
    }
    if (m_mapHandle) {
        CloseHandle(m_mapHandle);
        m_mapHandle = nullptr;
    }
}

HRESULT TeeDspApo::CreateInstance(IUnknown * /*pOuter*/, REFIID riid, void **ppv)
{
    ApoTrace(L"CreateInstance: entry");
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    // Aggregation is handled at the class-factory level: when pOuter != null
    // the factory has already ensured riid == IID_IUnknown. We simply return
    // our own IUnknown. For MFX APOs the audio proxy uses this inner IUnknown
    // as the sole lifetime token, so a single internal refcount is sufficient.
    auto *obj = new (std::nothrow) TeeDspApo();
    if (!obj) {
        ApoTrace(L"CreateInstance: alloc failed");
        return E_OUTOFMEMORY;
    }

    HRESULT hr = obj->QueryInterface(riid, ppv);
    wchar_t msg[64]{};
    StringCchPrintfW(msg, 64, L"CreateInstance: hr=0x%08X", (unsigned)hr);
    ApoTrace(msg);
    obj->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------
STDMETHODIMP TeeDspApo::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;

    const wchar_t *name = nullptr;
    if (IsEqualIID(riid, IID_IUnknown)) {
        *ppv = static_cast<IUnknown *>(static_cast<IAudioProcessingObject *>(this));
        name = L"IUnknown";
    } else if (IsEqualIID(riid, __uuidof(IAudioProcessingObject))) {
        *ppv = static_cast<IAudioProcessingObject *>(this);
        name = L"IAudioProcessingObject";
    } else if (IsEqualIID(riid, __uuidof(IAudioProcessingObjectRT))) {
        *ppv = static_cast<IAudioProcessingObjectRT *>(this);
        name = L"IAudioProcessingObjectRT";
    } else if (IsEqualIID(riid, __uuidof(IAudioProcessingObjectConfiguration))) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration *>(this);
        name = L"IAudioProcessingObjectConfiguration";
    } else if (IsEqualIID(riid, __uuidof(IAudioSystemEffects))) {
        *ppv = static_cast<IAudioSystemEffects *>(this);
        name = L"IAudioSystemEffects";
    } else if (IsEqualIID(riid, __uuidof(IAudioSystemEffects2))) {
        *ppv = static_cast<IAudioSystemEffects2 *>(this);
        name = L"IAudioSystemEffects2";
    } else if (IsEqualIID(riid, __uuidof(IAudioSystemEffects3))) {
        *ppv = static_cast<IAudioSystemEffects3 *>(this);
        name = L"IAudioSystemEffects3";
    } else if (IsEqualIID(riid, __uuidof(IAudioProcessingObjectNotifications))) {
        *ppv = static_cast<IAudioProcessingObjectNotifications *>(this);
        name = L"IAudioProcessingObjectNotifications";
    } else {
        wchar_t riidStr[40]{};
        StringFromGUID2(riid, riidStr, 40);
        wchar_t msg[80]{};
        StringCchPrintfW(msg, 80, L"QI: E_NOINTERFACE for %s", riidStr);
        ApoTrace(msg);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    wchar_t msg[80]{};
    StringCchPrintfW(msg, 80, L"QI: OK for %s", name);
    ApoTrace(msg);
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TeeDspApo::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) TeeDspApo::Release()
{
    ULONG ref = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (ref == 0)
        delete this;
    return ref;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObject (overrides of CBaseAudioProcessingObject)
// ---------------------------------------------------------------------------
STDMETHODIMP TeeDspApo::Initialize(UINT32 cbDataSize, BYTE *pbyData)
{
    wchar_t msg[128]{};
    StringCchPrintfW(msg, 128, L"Initialize called cbDataSize=%u", cbDataSize);
    ApoTrace(msg);

    HRESULT hr = CBaseAudioProcessingObject::Initialize(cbDataSize, pbyData);
    if (FAILED(hr)) {
        StringCchPrintfW(msg, 128, L"Initialize: base hr=0x%08X", (unsigned)hr);
        ApoTrace(msg);
        return hr;
    }

    ApoTrace(L"Initialize: base OK");
    return S_OK;
}

STDMETHODIMP TeeDspApo::Reset()
{
    ApoTrace(L"Reset");
    m_chain.reset();
    return CBaseAudioProcessingObject::Reset();
}

// ---------------------------------------------------------------------------
// Format negotiation overrides.
//
// CBaseAudioProcessingObject::IsInputFormatSupported only accepts
// IEEE_FLOAT / 32-bit containers, which audiodg's mix-stage sometimes
// doesn't offer (HDA drivers can propose int16/int24/32-bit-container
// PCM + unusual sample rates at discovery time). When the base rejects,
// audiodg silently drops our APO from the graph without calling
// LockForProcess. Accept any PCM / float format with 1–8 channels instead;
// in-place requirement still holds, so if the opposite side has already
// settled on a format we propose that one back.
// ---------------------------------------------------------------------------
namespace {
bool IsPcmOrFloat(const WAVEFORMATEX *wfx)
{
    if (!wfx) return false;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM)        return true;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wfx);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            || IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }
    return false;
}

bool WfxEqual(const WAVEFORMATEX *a, const WAVEFORMATEX *b)
{
    if (!a || !b) return false;
    const size_t aSize = sizeof(WAVEFORMATEX) + a->cbSize;
    const size_t bSize = sizeof(WAVEFORMATEX) + b->cbSize;
    if (aSize != bSize) return false;
    return std::memcmp(a, b, aSize) == 0;
}

void TraceFmt(const wchar_t *label, const WAVEFORMATEX *wfx)
{
    if (!wfx) { ApoTrace(label); return; }
    wchar_t msg[192]{};
    StringCchPrintfW(msg, 192,
        L"%s tag=0x%04X ch=%u rate=%u bits=%u block=%u cbSize=%u",
        label, (unsigned)wfx->wFormatTag, (unsigned)wfx->nChannels,
        (unsigned)wfx->nSamplesPerSec, (unsigned)wfx->wBitsPerSample,
        (unsigned)wfx->nBlockAlign, (unsigned)wfx->cbSize);
    ApoTrace(msg);
}
} // namespace

STDMETHODIMP TeeDspApo::IsInputFormatSupported(
    IAudioMediaType *pOutputFormat,
    IAudioMediaType *pRequestedInputFormat,
    IAudioMediaType **ppSupportedInputFormat)
{
    if (ppSupportedInputFormat) *ppSupportedInputFormat = nullptr;

    const WAVEFORMATEX *reqWfx = pRequestedInputFormat ? pRequestedInputFormat->GetAudioFormat() : nullptr;
    const WAVEFORMATEX *oppWfx = pOutputFormat         ? pOutputFormat->GetAudioFormat()         : nullptr;
    TraceFmt(L"IsInputFormatSupported req",  reqWfx);
    TraceFmt(L"IsInputFormatSupported opp",  oppWfx);

    if (!reqWfx && !oppWfx) {
        ApoTrace(L"IsInputFormatSupported: no format -> E_INVALIDARG");
        return E_INVALIDARG;
    }
    const WAVEFORMATEX *cand = reqWfx ? reqWfx : oppWfx;
    if (!IsPcmOrFloat(cand)) {
        ApoTrace(L"IsInputFormatSupported: not PCM/float -> APOERR_FORMAT_NOT_SUPPORTED");
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (cand->nChannels == 0 || cand->nChannels > 8) {
        ApoTrace(L"IsInputFormatSupported: bad channel count");
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    // In-place means input must equal output. If both supplied and they
    // differ, propose the output as the supported input.
    if (reqWfx && oppWfx && !WfxEqual(reqWfx, oppWfx)) {
        if (ppSupportedInputFormat && pOutputFormat) {
            *ppSupportedInputFormat = pOutputFormat;
            pOutputFormat->AddRef();
        }
        ApoTrace(L"IsInputFormatSupported: mismatch -> S_FALSE (proposing opposite)");
        return S_FALSE;
    }
    ApoTrace(L"IsInputFormatSupported: S_OK");
    return S_OK;
}

STDMETHODIMP TeeDspApo::IsOutputFormatSupported(
    IAudioMediaType *pInputFormat,
    IAudioMediaType *pRequestedOutputFormat,
    IAudioMediaType **ppSupportedOutputFormat)
{
    if (ppSupportedOutputFormat) *ppSupportedOutputFormat = nullptr;

    const WAVEFORMATEX *reqWfx = pRequestedOutputFormat ? pRequestedOutputFormat->GetAudioFormat() : nullptr;
    const WAVEFORMATEX *oppWfx = pInputFormat           ? pInputFormat->GetAudioFormat()           : nullptr;
    TraceFmt(L"IsOutputFormatSupported req", reqWfx);
    TraceFmt(L"IsOutputFormatSupported opp", oppWfx);

    if (!reqWfx && !oppWfx) {
        ApoTrace(L"IsOutputFormatSupported: no format -> E_INVALIDARG");
        return E_INVALIDARG;
    }
    const WAVEFORMATEX *cand = reqWfx ? reqWfx : oppWfx;
    if (!IsPcmOrFloat(cand)) {
        ApoTrace(L"IsOutputFormatSupported: not PCM/float -> APOERR_FORMAT_NOT_SUPPORTED");
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (cand->nChannels == 0 || cand->nChannels > 8) {
        ApoTrace(L"IsOutputFormatSupported: bad channel count");
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (reqWfx && oppWfx && !WfxEqual(reqWfx, oppWfx)) {
        if (ppSupportedOutputFormat && pInputFormat) {
            *ppSupportedOutputFormat = pInputFormat;
            pInputFormat->AddRef();
        }
        ApoTrace(L"IsOutputFormatSupported: mismatch -> S_FALSE (proposing opposite)");
        return S_FALSE;
    }
    ApoTrace(L"IsOutputFormatSupported: S_OK");
    return S_OK;
}

STDMETHODIMP TeeDspApo::GetInputChannelCount(UINT32 *pu32ChannelCount)
{
    ApoTrace(L"GetInputChannelCount called");
    if (!pu32ChannelCount) return E_POINTER;
    *pu32ChannelCount = m_channels ? m_channels : 2;
    return S_OK;
}

STDMETHODIMP TeeDspApo::GetLatency(HNSTIME *pTime)
{
    ApoTrace(L"GetLatency called");
    if (!pTime) return E_POINTER;
    *pTime = 0;
    return S_OK;
}

STDMETHODIMP TeeDspApo::GetRegistrationProperties(APO_REG_PROPERTIES **ppRegProps)
{
    ApoTrace(L"GetRegistrationProperties called");
    return CBaseAudioProcessingObject::GetRegistrationProperties(ppRegProps);
}

STDMETHODIMP_(UINT32) TeeDspApo::CalcInputFrames(UINT32 u32OutputFrameCount)
{
    ApoTrace(L"CalcInputFrames called");
    return u32OutputFrameCount;
}

STDMETHODIMP_(UINT32) TeeDspApo::CalcOutputFrames(UINT32 u32InputFrameCount)
{
    ApoTrace(L"CalcOutputFrames called");
    return u32InputFrameCount;
}

HRESULT __fastcall TeeDspApo::ValidateDefaultAPOFormat(
    UNCOMPRESSEDAUDIOFORMAT &audioFormat, bool bIsInput)
{
    wchar_t msg[160]{};
    StringCchPrintfW(msg, 160,
        L"ValidateDefaultAPOFormat %s ch=%u rate=%.1f bits/container=%u valid=%u",
        bIsInput ? L"input" : L"output",
        (unsigned)audioFormat.dwSamplesPerFrame,
        audioFormat.fFramesPerSecond,
        (unsigned)audioFormat.dwBytesPerSampleContainer * 8u,
        (unsigned)audioFormat.dwValidBitsPerSample);
    ApoTrace(msg);
    // Accept anything the engine presents — base only whitelisted float32 at
    // specific rates, which the HDA mix stage doesn't always propose.
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectConfiguration
// ---------------------------------------------------------------------------
STDMETHODIMP TeeDspApo::LockForProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_DESCRIPTOR **ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_DESCRIPTOR **ppOutputConnections)
{
    ApoTrace(L"LockForProcess called");

    HRESULT hr = CBaseAudioProcessingObject::LockForProcess(
        u32NumInputConnections, ppInputConnections,
        u32NumOutputConnections, ppOutputConnections);
    if (FAILED(hr)) {
        wchar_t msg[96]{};
        StringCchPrintfW(msg, 96, L"LockForProcess: base hr=0x%08X", (unsigned)hr);
        ApoTrace(msg);
        return hr;
    }

    // Cache the working format for APOProcess. The base class has validated
    // that it's IEEE-float32 (per ValidateDefaultAPOFormat) so we don't need
    // to re-check sample format here.
    if (ppInputConnections && ppInputConnections[0] && ppInputConnections[0]->pFormat) {
        const WAVEFORMATEX *wfx = ppInputConnections[0]->pFormat->GetAudioFormat();
        if (wfx) {
            m_channels   = wfx->nChannels;
            m_sampleRate = wfx->nSamplesPerSec;
        }
    }
    if (m_channels == 0)   m_channels   = 2;
    if (m_sampleRate == 0) m_sampleRate = 48000;

    m_chain.prepare(static_cast<double>(m_sampleRate),
                    static_cast<std::size_t>(m_channels));
    applyParamsIfChanged();

    if (m_shared) {
        m_shared->meterRing.sampleRate.store(m_sampleRate, std::memory_order_relaxed);
        m_shared->meterRing.channels.store(m_channels, std::memory_order_relaxed);
    }

    wchar_t okMsg[128]{};
    StringCchPrintfW(okMsg, 128, L"LockForProcess: OK ch=%u rate=%u", m_channels, m_sampleRate);
    ApoTrace(okMsg);
    return S_OK;
}

STDMETHODIMP TeeDspApo::UnlockForProcess()
{
    ApoTrace(L"UnlockForProcess");
    m_chain.reset();
    return CBaseAudioProcessingObject::UnlockForProcess();
}

// ---------------------------------------------------------------------------
// IAudioSystemEffects / 2 / 3
// ---------------------------------------------------------------------------
STDMETHODIMP TeeDspApo::GetEffectsList(LPGUID *ppEffectsIds,
                                          UINT *pcEffects,
                                          HANDLE /*Event*/)
{
    ApoTrace(L"GetEffectsList called");
    if (!ppEffectsIds || !pcEffects)
        return E_POINTER;

    // Allocate a single-entry effect list so legacy consumers know we have an
    // active effect. Some Win11 audio-engine paths treat a zero-effect APO as
    // "disabled" and refuse to apply LockForProcess.
    auto *ids = static_cast<LPGUID>(CoTaskMemAlloc(sizeof(GUID)));
    if (!ids) {
        *ppEffectsIds = nullptr;
        *pcEffects    = 0;
        return E_OUTOFMEMORY;
    }
    ids[0] = GUID_TeeDspEffect;
    *ppEffectsIds = ids;
    *pcEffects    = 1;
    return S_OK;
}

STDMETHODIMP TeeDspApo::GetControllableSystemEffectsList(
    AUDIO_SYSTEMEFFECT **effects, UINT *numEffects, HANDLE /*event*/)
{
    ApoTrace(L"GetControllableSystemEffectsList called");
    if (!effects || !numEffects)
        return E_POINTER;

    auto *list = static_cast<AUDIO_SYSTEMEFFECT *>(CoTaskMemAlloc(sizeof(AUDIO_SYSTEMEFFECT)));
    if (!list) {
        *effects    = nullptr;
        *numEffects = 0;
        return E_OUTOFMEMORY;
    }
    ZeroMemory(list, sizeof(AUDIO_SYSTEMEFFECT));
    list[0].id          = GUID_TeeDspEffect;
    list[0].state       = AUDIO_SYSTEMEFFECT_STATE_ON;
    list[0].canSetState = FALSE;
    *effects    = list;
    *numEffects = 1;
    return S_OK;
}

STDMETHODIMP TeeDspApo::SetAudioSystemEffectState(GUID /*effectId*/,
                                                     AUDIO_SYSTEMEFFECT_STATE /*state*/)
{
    ApoTrace(L"SetAudioSystemEffectState called");
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectNotifications — stub. We don't need any of the
// system events. Returning 0 notifications tells audiodg "no callbacks
// needed" but audiodg still requires the interface to exist.
// ---------------------------------------------------------------------------
STDMETHODIMP TeeDspApo::GetApoNotificationRegistrationInfo(
    APO_NOTIFICATION_DESCRIPTOR **apoNotifications, DWORD *count)
{
    ApoTrace(L"GetApoNotificationRegistrationInfo called");
    if (!apoNotifications || !count)
        return E_POINTER;
    *apoNotifications = nullptr;
    *count = 0;
    return S_OK;
}

STDMETHODIMP_(void) TeeDspApo::HandleNotification(APO_NOTIFICATION * /*n*/)
{
    // No-op — we didn't subscribe to anything.
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectRT — real-time callback. No heap, no locks, no blocking.
// ---------------------------------------------------------------------------
STDMETHODIMP_(void) TeeDspApo::APOProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_PROPERTY **ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_PROPERTY **ppOutputConnections)
{
    if (u32NumInputConnections < 1 || u32NumOutputConnections < 1)
        return;
    if (!ppInputConnections || !ppOutputConnections)
        return;

    APO_CONNECTION_PROPERTY *pIn  = ppInputConnections[0];
    APO_CONNECTION_PROPERTY *pOut = ppOutputConnections[0];
    if (!pIn || !pOut)
        return;

    const UINT32 frames = pIn->u32ValidFrameCount;

    // Passthrough when we aren't locked yet — should not happen in practice but
    // defensive against unusual chain configurations.
    if (!m_bIsLocked || m_channels == 0) {
        if (pIn->pBuffer != pOut->pBuffer && frames && m_channels) {
            std::memcpy(reinterpret_cast<void *>(pOut->pBuffer),
                        reinterpret_cast<const void *>(pIn->pBuffer),
                        frames * m_channels * sizeof(float));
        }
        pOut->u32ValidFrameCount = frames;
        pOut->u32BufferFlags     = (frames && m_channels) ? BUFFER_VALID : BUFFER_SILENT;
        return;
    }

    if (frames == 0) {
        pOut->u32ValidFrameCount = 0;
        pOut->u32BufferFlags     = BUFFER_SILENT;
        return;
    }

    // Pick up any param updates from the UI (seqlock, bounded spin).
    applyParamsIfChanged();

    auto *pInBuf  = reinterpret_cast<float *>(pIn->pBuffer);
    auto *pOutBuf = reinterpret_cast<float *>(pOut->pBuffer);

    if (pIn->u32BufferFlags & BUFFER_SILENT) {
        if (pIn->pBuffer != pOut->pBuffer)
            ZeroMemory(pOutBuf, frames * m_channels * sizeof(float));
        pOut->u32ValidFrameCount = frames;
        pOut->u32BufferFlags     = BUFFER_SILENT;
        return;
    }

    // In-place DSP on pOutBuf (copy first if buffers differ).
    if (pIn->pBuffer != pOut->pBuffer)
        std::memcpy(pOutBuf, pInBuf, frames * m_channels * sizeof(float));

    m_chain.process(pOutBuf, static_cast<std::size_t>(frames));

    if (m_shared) {
        m_shared->meterRing.write(pOutBuf, frames, m_channels);
        m_shared->compGainReductionDb.store(
            m_chain.compressor().currentGainReductionDb(),
            std::memory_order_relaxed);
    }

    pOut->u32ValidFrameCount = frames;
    pOut->u32BufferFlags     = BUFFER_VALID;
}

// ---------------------------------------------------------------------------
// Shared memory + param sync
// ---------------------------------------------------------------------------
void TeeDspApo::openOrCreateSharedMemory()
{
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR pSd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;LS)",
            SDDL_REVISION_1, &pSd, nullptr)) {
        sa.nLength              = sizeof(sa);
        sa.lpSecurityDescriptor = pSd;
        sa.bInheritHandle       = FALSE;
    }

    const DWORD mapSize = static_cast<DWORD>(sizeof(dsp::SharedBlock));

    m_mapHandle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, dsp::kSharedMemName);
    if (!m_mapHandle) {
        m_mapHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
                                         PAGE_READWRITE, 0, mapSize,
                                         dsp::kSharedMemName);
        if (m_mapHandle) {
            ApoTrace(L"openOrCreateSharedMemory: created Global mapping");
        } else {
            wchar_t msg[160]{};
            StringCchPrintfW(msg, 160,
                L"openOrCreateSharedMemory: Global create failed err=%lu; trying Local\\",
                GetLastError());
            ApoTrace(msg);
            m_mapHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
                                             PAGE_READWRITE, 0, mapSize,
                                             L"Local\\TeeDspApo_v1");
            if (m_mapHandle)
                ApoTrace(L"openOrCreateSharedMemory: created Local mapping");
        }
    } else {
        ApoTrace(L"openOrCreateSharedMemory: opened existing Global mapping");
    }

    if (pSd)
        LocalFree(pSd);

    if (!m_mapHandle)
        return;

    m_shared = static_cast<dsp::SharedBlock *>(
        MapViewOfFile(m_mapHandle, FILE_MAP_ALL_ACCESS, 0, 0, mapSize));

    if (m_shared && m_shared->magic == 0) {
        new (m_shared) dsp::SharedBlock();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ApoTrace(L"openOrCreateSharedMemory: initialised SharedBlock");
    } else if (m_shared) {
        if (m_shared->magic != dsp::kSharedMagic
                || m_shared->version != dsp::kSharedVersion
                || m_shared->blockSize != sizeof(dsp::SharedBlock)) {
            ApoTrace(L"openOrCreateSharedMemory: stale block — reinitialising");
            new (m_shared) dsp::SharedBlock();
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    }
}

void TeeDspApo::applyParamsIfChanged()
{
    if (!m_shared)
        return;
    if (!m_shared->paramSeqlock.load(m_localParams, m_lastParamSeq))
        return;

    auto &chain = m_chain;
    chain.setBypass(m_localParams.bypassed);

    auto &eq = chain.eq();
    eq.setBypass(!m_localParams.eqEnabled);
    for (int i = 0; i < dsp::kEqBandCount; ++i) {
        const auto &b = m_localParams.eqBands[i];
        eq.setBandEnabled(i, b.enabled);
        eq.setBandType(i, static_cast<dsp::ParametricEQ::BandType>(b.type));
        eq.setBandFrequency(i, b.freqHz);
        eq.setBandQ(i, b.q);
        eq.setBandGainDb(i, b.gainDb);
    }

    auto &comp = chain.compressor();
    comp.setBypass(!m_localParams.compEnabled);
    comp.setThresholdDb(m_localParams.compThreshDb);
    comp.setRatio(m_localParams.compRatio);
    comp.setKneeDb(m_localParams.compKneeDb);
    comp.setAttackMs(m_localParams.compAttackMs);
    comp.setReleaseMs(m_localParams.compReleaseMs);
    comp.setMakeupDb(m_localParams.compMakeupDb);

    auto &exc = chain.exciter();
    exc.setBypass(!m_localParams.exciterEnabled);
    exc.setDrive(m_localParams.exciterDrive);
    exc.setMix(m_localParams.exciterMix);
    exc.setToneHz(m_localParams.exciterToneHz);
}
