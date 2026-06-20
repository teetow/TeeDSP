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

TeeDspApo::TeeDspApo(IUnknown *pUnkOuter)
{
    m_inner.owner = this;
    // When aggregated, delegate the public IUnknown to the engine's controlling
    // unknown; otherwise to our own inner (non-delegating) unknown.
    m_pOuter = pUnkOuter ? pUnkOuter : static_cast<IUnknown *>(&m_inner);
}

TeeDspApo::~TeeDspApo()
{
    closeTelemetry();
}

// -------- IUnknown: delegating (forwarded to controlling unknown) --------

HRESULT STDMETHODCALLTYPE TeeDspApo::QueryInterface(REFIID riid, void **ppv)
{
    return m_pOuter->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE TeeDspApo::AddRef()
{
    return m_pOuter->AddRef();
}

ULONG STDMETHODCALLTYPE TeeDspApo::Release()
{
    return m_pOuter->Release();
}

// -------- Inner IUnknown: forwards to the non-delegating implementation --------

HRESULT STDMETHODCALLTYPE TeeDspApo::Inner::QueryInterface(REFIID riid, void **ppv)
{
    return owner->NonDelegatingQueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE TeeDspApo::Inner::AddRef()
{
    return owner->NonDelegatingAddRef();
}

ULONG STDMETHODCALLTYPE TeeDspApo::Inner::Release()
{
    return owner->NonDelegatingRelease();
}

// -------- Non-delegating IUnknown: the real implementation --------

HRESULT TeeDspApo::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown)) {
        // Aggregation rule: hand back the non-delegating identity, never a
        // public interface, for IID_IUnknown.
        *ppv = static_cast<IUnknown *>(&m_inner);
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject *>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT *>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration *>(this);
    } else if (riid == __uuidof(IAudioSystemEffects)) {
        // The audio engine QIs for this to accept us as a system-effects APO.
        *ppv = static_cast<IAudioSystemEffects *>(this);
    } else {
        return E_NOINTERFACE;
    }

    // AddRef through the returned pointer, NOT NonDelegatingAddRef(): for a
    // public interface that delegates to the controlling (outer) unknown, so
    // the matching Release also hits the outer. For IID_IUnknown, *ppv is the
    // inner unknown, so this resolves to NonDelegatingAddRef anyway. Calling
    // NonDelegatingAddRef() unconditionally corrupts the aggregated refcount
    // and crashes audiodg on release.
    reinterpret_cast<IUnknown *>(*ppv)->AddRef();
    return S_OK;
}

ULONG TeeDspApo::NonDelegatingAddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG TeeDspApo::NonDelegatingRelease()
{
    const ULONG n = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) delete this;
    return n;
}

// -------- Telemetry (cross-process proof that APOProcess runs) --------

void TeeDspApo::openTelemetry()
{
    if (m_shm) return;

    // NULL DACL so a user-session observer can open the section. Dev-only;
    // the production build will gate this behind a debug flag.
    SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;

    m_shmHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                     0, sizeof(teedsp::ApoShared), teedsp::kApoSharedName);
    if (!m_shmHandle) return;

    m_shm = static_cast<teedsp::ApoShared *>(
        MapViewOfFile(m_shmHandle, FILE_MAP_WRITE, 0, 0, sizeof(teedsp::ApoShared)));
    if (!m_shm) {
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
        return;
    }

    m_shm->magic   = teedsp::kApoSharedMagic;
    m_shm->version = teedsp::kApoSharedVersion;
}

void TeeDspApo::closeTelemetry()
{
    if (m_shm) {
        m_shm->locked = 0;
        UnmapViewOfFile(m_shm);
        m_shm = nullptr;
    }
    if (m_shmHandle) {
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
    }
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

    // The registration record identifies the processing interface. This
    // matches Microsoft's current componentized APO INF sample. The audio
    // engine discovers the system-effects capability separately by QI'ing
    // IAudioSystemEffects after activation.
    constexpr UINT32 kNumIfaces = 1;
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

    *ppRegProps = p;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::Reset()
{
    // Flush filter memory at start-of-stream / format change.
    m_chain.reset();
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

    // Bring up the DSP chain for this stream's format (config thread; may
    // allocate). The RT path then only calls m_chain.process().
    m_chain.prepare(static_cast<double>(m_sampleRate), m_channels);
    m_chain.reset();

    // --- Milestone-1 PROOF CONFIG ---------------------------------------
    // Hard-coded, unmistakable setting so we can hear that APOProcess is
    // actually running inside audiodg: everything bypassed except a large
    // low-shelf boost (heavy bass). Stage-2 replaces this with live params
    // read from the UI over shared memory.
    m_chain.setBypass(false);
    m_chain.setInputTrimDb(0.0f);
    m_chain.setOutputTrimDb(0.0f);
    m_chain.setStereoWidth(1.0f);
    m_chain.leveler().setBypass(true);
    m_chain.outputLeveler().setBypass(true);
    m_chain.compressor().setBypass(true);
    m_chain.exciter().setBypass(true);

    auto &eq = m_chain.eq();
    eq.setBypass(false);
    for (int b = 0; b < dsp::kEqBandCount; ++b)
        eq.setBandEnabled(b, false);
    eq.setBandEnabled(0, true);
    eq.setBandType(0, dsp::ParametricEQ::BandType::LowShelf);
    eq.setBandFrequency(0, 200.0f);
    eq.setBandQ(0, 0.7f);
    eq.setBandGainDb(0, 15.0f);
    // --------------------------------------------------------------------

    openTelemetry();
    if (m_shm) {
        m_shm->channels        = m_channels;
        m_shm->sampleRate      = m_sampleRate;
        m_shm->bytesPerFrame   = m_bytesPerFrame;
        m_shm->processCalls    = 0;
        m_shm->framesProcessed = 0;
        m_shm->lastBufferFlags = 0;
        m_shm->locked          = 1;
    }

    m_locked        = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::UnlockForProcess()
{
    if (m_shm) m_shm->locked = 0;
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

    // RT-safe telemetry: single interlocked ops on already-mapped memory.
    if (m_shm) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64 *>(&m_shm->processCalls));
        m_shm->lastBufferFlags = in->u32BufferFlags;
        if (in->u32BufferFlags == BUFFER_VALID)
            InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64 *>(&m_shm->framesProcessed),
                                     static_cast<LONG64>(in->u32ValidFrameCount));
    }

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
        // Copy input to the (separate) output buffer, then process in place.
        // The engine hands us distinct buffers (we do not set APO_FLAG_INPLACE).
        if (in->pBuffer != out->pBuffer) {
            std::memcpy(reinterpret_cast<void *>(out->pBuffer),
                        reinterpret_cast<const void *>(in->pBuffer),
                        static_cast<size_t>(in->u32ValidFrameCount) * m_bytesPerFrame);
        }
        m_chain.process(reinterpret_cast<float *>(out->pBuffer), in->u32ValidFrameCount);
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
