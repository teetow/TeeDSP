#pragma once
#include "editor/Transport.h"
#include <QObject>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>

namespace remote {
// HTTP transport only: the browser never processes audio or owns saved settings.
class Client : public editor::Transport {
    Q_OBJECT
public:
    explicit Client(QObject *parent=nullptr);
    void start() override;
    bool ready() const override { return m_ready; }
    bool connected() const { return m_lastMeter.isValid() && m_lastMeter.elapsed()<3000; }
    QString statusText() const;
    void submit(const dsp::ChainParams &params) override;
    void setVolume(const QJsonObject &patch);
    editor::Meters meters() const override;
    void flush() override;
    void setEditorVisible(bool visible) override;
    void setSpectrumVisible(bool visible) override;
    QJsonObject meterData, volume;
signals:
    void metersReceived();
    void volumeReceived();
private:
    void request(const QString &path, const QJsonObject *patch,
                 std::function<void(bool,QJsonObject)> done);
    void pollParams();
    void sendPending();
    void sendVolume();
    QNetworkAccessManager m_network;
    QTimer m_meterTimer, m_stateTimer;
    QElapsedTimer m_lastMeter;
    QJsonObject m_values, m_pending, m_pendingVolume;
    QString m_error;
    bool m_spectrumVisible=true, m_ready=false, m_gettingParams=false, m_gettingMeters=false, m_gettingVolume=false, m_posting=false, m_postingVolume=false;
    quint64 m_generation=0, m_volumeGeneration=0;
};
}
