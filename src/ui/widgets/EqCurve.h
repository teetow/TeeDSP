#pragma once

#include <QChronoTimer>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QWidget>

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
// FFT targets arrive at ~60 Hz; lightweight curve + heatmap interpolation
// runs at the current screen refresh rate without rerunning analysis or
// rebuilding static layers. This remains a raster QWidget so it does not force the entire
// top-level window through QOpenGLWidget composition.
class EqCurve : public QWidget
{
    Q_OBJECT

public:
    explicit EqCurve(QWidget *parent = nullptr);
    ~EqCurve() override;

    void setSampleRate(double sr);
    void setBands(const QVector<EqBandData> &bands);
    void setEqEnabled(bool enabled);

    // Spectrum overlay. magDb arrays are linear-frequency bins from DC to Nyquist
    // (length = N/2 + 1). Bin spacing is sampleRate / N.
    void setSpectra(const QVector<float> &inMagDb, const QVector<float> &outMagDb,
                    double sampleRate, int fftSize);
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
    void bandReset(int band);   // full reset (e.g. context-menu "Reset Band")
    void bandEqReset(int band); // EQ-only reset — preserves dynamics params
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

    // Builds column-indexed frequency table for the current plot width.
    // Called from resizeEvent.
    void rebuildColumnTables();
    void rebuildAngularTables();
    void rebuildResponseCurves();
    void rebuildStaticLayers();
    void rebuildSpectrumTargets(bool animate);
    void updateAnimationTimer();
    void projectSpectrum(const QVector<float> &mag, double specSr,
                         QVector<float> &outY) const;
    double spectrumInterpolationAlpha() const;
    // Rasterizes the area under the interpolated spectrum curve(s) into
    // m_spectrumImage — inferno-colored columns in heatmap mode, translucent
    // constant-color fills otherwise. Column fills into a reused image are an
    // order of magnitude cheaper than antialiased QPainterPath fills, which
    // is what makes screen-refresh-rate repaints affordable.
    void ensureSpectrumImage(int source, double alpha);
    void renderSpectrumImage(int source, double alpha, QImage &out);
    void rebuildResponseLayer(qreal dpr, const QSize &pixelSize);
    void stopAnimation();
    void setTimerResolutionRaised(bool raised);

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
    QVector<double> m_colCosW;
    QVector<double> m_colCos2W;
    QVector<double> m_colSinW;
    QVector<double> m_colSin2W;
    int m_cachedPlotWidth = 0;
    int m_cachedPlotHeight = 0;
    // Spectrum-area raster (heatmap or translucent fills), derived per
    // animation frame from the interpolated per-column levels, so it moves in
    // lockstep with the outline strokes. Memo keys skip re-rasterizing on
    // repaints that didn't move the spectrum (hover, band drags).
    QImage m_spectrumImage;
    quint64 m_spectraGeneration = 0;
    quint64 m_spectrumImageGeneration = 0;
    double m_spectrumImageAlpha = -1.0;
    int m_spectrumImageSource = 0;
    // First row the raster actually wrote — rows above are stale garbage and
    // must never be blitted. Rastering and blitting from here down skips the
    // (typically large) empty area above the spectrum.
    int m_spectrumImageTop = 0;

    // The display curves interpolate between FFT targets on a screen-refresh-
    // paced timer without repeating FFT or spectrum-projection work. The
    // interpolation window tracks the measured target arrival interval (EMA)
    // so the curve keeps gliding at whatever cadence analysis actually runs,
    // instead of freezing after a hardcoded window.
    QVector<float> m_inSpectrumFromY;
    QVector<float> m_inSpectrumTargetY;
    QVector<float> m_outSpectrumFromY;
    QVector<float> m_outSpectrumTargetY;
    QElapsedTimer m_spectrumInterpolationClock;
    QChronoTimer m_animationTimer;
    bool m_spectrumAnimating = false;
    double m_spectrumIntervalMs = 17.0;
    bool m_timerResolutionRaised = false;


    // EQ response geometry changes only when parameters, sample rate, or size
    // changes. Spectrum-only repaints reuse these polylines.
    QVector<QVector<QPointF>> m_liveBandPolys;
    QVector<QPointF> m_staticCombinedPoly;
    QVector<QPointF> m_liveCombinedPoly;
    bool m_responseCurvesDirty = true;

    // Background chrome and grid text never change between resizes. Keeping
    // them in pixmaps makes high-refresh animation frames mostly texture blits
    // plus dynamic polylines.
    QPixmap m_backgroundLayer;
    QPixmap m_gridLayer;
    bool m_staticLayersDirty = true;

    // EQ response curves + band handles only change on parameter edits, hover,
    // or drags — nowhere near frame rate. Stroking ~10 antialiased polylines
    // per frame was the single biggest paint cost, so they render into this
    // layer on change and blit per frame.
    QPixmap m_responseLayer;
    bool m_responseLayerDirty = true;
    int m_responseLayerHover = -2;
    int m_responseLayerDrag = -2;

    static constexpr double kFreqMin = 20.0;
    static constexpr double kFreqMax = 20000.0;
    static constexpr double kDbRange = 24.0;
    static constexpr double kSpecDbMin = -90.0;
    static constexpr double kSpecDbMax = 0.0;
    // Stop animating (and drop the raised timer resolution) once no fresh
    // spectrum target has arrived for this long.
    static constexpr int kSpectrumStaleMs = 250;
};

} // namespace ui
