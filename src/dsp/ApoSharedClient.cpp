#include "ApoSharedClient.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QDebug>
#include <QMetaType>

namespace dsp {

namespace {
constexpr int kRetryIntervalMs = 2000;
constexpr int kMeterIntervalMs = 16; // ~60 Hz meter poll
constexpr int kDrainFrames     = 4096;
}

ApoSharedClient::ApoSharedClient(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QVector<float>>("QVector<float>");

    tryOpen();

    m_retryTimer.setInterval(kRetryIntervalMs);
    m_retryTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_retryTimer, &QTimer::timeout, this, [this]() {
        if (!m_shared)
            tryOpen();
        else
            m_retryTimer.stop();
    });
    if (!m_shared)
        m_retryTimer.start();

    m_meterTimer.setInterval(kMeterIntervalMs);
    m_meterTimer.setTimerType(Qt::CoarseTimer); // avoid 1 ms system-timer resolution
    connect(&m_meterTimer, &QTimer::timeout, this, &ApoSharedClient::poll);
    m_meterTimer.start();
}

ApoSharedClient::~ApoSharedClient()
{
    if (m_shared) {
        UnmapViewOfFile(m_shared);
        m_shared = nullptr;
    }
    if (m_mapHandle) {
        CloseHandle(m_mapHandle);
        m_mapHandle = nullptr;
    }
}

float ApoSharedClient::compGainReductionDb() const
{
    if (!m_shared) return 0.0f;
    return m_shared->compGainReductionDb.load(std::memory_order_relaxed);
}

void ApoSharedClient::pushParams(const ChainParams &p)
{
    if (!m_shared) return;
    m_shared->paramSeqlock.store(p);
}

void ApoSharedClient::poll()
{
    emit meterTick();

    if (!m_shared) return;

    const uint32_t ch = m_shared->meterRing.channels.load(std::memory_order_relaxed);
    const uint32_t sr = m_shared->meterRing.sampleRate.load(std::memory_order_relaxed);
    if (ch == 0 || sr == 0) return;

    QVector<float> buf(kDrainFrames * static_cast<int>(ch));
    const uint32_t frames = m_shared->meterRing.read(
        buf.data(), static_cast<uint32_t>(kDrainFrames));

    if (frames > 0) {
        buf.resize(static_cast<int>(frames * ch));
        emit samplesReady(buf, static_cast<int>(ch), static_cast<int>(sr));
    }
}

void ApoSharedClient::tryOpen()
{
    const DWORD mapSize = static_cast<DWORD>(sizeof(SharedBlock));

    // Try Global\ first (APO in audiodg Session 0 with SeCreateGlobalPrivilege).
    // Fall back to Local\ (APO fell back to session-local namespace).
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemName);
    if (!h) {
        h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\TeeDspApo_v1");
    }
    if (!h) {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND)
            qWarning() << "ApoSharedClient: OpenFileMappingW failed, error" << err;
        return;
    }

    auto *view = static_cast<SharedBlock *>(
        MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, mapSize));
    if (!view) {
        CloseHandle(h);
        return;
    }

    // The APO is the sole initialiser of the SharedBlock.  The UI only consumes it.
    // Validate that the block is fully initialised and matches our compiled-in layout
    // before using it; if not, unmap and let the retry timer try again.
    if (view->magic != kSharedMagic
            || view->version != kSharedVersion
            || view->blockSize != sizeof(SharedBlock)) {
        qWarning() << "ApoSharedClient: shared block invalid (magic/version/size mismatch) — will retry";
        UnmapViewOfFile(view);
        CloseHandle(h);
        return;
    }

    m_mapHandle = h;
    m_shared    = view;
    emit connectedChanged();
}

} // namespace dsp
