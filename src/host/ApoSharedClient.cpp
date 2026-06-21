#include "ApoSharedClient.h"

#include <windows.h>

#include <atomic>

namespace host {

ApoSharedClient::~ApoSharedClient()
{
    close();
}

bool ApoSharedClient::tryOpen()
{
    if (m_shm)
        return true;

    HANDLE h = OpenFileMappingW(FILE_MAP_WRITE, FALSE, teedsp::kApoSharedName);
    if (!h)
        return false;

    auto *p = static_cast<teedsp::ApoShared *>(
        MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(teedsp::ApoShared)));
    if (!p) {
        CloseHandle(h);
        return false;
    }
    // The APO initialises magic/version when it creates the section; bail (and
    // retry later) if we somehow mapped it before that happened.
    if (p->magic != teedsp::kApoSharedMagic || p->version != teedsp::kApoSharedVersion) {
        UnmapViewOfFile(p);
        CloseHandle(h);
        return false;
    }

    m_handle = h;
    m_shm = p;
    return true;
}

void ApoSharedClient::close()
{
    if (m_shm) {
        UnmapViewOfFile(m_shm);
        m_shm = nullptr;
    }
    if (m_handle) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}

bool ApoSharedClient::readStatus(ApoStatus &out) const
{
    out = ApoStatus{};
    if (!m_shm)
        return false;
    out.open       = true;
    out.locked     = m_shm->locked;
    out.channels   = m_shm->channels;
    out.sampleRate = m_shm->sampleRate;
    out.uiAlive    = m_shm->uiAlive;
    std::atomic_ref<uint64_t> pc(m_shm->processCalls);
    out.processCalls = pc.load(std::memory_order_relaxed);
    return true;
}

bool ApoSharedClient::readMeters(ApoMeters &out) const
{
    out = ApoMeters{};
    if (!m_shm)
        return false;
    out.inPeakDbfs[0]    = m_shm->inPeakDbfs[0];
    out.inPeakDbfs[1]    = m_shm->inPeakDbfs[1];
    out.outPeakDbfs[0]   = m_shm->outPeakDbfs[0];
    out.outPeakDbfs[1]   = m_shm->outPeakDbfs[1];
    out.outRmsDbfs       = m_shm->outRmsDbfs;
    out.compGrDb         = m_shm->compGrDb;
    out.levelerGainDb    = m_shm->levelerGainDb;
    for (int b = 0; b < 4; ++b)
        out.spectralGainDb[b] = m_shm->spectralGainDb[b];
    out.outLevelerGainDb = m_shm->outLevelerGainDb;
    for (int b = 0; b < 5; ++b)
        out.bandGrDb[b] = m_shm->bandGrDb[b];
    out.outLufsCh[0] = m_shm->outLufsCh[0];
    out.outLufsCh[1] = m_shm->outLufsCh[1];
    out.outLufsM     = m_shm->outLufsM;
    return true;
}

void ApoSharedClient::drainAudio(std::vector<float> &pre, std::vector<float> &post)
{
    pre.clear();
    post.clear();
    if (!m_shm)
        return;

    const unsigned long long wp =
        std::atomic_ref<unsigned long long>(m_shm->audioWritePos).load(std::memory_order_acquire);
    if (wp <= m_audioReadPos) {        // nothing new (or APO restarted/rewound)
        m_audioReadPos = wp;
        return;
    }
    unsigned long long rp = m_audioReadPos;
    unsigned long long avail = wp - rp;
    if (avail > teedsp::kApoAudioRing) {   // fell behind — keep only the freshest
        rp = wp - teedsp::kApoAudioRing;
        avail = teedsp::kApoAudioRing;
    }
    pre.reserve(static_cast<size_t>(avail));
    post.reserve(static_cast<size_t>(avail));
    for (unsigned long long i = 0; i < avail; ++i) {
        const uint32_t idx = static_cast<uint32_t>((rp + i) % teedsp::kApoAudioRing);
        pre.push_back(m_shm->inRing[idx]);
        post.push_back(m_shm->outRing[idx]);
    }
    m_audioReadPos = wp;
}

void ApoSharedClient::writeParams(const dsp::ChainParams &p)
{
    if (m_shm)
        teedsp::apoWriteParams(m_shm, p);
}

void ApoSharedClient::heartbeat()
{
    if (!m_shm)
        return;
    std::atomic_ref<uint64_t> hb(m_shm->uiHeartbeat);
    hb.store(hb.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

} // namespace host
