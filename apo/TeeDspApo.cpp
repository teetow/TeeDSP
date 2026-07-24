#include "TeeDspApo.h"
#include "Guids.h"

#include <objbase.h>
#include <ksmedia.h>
#include <mmreg.h>
#include <sddl.h>
#include <new>
#include <cmath>
#include <cstdio>
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

// Load the UI's persisted ChainParams (always-on baseline). Returns false if the
// file is missing/short/wrong-magic/wrong-version, in which case the caller uses
// defaults. Config-thread only (file I/O); never on the RT path.
bool loadParamsFile(dsp::ChainParams &out)
{
    HANDLE h = CreateFileW(teedsp::kApoParamsPath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    uint32_t magic = 0;
    DWORD got = 0;
    dsp::ChainParams tmp;
    if (ReadFile(h, &magic, sizeof(magic), &got, nullptr) && got == sizeof(magic)
        && magic == teedsp::kApoParamsMagic
        && ReadFile(h, &tmp, sizeof(tmp), &got, nullptr) && got == sizeof(tmp)
        && tmp.version == dsp::ChainParams{}.version) {
        out = tmp;
        ok = true;
    }
    CloseHandle(h);
    return ok;
}

} // namespace

// Process-unique id per APO instance. Concurrent render streams each get their
// own SFX instance inside audiodg; this id elects one of them to own the shared
// meters/ring so they don't interleave.
static std::atomic<uint32_t> g_nextInstanceId{ 1u };

TeeDspApo::TeeDspApo(IUnknown *pUnkOuter)
{
    m_inner.owner = this;
    m_instanceId = g_nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
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

    // audiodg runs at a higher integrity than a normal-user UI, so a plain NULL
    // DACL isn't enough — mandatory "no-write-up" would still block the UI's
    // writes. SDDL: allow everyone (DACL GA to WD) AND label the object Low
    // (SACL ML ... LW) so a Medium-integrity UI can write to it. Dev-grade;
    // production should scope the DACL to the specific UI and gate it behind a
    // debug flag.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }

    m_shmHandle = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                     sa.lpSecurityDescriptor ? &sa : nullptr,
                                     PAGE_READWRITE, 0, sizeof(teedsp::ApoShared),
                                     teedsp::kApoSharedName);
    const DWORD createErr = GetLastError();   // capture before LocalFree clobbers it
    if (sd) LocalFree(sd);
    if (!m_shmHandle) return;
    m_createdShared = (createErr != ERROR_ALREADY_EXISTS);

    m_shm = static_cast<teedsp::ApoShared *>(
        MapViewOfFile(m_shmHandle, FILE_MAP_WRITE, 0, 0, sizeof(teedsp::ApoShared)));
    if (!m_shm) {
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
        return;
    }

    if (m_createdShared) {
        // We are the first creator: lay down a clean block with default params.
        // If the UI already holds the section, we attach without clobbering the
        // params/heartbeat it has been maintaining.
        std::memset(m_shm, 0, sizeof(*m_shm));
        m_shm->magic   = teedsp::kApoSharedMagic;
        m_shm->version = teedsp::kApoSharedVersion;
        m_shm->params  = dsp::ChainParams{};
        m_shm->inPeakDbfs[0]  = m_shm->inPeakDbfs[1]  = -120.0f;
        m_shm->outPeakDbfs[0] = m_shm->outPeakDbfs[1] = -120.0f;
        m_shm->outRmsDbfs     = -120.0f;
        m_shm->outLufsCh[0]   = m_shm->outLufsCh[1]   = -120.0f;
        m_shm->outLufsM       = -120.0f;
    }

    // Stamp with *this instance's* compiled build time unconditionally — not
    // gated on m_createdShared. The section can outlive any single APO
    // instance (the UI keeps it mapped across stream gaps), so if we only
    // stamped it on creation, a rebuilt DLL loading into a fresh audiodg.exe
    // would leave whatever an older instance wrote in place. Writing it every
    // time an instance attaches means the field always reflects the code
    // that's actually running right now.
    std::snprintf(m_shm->dspBuildStamp, sizeof(m_shm->dspBuildStamp),
                  "%s %s", __DATE__, __TIME__);
}

