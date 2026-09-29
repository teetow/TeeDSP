#include "editor/DspController.h"
#include "editor/Transport.h"
#include <QCoreApplication>
#include <cassert>

namespace {
class FakeTransport final : public editor::Transport {
public:
    explicit FakeTransport(QObject *parent) : Transport(parent) {}
    void start() override {
        active = true;
        auto saved = dsp::ChainParams{};
        saved.inputTrimDb = -3.f;
        saved.eqBands[0].gainDb = 4.f;
        emit parametersReceived(saved);
    }
    bool ready() const override { return active; }
    void submit(const dsp::ChainParams &params) override { last = params; ++writes; }
    void flush() override { ++flushes; }
    editor::Meters meters() const override { return {}; }
    void setEditorVisible(bool visible) override { editorVisible = visible; }
    void setSpectrumVisible(bool visible) override { spectrumVisible = visible; }
    bool active = false, editorVisible = false, spectrumVisible = false;
    int writes = 0, flushes = 0;
    dsp::ChainParams last{};
};
FakeTransport *transport = nullptr;
}

namespace editor {
Transport *createTransport(QObject *parent) {
    transport = new FakeTransport(parent);
    return transport;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    dsp::DspController model;
    assert(!transport->ready());
    model.start();
    assert(model.inputTrimDb() == -3.f);
    assert(model.eqBandView(0).gainDb == 4.f);
    model.flush();
    assert(transport->writes == 0); // Loading must never overwrite saved state.

    model.setEditorVisible(true);
    transport->setSpectrumVisible(true);
    assert(transport->editorVisible && transport->spectrumVisible);
    model.setEqBandGainDb(0, 7.f);
    model.flush();
    assert(transport->writes == 1);
    assert(transport->last.inputTrimDb == -3.f);
    assert(transport->last.eqBands[0].gainDb == 7.f);

    auto external = transport->last;
    external.compThreshDb = -24.f;
    emit transport->parametersReceived(external);
    assert(model.compThresholdDb() == -24.f);
    model.flush();
    assert(transport->writes == 1); // Remote updates must not echo back.
    return 0;
}
