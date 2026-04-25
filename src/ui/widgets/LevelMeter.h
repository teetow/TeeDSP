#pragma once

#include <QWidget>

namespace ui {

// Horizontal bar meter for compressor gain reduction.
// Left edge = 0 dB (no reduction), right edge = kMaxDb of reduction.
// Includes a smoothed peak hold that drifts back to the live value.
class LevelMeter : public QWidget
{
    Q_OBJECT

public:
    explicit LevelMeter(QWidget *parent = nullptr);

    void setReductionDb(double db);  // passed as a positive number
    double reductionDb() const { return m_valueDb; }

    QSize sizeHint() const override { return {160, 14}; }
    QSize minimumSizeHint() const override { return {80, 12}; }

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    double m_valueDb = 0.0;
    double m_peakDb = 0.0;
    qint64 m_peakStampMs = 0;

    static constexpr double kMaxDb = 20.0;
};

} // namespace ui
