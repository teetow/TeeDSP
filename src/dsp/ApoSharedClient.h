#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ApoShared.h"
#include "ChainParams.h"

#include <QObject>
#include <QTimer>

namespace dsp {

// Opened by TeeDspConfig.exe to communicate with the APO DLL living in audiodg.
// Creates or opens the global named mapping, writes params via the seqlock,
// and exposes a slot for periodic meter polling.
class ApoSharedClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged FINAL)
    Q_PROPERTY(float compGainReductionDb READ compGainReductionDb NOTIFY meterTick FINAL)

public:
    explicit ApoSharedClient(QObject *parent = nullptr);
    ~ApoSharedClient() override;

    bool isConnected() const { return m_shared != nullptr; }
    float compGainReductionDb() const;

    void pushParams(const ChainParams &p);

signals:
    void connectedChanged();
    void meterTick();
    // Emitted with post-DSP samples for optional UI metering.
    void samplesReady(const QVector<float> &samples, int channels, int sampleRate);

public slots:
    void poll(); // called by timer — drains meter ring and emits samplesReady

private:
    void tryOpen();

    HANDLE       m_mapHandle = nullptr;
    SharedBlock *m_shared    = nullptr;
    QTimer       m_retryTimer;
    QTimer       m_meterTimer;
};

} // namespace dsp
