#pragma once

#include <QImage>
#include <QOpenGLWidget>
#include <QVector>

namespace ui {

struct EqBandData {
    bool enabled = true;
    int type = 0;           // 0=Peaking, 1=LowShelf, 2=HighShelf
    float freqHz = 1000.0f;
    float q = 0.707f;
    float gainDb = 0.0f;
    float dynThresholdDb = 0.0f;
    float dynGainReductionDb = 0.0f;
};

// Draws a log-frequency / dB magnitude plot of the combined EQ response.
// Band handles can be dragged to change frequency + gain in one gesture.
// Double-clicking a handle resets that band's gain to 0 dB.
// Optional pre/post-DSP magnitude spectra can be drawn behind the EQ curve.
//
// Implemented as a QOpenGLWidget so update() requests are coalesced to the
// display refresh rate by the platform compositor — repaints can be triggered
// freely from data sources without burning CPU above vsync.
class EqCurve : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit EqCurve(QWidget *parent = nullptr);

    void setSampleRate(double sr);
    void setBands(const QVector<EqBandData> &bands);
    void setEqEnabled(bool enabled);

    // Spectrum overlay. magDb arrays are linear-frequency bins from DC to Nyquist
    // (length = N/2 + 1). Bin spacing is sampleRate / N.
    void setInputSpectrum(const QVector<float> &magDb, double sampleRate, int fftSize);
    void setOutputSpectrum(const QVector<float> &magDb, double sampleRate, int fftSize);
    void clearSpectra();

    void setShowInputSpectrum(bool show);
    void setShowOutputSpectrum(bool show);

    void setShowHeatmap(bool show);

    QSize sizeHint() const override { return {560, 240}; }
    QSize minimumSizeHint() const override { return {360, 160}; }

signals:
    void bandDragged(int band, float freqHz, float gainDb);
    void bandSelected(int band);
    void bandReset(int band);   // double-click — caller decides what to reset
    void bandQAdjusted(int band, float q);
    void bandTypeChanged(int band, int type);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    QRectF plotRect() const;
    double freqToX(double hz) const;
    double gainToY(double db) const;
    double xToFreq(double x) const;
    double yToGain(double y) const;
    double specDbToY(double db) const;     // separate Y mapping for spectrum

    int hitBand(const QPointF &pos) const;

    // Builds column-indexed frequency table + clears heatmap cache for the
    // current plot width. Called from resizeEvent.
    void rebuildColumnTables();
    void renderHeatmapImage(const QVector<float> &mag, double specSr,
                            QImage &out) const;

    void drawSpectrumOutline(class QPainter &p, const QVector<float> &mag,
                             double specSampleRate, const QColor &fill,
                             const QColor &stroke);
    void drawSpectrumHeatmap(class QPainter &p, const QVector<float> &mag,
                             double specSampleRate, QImage &cache);

    QVector<EqBandData> m_bands;
    double m_sampleRate = 48000.0;
    bool m_eqEnabled = true;

    QVector<float> m_inSpec;
    double m_inSpecSr = 48000.0;
    QVector<float> m_outSpec;
    double m_outSpecSr = 48000.0;
    bool m_showInputSpectrum = true;
    bool m_showOutputSpectrum = true;

    bool m_showHeatmap = false;

    int m_draggingBand = -1;
    int m_hoverBand = -1;

    // --- Per-resize caches ---
    // m_colFreq[i] = frequency at pixel-column i (i=0..plotWidth-1).
    QVector<double> m_colFreq;
    int m_cachedPlotWidth = 0;
    int m_cachedPlotHeight = 0;
    // Pre-rendered heatmap stripes for input/output. Re-rasterized when the
    // spectrum or geometry changes; blitted with drawImage (much cheaper than
    // per-column setPen + drawLine).
    QImage m_heatInImage;
    QImage m_heatOutImage;

    // Reusable scratch buffers for line strokes — avoids allocation per paint.
    QVector<QPointF> m_scratchPolyA;
    QVector<QPointF> m_scratchPolyB;

    static constexpr double kFreqMin = 20.0;
    static constexpr double kFreqMax = 20000.0;
    static constexpr double kDbRange = 24.0;
    static constexpr double kSpecDbMin = -90.0;
    static constexpr double kSpecDbMax = 0.0;
};

} // namespace ui
