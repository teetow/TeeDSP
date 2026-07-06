#pragma once

#include <QWidget>

namespace ui {

// Thin vertical bipolar meter for a leveler's live gain. Centered on 0 dB:
// fills upward (green) while boosting, downward (red) while cutting, and
// shows a short neutral gray tick at center when the gain is ~0. Stands as
// its own bar next to the amplitude meters instead of an overlay, so it's
// never ambiguous whether it's showing anything.
class BipolarGainMeter : public QWidget
{
    Q_OBJECT

public:
    explicit BipolarGainMeter(QWidget *parent = nullptr);

    // Symmetric +/- range the bar represents; gain is clamped to it.
    void setRangeDb(double rangeDb);
    void setGainDb(double gainDb);

    QSize sizeHint() const override { return {10, 200}; }
    QSize minimumSizeHint() const override { return {8, 60}; }

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    double m_rangeDb = 18.0;
    double m_gainDb = 0.0;
};

} // namespace ui
