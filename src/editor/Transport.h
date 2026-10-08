#pragma once

#include "dsp/ChainParams.h"
#include <QObject>
#include <QVector>

namespace editor {

// Values displayed by either editor. The audio integration owns their production.
struct Meters {
    static constexpr std::size_t kSpectralBandCount = 10;
    float inPeakDbfs[2] = {-120.f, -120.f};
    float outPeakDbfs[2] = {-120.f, -120.f};
    float outRmsDbfs = -120.f;
    float compGrDb = 0.f;
    float levelerGainDb = 0.f;
    float spectralGainDb[kSpectralBandCount] = {};
    float outLevelerGainDb = 0.f;
    float bandGrDb[5] = {};
    float outLufsCh[2] = {-120.f, -120.f};
    float outLufsM = -120.f;
};

// The editor's entire connection to a running processor. Implementations own
// persistence and delivery; the Qt model only holds the current editable view.
class Transport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~Transport() override = default;
    virtual void start() = 0;
    virtual bool ready() const = 0;
    virtual void submit(const dsp::ChainParams &params) = 0;
    // Transient action, separate from persisted parameters. output selects
    // the output loudness rider; false selects the input rider.
    virtual void relearnLeveler(bool output) = 0;
    virtual void flush() = 0;
    virtual Meters meters() const = 0;
    virtual void setEditorVisible(bool visible) = 0;
    virtual void setSpectrumVisible(bool visible) = 0;
signals:
    void parametersReceived(const dsp::ChainParams &params);
    void spectraUpdated(QVector<float> input, QVector<float> output,
                        double sampleRate, int fftSize);
};

// Each executable links one implementation. No platform switch in the model.
Transport *createTransport(QObject *parent);

} // namespace editor
