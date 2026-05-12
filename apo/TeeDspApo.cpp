#include "TeeDspApo.h"
#include "Guids.h"

#include <objbase.h>
#include <ksmedia.h>
#include <mmreg.h>
#include <new>
#include <cstring>

namespace teedsp::apo {

namespace {

constexpr UINT32 kMinChannels = 1;
constexpr UINT32 kMaxChannels = 8;
constexpr UINT32 kMinSampleRate = 8000;
constexpr UINT32 kMaxSampleRate = 192000;

bool isFloat32Format(const WAVEFORMATEX *wf)
{
    if (!wf) return false;
    if (wf->wBitsPerSample != 32) return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
            && ext->Samples.wValidBitsPerSample == 32;
    }
    return false;
}

bool isFormatAcceptable(const WAVEFORMATEX *wf)
{
    if (!isFloat32Format(wf)) return false;
    if (wf->nChannels < kMinChannels || wf->nChannels > kMaxChannels) return false;
    if (wf->nSamplesPerSec < kMinSampleRate || wf->nSamplesPerSec > kMaxSampleRate) return false;
    return true;
}

} // namespace

TeeDspApo::TeeDspApo() = default;

TeeDspApo::~TeeDspApo() = default;

// -------- IUnknown --------

HRESULT STDMETHODCALLTYPE TeeDspApo::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown)
        || riid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject *>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT *>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration *>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE TeeDspApo::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE TeeDspApo::Release()
{
    const ULONG n = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) delete this;
    return n;
}

// -------- IAudioProcessingObject --------

