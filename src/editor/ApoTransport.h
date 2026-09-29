#pragma once
#include "editor/Transport.h"
#include "host/ApoSharedClient.h"
#include "host/SpectrumAnalyzer.h"
#include <QTimer>
#include <vector>

namespace editor {
class ApoTransport : public Transport {
    Q_OBJECT
public:
    explicit ApoTransport(QObject *parent=nullptr);
    void start() override;
    bool ready() const override { return m_started; }
    void submit(const dsp::ChainParams &params) override;
    void flush() override;
    Meters meters() const override;
    void setEditorVisible(bool visible) override;
    void setSpectrumVisible(bool visible) override;
    host::ApoSharedClient::ApoStatus status() const;
private:
    void tick();
    void spectrumTick();
    void writeParamsFile(const dsp::ChainParams &params) const;
    host::ApoSharedClient m_apo;
    host::SpectrumAnalyzer m_analyzer;
    QTimer m_heartbeat, m_spectrum, m_saveDebounce;
    dsp::ChainParams m_params{};
    std::vector<float> m_pre, m_post;
    double m_analyzerSampleRate = 0;
    bool m_started=false, m_dirty=true, m_visible=false, m_showSpectrum=false;
};
} // namespace editor