void TeeDspApo::closeTelemetry()
{
    if (m_shm) {
        leaveSharedProcessing();   // if torn down while still locked, stay balanced
        UnmapViewOfFile(m_shm);
        m_shm = nullptr;
    }
    if (m_shmHandle) {
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
    }
}

// Drop this instance's contribution to the shared block: release meter
// ownership if we hold it and decrement the active-stream refcount. Guarded by
// m_locked so Unlock + teardown can't double-count.
void TeeDspApo::leaveSharedProcessing()
{
    if (!m_shm || !m_locked)
        return;
    if (m_ownsMeters)
        teedsp::apoWriteCalibration(m_shm, m_chain.calibrationState());
    uint32_t expected = m_instanceId;
    const bool wasOwner = std::atomic_ref<uint32_t>(m_shm->meterOwner)
        .compare_exchange_strong(expected, 0u, std::memory_order_acq_rel);
    if (wasOwner) {
        // The format fields describe whichever stream owns the meters. Clear
        // them so observers don't keep showing a torn-down stream's format;
        // whichever instance is next elected owner republishes its own from
        // publishMeters().
        m_shm->channels = 0;
        m_shm->sampleRate = 0;
        m_shm->bytesPerFrame = 0;
    }
    m_ownsMeters = false;
    std::atomic_ref<uint32_t>(m_shm->activeStreams)
        .fetch_sub(1u, std::memory_order_acq_rel);
    m_locked = false;
}