HRESULT STDMETHODCALLTYPE TeeDspApo::Initialize(UINT32 /*cbDataSize*/, BYTE * /*pbyData*/)
{
    // Stage 1: nothing to initialise. Stage 2 will open the IPC handles to
    // the TeeDSP UI process here (shared-memory mapping, named events) and
    // resolve them in a way that gracefully degrades to bypass if the UI
    // app isn't running.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::IsInputFormatSupported(
    IAudioMediaType * /*pOutputFormat*/,
    IAudioMediaType *pRequestedInputFormat,
    IAudioMediaType **ppSupportedInputFormat)
{
    if (!pRequestedInputFormat) return E_POINTER;
    if (ppSupportedInputFormat) *ppSupportedInputFormat = nullptr;

    const WAVEFORMATEX *wf = pRequestedInputFormat->GetAudioFormat();
    if (!isFormatAcceptable(wf))
        return APOERR_FORMAT_NOT_SUPPORTED;

    if (ppSupportedInputFormat) {
        // Format is fine as-is; return it (AddRef'd) as the supported one.
        pRequestedInputFormat->AddRef();
        *ppSupportedInputFormat = pRequestedInputFormat;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::IsOutputFormatSupported(
    IAudioMediaType * /*pInputFormat*/,
    IAudioMediaType *pRequestedOutputFormat,
    IAudioMediaType **ppSupportedOutputFormat)
{
    if (!pRequestedOutputFormat) return E_POINTER;
    if (ppSupportedOutputFormat) *ppSupportedOutputFormat = nullptr;

    const WAVEFORMATEX *wf = pRequestedOutputFormat->GetAudioFormat();
    if (!isFormatAcceptable(wf))
        return APOERR_FORMAT_NOT_SUPPORTED;

    if (ppSupportedOutputFormat) {
        pRequestedOutputFormat->AddRef();
        *ppSupportedOutputFormat = pRequestedOutputFormat;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::GetLatency(HNSTIME *pTime)
{
    if (!pTime) return E_POINTER;
    // Pass-through introduces no algorithmic latency. When real DSP lands,
    // any look-ahead (e.g., limiter look-ahead) gets reported here in
    // 100 ns units so the engine can compensate.
    *pTime = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::GetRegistrationProperties(APO_REG_PROPERTIES **ppRegProps)
{
    if (!ppRegProps) return E_POINTER;

    constexpr UINT32 kNumIfaces = 3;
    const SIZE_T cb = sizeof(APO_REG_PROPERTIES) + (kNumIfaces - 1) * sizeof(IID);
    auto *p = static_cast<APO_REG_PROPERTIES *>(CoTaskMemAlloc(cb));
    if (!p) return E_OUTOFMEMORY;

    std::memset(p, 0, cb);
    p->clsid = CLSID_TeeDspApoMfx;
    p->Flags = static_cast<APO_FLAG>(APO_FLAG_SAMPLESPERFRAME_MUST_MATCH
                                     | APO_FLAG_FRAMESPERSECOND_MUST_MATCH
                                     | APO_FLAG_BITSPERSAMPLE_MUST_MATCH);

    wcscpy_s(p->szFriendlyName, L"TeeDSP Mode Effect");
    wcscpy_s(p->szCopyrightInfo, L"TeeDSP");

    p->u32MajorVersion = 1;
    p->u32MinorVersion = 0;
    p->u32MinInputConnections = 1;
    p->u32MaxInputConnections = 1;
    p->u32MinOutputConnections = 1;
    p->u32MaxOutputConnections = 1;
    p->u32MaxInstances = 0xFFFFFFFFul;
    p->u32NumAPOInterfaces = kNumIfaces;

    p->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
    p->iidAPOInterfaceList[1] = __uuidof(IAudioProcessingObjectRT);
    p->iidAPOInterfaceList[2] = __uuidof(IAudioProcessingObjectConfiguration);

    *ppRegProps = p;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::Reset()
{
    // Stage 3 will call ProcessorChain::reset() here to flush filter
    // memory at start-of-stream / format change.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::GetInputChannelCount(UINT32 *pu32ChannelCount)
{
    if (!pu32ChannelCount) return E_POINTER;
    if (!m_locked) return APOERR_NOT_INITIALIZED;
    *pu32ChannelCount = m_channels;
    return S_OK;
}

// -------- IAudioProcessingObjectConfiguration --------

HRESULT STDMETHODCALLTYPE TeeDspApo::LockForProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_DESCRIPTOR **ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_DESCRIPTOR **ppOutputConnections)
{
    if (u32NumInputConnections != 1 || u32NumOutputConnections != 1)
        return E_INVALIDARG;
    if (!ppInputConnections || !ppOutputConnections) return E_POINTER;

    APO_CONNECTION_DESCRIPTOR *in  = ppInputConnections[0];
    APO_CONNECTION_DESCRIPTOR *out = ppOutputConnections[0];
    if (!in || !out) return E_POINTER;
    if (!in->pFormat || !out->pFormat) return E_POINTER;

    const WAVEFORMATEX *wfIn  = in->pFormat->GetAudioFormat();
    const WAVEFORMATEX *wfOut = out->pFormat->GetAudioFormat();
    if (!isFormatAcceptable(wfIn) || !isFormatAcceptable(wfOut))
        return APOERR_FORMAT_NOT_SUPPORTED;
    if (wfIn->nChannels != wfOut->nChannels
        || wfIn->nSamplesPerSec != wfOut->nSamplesPerSec
        || wfIn->wBitsPerSample != wfOut->wBitsPerSample)
        return APOERR_FORMAT_NOT_SUPPORTED;

    m_channels      = wfIn->nChannels;
    m_sampleRate    = wfIn->nSamplesPerSec;
    m_bytesPerFrame = wfIn->nBlockAlign;
    m_locked        = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::UnlockForProcess()
{
    m_locked = false;
    m_channels = 0;
    m_sampleRate = 0;
    m_bytesPerFrame = 0;
    return S_OK;
}

// -------- IAudioProcessingObjectRT --------
//
// REAL-TIME PATH. No allocation, no locking, no logging, no system calls
// beyond the supplied buffer pointers. Audit any change here against
// audiodg.exe's hard real-time deadline.

void STDMETHODCALLTYPE TeeDspApo::APOProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_PROPERTY **ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_PROPERTY **ppOutputConnections)
{
    if (u32NumInputConnections != 1 || u32NumOutputConnections != 1) return;
    if (!ppInputConnections || !ppOutputConnections) return;

    APO_CONNECTION_PROPERTY *in  = ppInputConnections[0];
    APO_CONNECTION_PROPERTY *out = ppOutputConnections[0];
    if (!in || !out) return;

    switch (in->u32BufferFlags) {
    case BUFFER_INVALID:
        out->u32ValidFrameCount = 0;
        out->u32BufferFlags = BUFFER_INVALID;
        return;

    case BUFFER_SILENT:
        // No need to copy — engine treats silent output buffers as zero.
        out->u32ValidFrameCount = in->u32ValidFrameCount;
        out->u32BufferFlags = BUFFER_SILENT;
        return;

    case BUFFER_VALID:
    default:
        // Stage 1: pass-through. Stage 3 replaces the memcpy with
        // ProcessorChain::process(reinterpret_cast<float*>(out->pBuffer),
        //                          in->u32ValidFrameCount).
        if (in->pBuffer != out->pBuffer) {
            std::memcpy(reinterpret_cast<void *>(out->pBuffer),
                        reinterpret_cast<const void *>(in->pBuffer),
                        static_cast<size_t>(in->u32ValidFrameCount) * m_bytesPerFrame);
        }
        out->u32ValidFrameCount = in->u32ValidFrameCount;
        out->u32BufferFlags = BUFFER_VALID;
        return;
    }
}

UINT32 STDMETHODCALLTYPE TeeDspApo::CalcInputFrames(UINT32 u32OutputFrameCount)
{
    return u32OutputFrameCount;
}

UINT32 STDMETHODCALLTYPE TeeDspApo::CalcOutputFrames(UINT32 u32InputFrameCount)
{
    return u32InputFrameCount;
}

} // namespace teedsp::apo
