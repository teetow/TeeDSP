#pragma once

#include <QVector>
#include <QWidget>

namespace ui {

struct EqBandData {
    bool enabled = true;
    int type = 0;           // 0=Peaking, 1=LowShelf, 2=HighShelf
    float freqHz = 1000.0f;
    float q = 0.707f;
    float gainDb = 0.0f;
};

// Draws a log-frequency / dB magnitude plot of the combined EQ response.
// Band handles can be dragged to change frequency + gain in one gesture.
class EqCurve : public QWidget
{
    Q_OBJECT

public:
    explicit EqCurve(QWidget *parent = nullptr);

    void setSampleRate(double sr);
    void setBands(const QVector<EqBandData> &bands);
    void setEqEnabled(bool enabled);

    QSize sizeHint() const override { return {560, 240}; }
    QSize minimumSizeHint() const override { return {360, 160}; }

signals:
    void bandDragged(int band, float freqHz, float gainDb);
    void bandSelected(int band);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    // plot coords are in device pixels; freq axis is log10
    QRectF plotRect() const;
    double freqToX(double hz) const;
    double gainToY(double db) const;
    double xToFreq(double x) const;
    double yToGain(double y) const;

    int hitBand(const QPointF &pos) const;
    double combinedMagDb(double hz) const;
    double bandMagDb(const EqBandData &b, double hz) const;

    QVector<EqBandData> m_bands;
    double m_sampleRate = 48000.0;
    bool m_eqEnabled = true;

    int m_draggingBand = -1;
    int m_hoverBand = -1;

    static constexpr double kFreqMin = 20.0;
    static constexpr double kFreqMax = 20000.0;
    static constexpr double kDbRange = 24.0;   // +/- dB shown
};

} // namespace ui