void TeeDspApo::publishMeters(const float *inBuf, const float *outBuf, UINT32 frames)
{
    if (!m_shm || !m_ownsMeters) return;   // only the elected owner writes meters/ring
    const UINT32 ch = m_channels ? m_channels : 1;

    // Format follows the elected owner, refreshed every block so a change of
    // ownership (e.g. the previous owner's stream ended) self-corrects within
    // one process call instead of showing whichever instance last locked.
    m_shm->channels      = m_channels;
    m_shm->sampleRate    = m_sampleRate;
    m_shm->bytesPerFrame = m_bytesPerFrame;

    // Single pass: peaks, output power (for RMS), and the mono pre/post sample
    // ring for the UI spectrum analyzer.
    float inPk[2]  = { 0.0f, 0.0f };
    float outPk[2] = { 0.0f, 0.0f };
    double sumSq = 0.0;
    const uint64_t wp = m_shm->audioWritePos;
    for (UINT32 f = 0; f < frames; ++f) {
        float inMono = 0.0f, outMono = 0.0f;
        for (UINT32 c = 0; c < ch; ++c) {
            const size_t i = static_cast<size_t>(f) * ch + c;
            const float xi = inBuf  ? inBuf[i]  : 0.0f;
            const float xo = outBuf ? outBuf[i] : 0.0f;
            inMono  += xi;
            outMono += xo;
            const float ai = std::fabs(xi), ao = std::fabs(xo);
            if (c < 2) { if (ai > inPk[c]) inPk[c] = ai; if (ao > outPk[c]) outPk[c] = ao; }
            sumSq += static_cast<double>(xo) * xo;
        }
        const uint32_t idx = static_cast<uint32_t>((wp + f) % teedsp::kApoAudioRing);
        m_shm->inRing[idx]  = inMono  / static_cast<float>(ch);
        m_shm->outRing[idx] = outMono / static_cast<float>(ch);
    }
    // Publish the advanced write position last (release), so a UI reader that
    // acquire-loads it sees the samples already in place.
    std::atomic_ref<uint64_t>(m_shm->audioWritePos)
        .store(wp + frames, std::memory_order_release);

    const auto toDb = [](float lin) { return lin > 1e-6f ? 20.0f * std::log10(lin) : -120.0f; };
    m_shm->inPeakDbfs[0]  = toDb(inPk[0]);
    m_shm->inPeakDbfs[1]  = toDb(ch > 1 ? inPk[1] : inPk[0]);
    m_shm->outPeakDbfs[0] = toDb(outPk[0]);
    m_shm->outPeakDbfs[1] = toDb(ch > 1 ? outPk[1] : outPk[0]);
    const double n = static_cast<double>(frames) * ch;
    m_shm->outRmsDbfs = (n > 0.0 && sumSq > 1e-12)
        ? static_cast<float>(10.0 * std::log10(sumSq / n)) : -120.0f;

    // Chain telemetry (atomic getters — cheap, RT-safe). The shared struct's
    // spectralGainDb/bandGrDb are fixed-size arrays sized to match these band
    // counts today (see static_asserts below); a mismatch would overrun into
    // the neighbouring shared-memory field, so the write loops always derive
    // their bound from the same named constants the arrays are asserted against.
    static_assert(sizeof(teedsp::ApoShared::spectralGainDb) / sizeof(float)
                  == dsp::SpectralLeveler::kBandCount,
                  "ApoShared::spectralGainDb size must match dsp::SpectralLeveler::kBandCount");
    static_assert(sizeof(teedsp::ApoShared::bandGrDb) / sizeof(float) == dsp::kEqBandCount,
                  "ApoShared::bandGrDb size must match dsp::kEqBandCount");
    teedsp::apoWriteCalibration(m_shm, m_chain.calibrationState());
    m_shm->compGrDb         = m_chain.compressor().currentGainReductionDb();
    m_shm->levelerGainDb    = m_chain.leveler().currentGainDb();
    for (int b = 0; b < dsp::SpectralLeveler::kBandCount; ++b)
        m_shm->spectralGainDb[b] = m_chain.spectralLeveler().currentGainDb(b);
    m_shm->outLevelerGainDb = m_chain.outputLeveler().currentGainDb();
    dsp::ParametricEQ &eq = m_chain.eq();
    for (int b = 0; b < dsp::kEqBandCount; ++b)
        m_shm->bandGrDb[b] = eq.bandDynamicGainReductionDb(b);

    // Momentary LUFS on the post-chain output (decays toward silence when
    // outBuf is null).
    m_lufs.process(outBuf, frames, ch);
    m_shm->outLufsCh[0] = m_lufs.channelLufs(0);
    m_shm->outLufsCh[1] = m_lufs.channelLufs(ch > 1 ? 1 : 0);
    m_shm->outLufsM     = m_lufs.momentaryLufs();
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
    m_lufs.prepare(static_cast<double>(m_sampleRate), m_channels);
    m_lufs.reset();

    // Always-on baseline: apply the user's persisted params so the chain runs
    // with their last settings even when the UI isn't running. A live UI
    // connection (below) overrides this with fresher params when present.
    {
        dsp::ChainParams baseline;
        if (loadParamsFile(baseline)) dsp::applyChainParams(m_chain, baseline);
        else                          dsp::applyChainParams(m_chain, dsp::ChainParams{});
    }

    openTelemetry();

    // Seed liveness tracking and align with whatever the UI has already
    // published. Ongoing changes are picked up in APOProcess.
    m_lastAppliedGen = 0;
    m_framesSinceHeartbeat = 0;
    if (m_shm) {
        dsp::LevelerChainCalibration calibration;
        if (teedsp::apoReadCalibration(m_shm, calibration))
            m_chain.restoreCalibration(calibration);

        // channels/sampleRate/bytesPerFrame are NOT written here: with 2+
        // concurrent instances (e.g. a Communications-mode stream + a media
        // stream), whichever locked most recently would clobber the format
        // the other is actively running at. They're published only by the
        // elected meterOwner, in publishMeters(), and cleared by
        // leaveSharedProcessing() when that owner unlocks — so they always
        // describe whichever stream's meters are currently shown.
        // processCalls/framesProcessed are cumulative across ALL concurrent
        // instances now — never reset here (that would zero the counter mid-
        // stream for another instance). They're initialised once on creation.
        std::atomic_ref<uint32_t>(m_shm->activeStreams)
            .fetch_add(1u, std::memory_order_acq_rel);
        m_ownsMeters = false;   // claimed lazily by the first APOProcess to win

        std::atomic_ref<uint64_t> hb(m_shm->uiHeartbeat);
        m_lastHeartbeat = hb.load(std::memory_order_acquire);

        std::atomic_ref<uint32_t> gen(m_shm->paramGen);
        const uint32_t g = gen.load(std::memory_order_acquire);
        if (g != 0) {
            dsp::ChainParams p;
            if (teedsp::apoReadParams(m_shm, p)) {
                dsp::applyChainParams(m_chain, p);
                m_lastAppliedGen = g;
                std::atomic_ref<uint32_t>(m_shm->appliedGen).store(g, std::memory_order_release);
            }
        }
    }

    m_locked        = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TeeDspApo::UnlockForProcess()
{
    leaveSharedProcessing();   // release meter ownership + drop active-stream refcount
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

    // RT-safe: telemetry, UI-liveness, and live param pickup. All ops here are
    // interlocked/atomic on already-mapped memory or relaxed atomic loads — no
    // allocation, locking, or syscalls. applyChainParams only runs when the UI
    // commits a change (paramGen advances), not every block.
    bool uiAlive = false;
    if (m_shm) {
        const UINT32 frames = in->u32ValidFrameCount;
        InterlockedIncrement64(reinterpret_cast<volatile LONG64 *>(&m_shm->processCalls));
        std::atomic_ref<uint64_t>(m_shm->lastBufferFlags)
            .store(in->u32BufferFlags, std::memory_order_relaxed);
        if (in->u32BufferFlags == BUFFER_VALID)
            InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64 *>(&m_shm->framesProcessed),
                                     static_cast<LONG64>(frames));

        // Elect a single meter/ring writer among concurrent instances (one SFX
        // instance per stream) so the shared meters & spectrum reflect one
        // stream instead of interleaving silence from a bursty co-stream.
        std::atomic_ref<uint32_t> owner(m_shm->meterOwner);
        if (owner.load(std::memory_order_relaxed) == 0u) {
            uint32_t expected = 0u;
            owner.compare_exchange_strong(expected, m_instanceId,
                                          std::memory_order_acq_rel);
        }
        m_ownsMeters = (owner.load(std::memory_order_relaxed) == m_instanceId);

        // UI liveness: stale if the heartbeat hasn't advanced within ~1s of audio.
        std::atomic_ref<uint64_t> hb(m_shm->uiHeartbeat);
        const uint64_t cur = hb.load(std::memory_order_acquire);
        if (cur != m_lastHeartbeat) {
            m_lastHeartbeat = cur;
            m_framesSinceHeartbeat = 0;
        } else {
            m_framesSinceHeartbeat += frames;
        }
        uiAlive = (m_sampleRate != 0) && (m_framesSinceHeartbeat < m_sampleRate);
        std::atomic_ref<uint32_t>(m_shm->uiAlive).store(uiAlive ? 1u : 0u, std::memory_order_relaxed);

        if (uiAlive) {
            std::atomic_ref<uint32_t> gen(m_shm->paramGen);
            const uint32_t g = gen.load(std::memory_order_acquire);
            if (g != m_lastAppliedGen) {
                dsp::ChainParams p;
                if (teedsp::apoReadParams(m_shm, p)) {
                    dsp::applyChainParams(m_chain, p);
                    m_lastAppliedGen = g;
                    std::atomic_ref<uint32_t>(m_shm->appliedGen).store(g, std::memory_order_release);
                }
            }
        }
    }

    switch (in->u32BufferFlags) {
    case BUFFER_INVALID:
        out->u32ValidFrameCount = 0;
        out->u32BufferFlags = BUFFER_INVALID;
        return;

    case BUFFER_SILENT:
        // No need to copy — engine treats silent output buffers as zero. Still
        // advance meters/ring/LUFS with silence so they decay correctly.
        publishMeters(nullptr, nullptr, in->u32ValidFrameCount);
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
        // Always-on: process with the current params regardless of whether the
        // UI is running (live edits still arrive via paramGen when it is). "Off"
        // is the persisted bypass flag, which makes the chain pass through.
        m_chain.process(reinterpret_cast<float *>(out->pBuffer), in->u32ValidFrameCount);
        publishMeters(reinterpret_cast<const float *>(in->pBuffer),
                      reinterpret_cast<const float *>(out->pBuffer),
                      in->u32ValidFrameCount);
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
