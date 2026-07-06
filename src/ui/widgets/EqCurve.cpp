#include "EqCurve.h"

#include "../Theme.h"

#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QResizeEvent>
#include <QScreen>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace ui {

namespace {

constexpr double kBandHitRadius = 14.0;
constexpr double kBandDrawRadius = 8.0;
constexpr double kAnalyzerSlopeDbPerOct = 4.5;
constexpr double kAnalyzerSlopePivotHz = 1000.0;

// FL-Studio-style per-band palette. Distinct hues, easy to track at a glance.
const QColor kBandColors[] = {
    QColor(0xE7, 0x4C, 0x3C),   // band 1 — red
    QColor(0xF3, 0x9C, 0x12),   // band 2 — orange
    QColor(0xF1, 0xC4, 0x0F),   // band 3 — yellow
    QColor(0x2E, 0xCC, 0x71),   // band 4 — green
    QColor(0x4F, 0xC1, 0xE9),   // band 5 — teal/blue
};

QColor bandColor(int i)
{
    const int n = sizeof(kBandColors) / sizeof(kBandColors[0]);
    return kBandColors[std::clamp(i, 0, n - 1)];
}

inline double applyAnalyzerSlope(double db, double freqHz)
{
    const double f = std::max(1.0, freqHz);
    return db + kAnalyzerSlopeDbPerOct * std::log2(f / kAnalyzerSlopePivotHz);
}

// Catmull-Rom interpolation across four neighbouring FFT bins.
// Linear bin-lerp produces visible polygonal kinks on tonal peaks because each
// bin is sampled by many output pixels — straight lines between bin values
// become straight ramps in the rendered curve. A cubic smoothes those into
// natural-looking arcs (the look FL Studio's analyzer is known for).
//
// `bin` is the (fractional) bin index; `mag` is the bin array, `n` its size.
inline double sampleSpectrumSmooth(const float *mag, int n, double bin)
{
    if (n <= 0) return 0.0;
    if (n == 1) return mag[0];
    const int i1 = std::clamp(static_cast<int>(std::floor(bin)), 0, n - 1);
    const int i0 = std::max(i1 - 1, 0);
    const int i2 = std::min(i1 + 1, n - 1);
    const int i3 = std::min(i1 + 2, n - 1);
    const double t  = std::clamp(bin - i1, 0.0, 1.0);
    const double p0 = mag[i0];
    const double p1 = mag[i1];
    const double p2 = mag[i2];
    const double p3 = mag[i3];
    const double t2 = t * t;
    const double t3 = t2 * t;
    return 0.5 * (
        (2.0 * p1) +
        (-p0 + p2) * t +
        (2.0*p0 - 5.0*p1 + 4.0*p2 - p3) * t2 +
        (-p0 + 3.0*p1 - 3.0*p2 + p3) * t3);
}

// Inferno-flavoured perceptual colormap. Hand-picked nine stops covering the
// full magnitude range from silence (deep blue/black) through magenta and
// orange to a near-white peak. Linear RGB interpolation between stops; close
// enough to the real thing for our spectrogram.
struct ColorStop { float t; uint8_t r, g, b; };
constexpr ColorStop kInferno[] = {
    {0.000f,   0,   0,   4},
    {0.125f,  28,  16,  68},
    {0.250f,  66,  10, 104},
    {0.375f, 106,  23, 110},
    {0.500f, 147,  38, 103},
    {0.625f, 188,  55,  84},
    {0.750f, 221,  81,  58},
    {0.875f, 243, 120,  25},
    {1.000f, 252, 255, 164},
};

QRgb infernoRgb(float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    const int n = static_cast<int>(sizeof(kInferno) / sizeof(kInferno[0]));
    for (int i = 1; i < n; ++i) {
        if (t <= kInferno[i].t) {
            const auto &a = kInferno[i - 1];
            const auto &b = kInferno[i];
            const float u = (t - a.t) / (b.t - a.t);
            const int r = static_cast<int>(a.r + u * (b.r - a.r));
            const int g = static_cast<int>(a.g + u * (b.g - a.g));
            const int bl = static_cast<int>(a.b + u * (b.b - a.b));
            return qRgb(r, g, bl);
        }
    }
    return qRgb(kInferno[n - 1].r, kInferno[n - 1].g, kInferno[n - 1].b);
}

struct BiquadCoefs { double b0, b1, b2, a1, a2; };

BiquadCoefs coefsFor(int type, double freqHz, double q, double gainDb, double sr)
{
    if (freqHz <= 0.0) freqHz = 1.0;
    if (freqHz > sr * 0.49) freqHz = sr * 0.49;
    if (q < 0.1) q = 0.1;

    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * M_PI * freqHz / sr;
    const double cosW = std::cos(w0);
    const double sinW = std::sin(w0);
    const double alpha = sinW / (2.0 * q);

    BiquadCoefs c{};
    double a0 = 1.0;

    if (type == 0) { // peaking
        const double b0 = 1.0 + alpha * A;
        const double b1 = -2.0 * cosW;
        const double b2 = 1.0 - alpha * A;
        a0             = 1.0 + alpha / A;
        const double a1 = -2.0 * cosW;
        const double a2 = 1.0 - alpha / A;
        c = {b0, b1, b2, a1, a2};
    } else if (type == 1) { // low shelf
        const double sqA = std::sqrt(A);
        const double b0 = A * ((A + 1.0) - (A - 1.0) * cosW + 2.0 * sqA * alpha);
        const double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW);
        const double b2 = A * ((A + 1.0) - (A - 1.0) * cosW - 2.0 * sqA * alpha);
        a0              = (A + 1.0) + (A - 1.0) * cosW + 2.0 * sqA * alpha;
        const double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW);
        const double a2 = (A + 1.0) + (A - 1.0) * cosW - 2.0 * sqA * alpha;
        c = {b0, b1, b2, a1, a2};
    } else { // high shelf
        const double sqA = std::sqrt(A);
        const double b0 = A * ((A + 1.0) + (A - 1.0) * cosW + 2.0 * sqA * alpha);
        const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
        const double b2 = A * ((A + 1.0) + (A - 1.0) * cosW - 2.0 * sqA * alpha);
        a0              = (A + 1.0) - (A - 1.0) * cosW + 2.0 * sqA * alpha;
        const double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW);
        const double a2 = (A + 1.0) - (A - 1.0) * cosW - 2.0 * sqA * alpha;
        c = {b0, b1, b2, a1, a2};
    }

    c.b0 /= a0; c.b1 /= a0; c.b2 /= a0; c.a1 /= a0; c.a2 /= a0;
    return c;
}

inline double magDbAt(const BiquadCoefs &c,
                      double cw, double cw2, double sw, double sw2)
{
    const double numRe = c.b0 + c.b1 * cw + c.b2 * cw2;
    const double numIm = -(c.b1 * sw + c.b2 * sw2);
    const double denRe = 1.0 + c.a1 * cw + c.a2 * cw2;
    const double denIm = -(c.a1 * sw + c.a2 * sw2);

    const double numMag2 = numRe * numRe + numIm * numIm;
    const double denMag2 = denRe * denRe + denIm * denIm;
    if (denMag2 < 1e-30 || numMag2 < 1e-30) return -120.0;
    return 10.0 * std::log10(numMag2 / denMag2);
}

bool sameBand(const EqBandData &a, const EqBandData &b)
{
    return a.enabled == b.enabled
        && a.type == b.type
        && a.freqHz == b.freqHz
        && a.q == b.q
        && a.gainDb == b.gainDb
        && a.dynThresholdDb == b.dynThresholdDb
        && a.dynGainReductionDb == b.dynGainReductionDb;
}

} // namespace

EqCurve::EqCurve(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    // Background is fully drawn each frame — let Qt skip its automatic clear.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    m_animationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_animationTimer, &QChronoTimer::timeout, this, [this]() {
        if (m_spectrumAnimating && isVisible()) {
            update();
        } else {
            m_animationTimer.stop();
        }
    });
}

void EqCurve::setSampleRate(double sr)
{
    if (sr <= 0.0 || m_sampleRate == sr) return;
    m_sampleRate = sr;
    rebuildAngularTables();
    m_responseCurvesDirty = true;
    update();
}

void EqCurve::setBands(const QVector<EqBandData> &bands)
{
    if (m_bands.size() == bands.size()) {
        bool equal = true;
        for (int i = 0; i < bands.size(); ++i) {
            if (!sameBand(m_bands[i], bands[i])) {
                equal = false;
                break;
            }
        }
        if (equal) return;
    }
    m_bands = bands;
    m_responseCurvesDirty = true;
    update();
}

void EqCurve::setEqEnabled(bool enabled)
{
    if (m_eqEnabled == enabled) return;
    m_eqEnabled = enabled;
    m_responseCurvesDirty = true;
    update();
}

void EqCurve::setSpectra(const QVector<float> &inMag, const QVector<float> &outMag,
                         double sr, int /*fftSize*/)
{
    if (m_inSpec == inMag && m_outSpec == outMag
        && m_inSpecSr == sr && m_outSpecSr == sr) {
        return;
    }

    m_inSpec = inMag;
    m_inSpecSr = sr;
    m_outSpec = outMag;
    m_outSpecSr = sr;

    if (m_showHeatmap
        && (!m_heatmapClock.isValid() || m_heatmapClock.elapsed() >= kHeatmapIntervalMs)) {
        rebuildHeatmapCache();
        m_heatmapClock.restart();
    }

    rebuildSpectrumTargets(true);
    if (m_showInputSpectrum || m_showOutputSpectrum)
        update();
}

void EqCurve::setInputSpectrum(const QVector<float> &mag, double sr, int fftSize)
{
    setSpectra(mag, m_outSpec, sr, fftSize);
}

void EqCurve::setOutputSpectrum(const QVector<float> &mag, double sr, int fftSize)
{
    setSpectra(m_inSpec, mag, sr, fftSize);
}

void EqCurve::clearSpectra()
{
    m_inSpec.clear();
    m_outSpec.clear();
    m_heatInImage = QImage();
    m_heatOutImage = QImage();
    m_inSpectrumFromY.clear();
    m_inSpectrumTargetY.clear();
    m_outSpectrumFromY.clear();
    m_outSpectrumTargetY.clear();
    m_spectrumAnimating = false;
    m_animationTimer.stop();
    update();
}

void EqCurve::setShowInputSpectrum(bool show)
{
    if (m_showInputSpectrum == show) return;
    m_showInputSpectrum = show;
    if (m_showHeatmap)
        rebuildHeatmapCache();
    update();
}

void EqCurve::setShowOutputSpectrum(bool show)
{
    if (m_showOutputSpectrum == show) return;
    m_showOutputSpectrum = show;
    if (m_showHeatmap)
        rebuildHeatmapCache();
    update();
}

void EqCurve::setShowHeatmap(bool show)
{
    if (m_showHeatmap == show) return;
    m_showHeatmap = show;
    if (show) {
        rebuildHeatmapCache();
    } else {
        m_heatInImage = QImage();
        m_heatOutImage = QImage();
    }
    update();
}

QRectF EqCurve::plotRect() const
{
    return QRectF(36, 10, width() - 70, height() - 26);
}

double EqCurve::freqToX(double hz) const
{
    const QRectF r = plotRect();
    const double n = (std::log10(hz) - std::log10(kFreqMin))
                   / (std::log10(kFreqMax) - std::log10(kFreqMin));
    return r.left() + n * r.width();
}

double EqCurve::xToFreq(double x) const
{
    const QRectF r = plotRect();
    const double n = (x - r.left()) / r.width();
    return std::pow(10.0, std::log10(kFreqMin) + n * (std::log10(kFreqMax) - std::log10(kFreqMin)));
}

double EqCurve::gainToY(double db) const
{
    const QRectF r = plotRect();
    const double n = (db + kDbRange) / (2.0 * kDbRange);
    return r.bottom() - n * r.height();
}

double EqCurve::yToGain(double y) const
{
    const QRectF r = plotRect();
    const double n = (r.bottom() - y) / r.height();
    return -kDbRange + n * (2.0 * kDbRange);
}

double EqCurve::specDbToY(double db) const
{
    const QRectF r = plotRect();
    db = std::max(kSpecDbMin, std::min(kSpecDbMax, db));
    const double n = (db - kSpecDbMin) / (kSpecDbMax - kSpecDbMin);
    return r.bottom() - n * r.height();
}

int EqCurve::hitBand(const QPointF &pos) const
{
    int best = -1;
    double bestDist = kBandHitRadius;
    for (int i = 0; i < m_bands.size(); ++i) {
        const auto &b = m_bands[i];
        const QPointF bp(freqToX(b.freqHz), gainToY(b.gainDb));
        const double d = std::hypot(pos.x() - bp.x(), pos.y() - bp.y());
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

void EqCurve::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    rebuildColumnTables();
}

void EqCurve::rebuildColumnTables()
{
    const QRectF r = plotRect();
    const int w = std::max(1, static_cast<int>(r.width()));
    const int h = std::max(1, static_cast<int>(r.height()));
    if (w == m_cachedPlotWidth && h == m_cachedPlotHeight) return;

    m_cachedPlotWidth = w;
    m_cachedPlotHeight = h;

    m_colFreq.resize(w + 1);
    const double logMin = std::log10(kFreqMin);
    const double logMax = std::log10(kFreqMax);
    const double invW = 1.0 / static_cast<double>(w);
    for (int i = 0; i <= w; ++i) {
        const double n = static_cast<double>(i) * invW;
        m_colFreq[i] = std::pow(10.0, logMin + n * (logMax - logMin));
    }
    rebuildAngularTables();
    m_responseCurvesDirty = true;
    m_staticLayersDirty = true;

    // Rasterize size changed → invalidate cached heatmap stripes.
    m_heatInImage = QImage();
    m_heatOutImage = QImage();
    if (m_showHeatmap)
        rebuildHeatmapCache();
    rebuildSpectrumTargets(false);
}

void EqCurve::rebuildAngularTables()
{
    const int w = m_cachedPlotWidth;
    if (w <= 0 || m_colFreq.size() < w) return;

    m_colCosW.resize(w);
    m_colCos2W.resize(w);
    m_colSinW.resize(w);
    m_colSin2W.resize(w);
    const double wScale = 2.0 * M_PI / m_sampleRate;
    for (int s = 0; s < w; ++s) {
        const double angle = m_colFreq[s] * wScale;
        m_colCosW[s] = std::cos(angle);
        m_colCos2W[s] = std::cos(2.0 * angle);
        m_colSinW[s] = std::sin(angle);
        m_colSin2W[s] = std::sin(2.0 * angle);
    }
}

void EqCurve::rebuildResponseCurves()
{
    const int w = m_cachedPlotWidth;
    const int n = m_bands.size();
    const QRectF r = plotRect();
    if (w <= 0 || m_colCosW.size() < w) {
        m_liveBandPolys.clear();
        m_staticCombinedPoly.clear();
        m_liveCombinedPoly.clear();
        m_responseCurvesDirty = false;
        return;
    }

    std::array<BiquadCoefs, 16> liveCoefs{};
    std::array<BiquadCoefs, 16> staticCoefs{};
    std::array<bool, 16> bandActive{};
    const int nClamp = std::min(n, static_cast<int>(liveCoefs.size()));

    m_liveBandPolys.resize(n);
    for (int i = 0; i < n; ++i)
        m_liveBandPolys[i].clear();

    for (int i = 0; i < nClamp; ++i) {
        const auto &band = m_bands[i];
        bandActive[i] = m_eqEnabled && band.enabled;
        if (!bandActive[i]) continue;
        liveCoefs[i] = coefsFor(band.type, band.freqHz, band.q,
                                band.gainDb - band.dynGainReductionDb, m_sampleRate);
        staticCoefs[i] = coefsFor(band.type, band.freqHz, band.q,
                                  band.gainDb, m_sampleRate);
        m_liveBandPolys[i].resize(w);
    }

    m_staticCombinedPoly.resize(w);
    m_liveCombinedPoly.resize(w);
    const double yScale = r.height() / (2.0 * kDbRange);
    const auto dbToY = [&r, yScale](double db) {
        return r.bottom() - (db + kDbRange) * yScale;
    };

    for (int s = 0; s < w; ++s) {
        double staticSumDb = 0.0;
        double liveSumDb = 0.0;
        const double cw = m_colCosW[s];
        const double cw2 = m_colCos2W[s];
        const double sw = m_colSinW[s];
        const double sw2 = m_colSin2W[s];

        for (int i = 0; i < nClamp; ++i) {
            if (!bandActive[i]) continue;
            const double liveDb = magDbAt(liveCoefs[i], cw, cw2, sw, sw2);
            const double staticDb = magDbAt(staticCoefs[i], cw, cw2, sw, sw2);
            m_liveBandPolys[i][s] = QPointF(r.left() + s, dbToY(liveDb));
            liveSumDb += liveDb;
            staticSumDb += staticDb;
        }

        m_staticCombinedPoly[s] = QPointF(r.left() + s, dbToY(staticSumDb));
        m_liveCombinedPoly[s] = QPointF(r.left() + s, dbToY(liveSumDb));
    }

    m_responseCurvesDirty = false;
}

void EqCurve::projectSpectrum(const QVector<float> &mag, double specSr,
                              QVector<float> &outY) const
{
    const int bins = mag.size();
    const int w = m_cachedPlotWidth;
    if (bins < 2 || w <= 0 || m_colFreq.size() < w || specSr <= 0.0) {
        outY.clear();
        return;
    }

    outY.resize(w);
    const QRectF r = plotRect();
    const double binHz = specSr / static_cast<double>(2 * (bins - 1));
    const double dbSpan = kSpecDbMax - kSpecDbMin;
    for (int s = 0; s < w; ++s) {
        const double f = std::clamp(m_colFreq[s], kFreqMin, kFreqMax);
        const double bin = f / binHz;
        double db = applyAnalyzerSlope(
            sampleSpectrumSmooth(mag.constData(), bins, bin), f);
        db = std::clamp(db, kSpecDbMin, kSpecDbMax);
        const double normalized = (db - kSpecDbMin) / dbSpan;
        outY[s] = static_cast<float>(r.bottom() - normalized * r.height());
    }
}

double EqCurve::spectrumInterpolationAlpha() const
{
    if (!m_spectrumInterpolationClock.isValid()) return 1.0;
    return std::clamp(
        static_cast<double>(m_spectrumInterpolationClock.nsecsElapsed())
            / (static_cast<double>(kSpectrumInterpolationMs) * 1'000'000.0),
        0.0, 1.0);
}

void EqCurve::rebuildSpectrumTargets(bool animate)
{
    const double oldAlpha = animate ? spectrumInterpolationAlpha() : 1.0;
    bool changed = false;
    const auto advanceFrom = [animate, oldAlpha](QVector<float> &from,
                                                  const QVector<float> &target) {
        if (!animate || from.size() != target.size()) {
            from = target;
            return;
        }
        for (int i = 0; i < from.size(); ++i)
            from[i] = static_cast<float>(
                from[i] + oldAlpha * (target[i] - from[i]));
    };
    const auto detectChange = [&changed](QVector<float> &from,
                                         const QVector<float> &target) {
        if (from.size() != target.size())
            from = target;
        for (int i = 0; i < target.size(); ++i) {
            if (std::abs(target[i] - from[i]) > 0.01f) {
                changed = true;
                break;
            }
        }
    };

    advanceFrom(m_inSpectrumFromY, m_inSpectrumTargetY);
    advanceFrom(m_outSpectrumFromY, m_outSpectrumTargetY);
    projectSpectrum(m_inSpec, m_inSpecSr, m_inSpectrumTargetY);
    projectSpectrum(m_outSpec, m_outSpecSr, m_outSpectrumTargetY);
    detectChange(m_inSpectrumFromY, m_inSpectrumTargetY);
    detectChange(m_outSpectrumFromY, m_outSpectrumTargetY);

    m_spectrumAnimating = animate && changed;
    if (m_spectrumAnimating)
        m_spectrumInterpolationClock.restart();
    else
        m_spectrumInterpolationClock.invalidate();
    updateAnimationTimer();
}

void EqCurve::updateAnimationTimer()
{
    if (!m_spectrumAnimating || !isVisible()) {
        m_animationTimer.stop();
        return;
    }

    const QScreen *currentScreen = screen();
    const double refreshHz = std::clamp(
        currentScreen ? currentScreen->refreshRate() : 60.0,
        30.0, 240.0);
    const auto interval = std::chrono::nanoseconds(
        static_cast<qint64>(std::llround(1'000'000'000.0 / refreshHz)));
    if (m_animationTimer.interval() != interval)
        m_animationTimer.setInterval(interval);
    if (!m_animationTimer.isActive())
        m_animationTimer.start();
}

void EqCurve::buildSpectrumPolyline(const QVector<float> &fromY,
                                    const QVector<float> &targetY,
                                    double alpha,
                                    QVector<QPointF> &out) const
{
    const int count = std::min(fromY.size(), targetY.size());
    if (count <= 0) {
        out.clear();
        return;
    }

    out.resize(count);
    const double left = plotRect().left();
    for (int i = 0; i < count; ++i) {
        const double y = fromY[i] + alpha * (targetY[i] - fromY[i]);
        out[i] = QPointF(left + i, y);
    }
}

void EqCurve::rebuildStaticLayers()
{
    const qreal dpr = devicePixelRatioF();
    const QSize pixelSize(std::max(1, qRound(width() * dpr)),
                          std::max(1, qRound(height() * dpr)));

    m_backgroundLayer = QPixmap(pixelSize);
    m_backgroundLayer.setDevicePixelRatio(dpr);
    m_backgroundLayer.fill(theme::kBgDeep);
    {
        QPainter p(&m_backgroundLayer);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF full(0.5, 0.5, width() - 1.0, height() - 1.0);
        p.setPen(QPen(theme::kBorderSoft, 1.0));
        p.setBrush(theme::kBgSunken);
        p.drawRoundedRect(full, 5, 5);
    }

    m_gridLayer = QPixmap(pixelSize);
    m_gridLayer.setDevicePixelRatio(dpr);
    m_gridLayer.fill(Qt::transparent);
    {
        QPainter p(&m_gridLayer);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = plotRect();

        const double freqTicks[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
        QFont labelFont = p.font();
        labelFont.setPointSizeF(7.5);
        p.setFont(labelFont);
        for (double frequency : freqTicks) {
            const double x = freqToX(frequency);
            if (x < r.left() + 1 || x > r.right() - 1) continue;
            p.setPen(QPen(theme::kBorderSoft, 1.0, Qt::DotLine));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(theme::kTextDim);
            const QString label = (frequency >= 1000.0)
                ? QStringLiteral("%1k").arg(frequency / 1000.0, 0, 'g', 2)
                : QStringLiteral("%1").arg(frequency, 0, 'f', 0);
            p.drawText(QRectF(x - 20, r.bottom() + 2, 40, 14),
                       Qt::AlignCenter, label);
        }

        const double dbTicks[] = {-18, -12, -6, 0, 6, 12, 18};
        for (double db : dbTicks) {
            const double y = gainToY(db);
            p.setPen(db == 0.0
                ? QPen(theme::kBorder, 1.2, Qt::SolidLine)
                : QPen(theme::kBorderSoft, 1.0, Qt::DotLine));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            p.setPen(theme::kTextDim);
            p.drawText(QRectF(0, y - 8, r.left() - 2, 16),
                       Qt::AlignRight | Qt::AlignVCenter,
                       (db > 0) ? QStringLiteral("+%1").arg(db, 0, 'f', 0)
                                : QStringLiteral("%1").arg(db, 0, 'f', 0));
        }

        const double spectrumTicks[] = {-60.0, -48.0, -36.0, -24.0, -12.0, 0.0};
        p.setPen(theme::kTextDim);
        for (double db : spectrumTicks) {
            const double y = specDbToY(db);
            p.drawText(QRectF(r.right() + 2, y - 8, 30, 16),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(db, 'f', 0));
        }
    }

    m_staticLayersDirty = false;
}

void EqCurve::rebuildHeatmapCache()
{
    // A pair of dense heatmaps is expensive and visually ambiguous because
    // the second one largely covers the first. Prefer output whenever it is
    // visible; input remains available as the lightweight comparison outline.
    if (m_showOutputSpectrum && !m_outSpec.isEmpty()) {
        renderHeatmapImage(m_outSpec, m_outSpecSr, m_heatOutImage);
        m_heatInImage = QImage();
    } else if (m_showInputSpectrum && !m_inSpec.isEmpty()) {
        renderHeatmapImage(m_inSpec, m_inSpecSr, m_heatInImage);
        m_heatOutImage = QImage();
    } else {
        m_heatInImage = QImage();
        m_heatOutImage = QImage();
    }
}

void EqCurve::renderHeatmapImage(const QVector<float> &mag, double specSr,
                                  QImage &out) const
{
    if (mag.isEmpty() || m_cachedPlotWidth <= 0 || m_cachedPlotHeight <= 0) {
        out = QImage();
        return;
    }
    const int bins = mag.size();
    if (bins < 2) { out = QImage(); return; }

    const int w = m_cachedPlotWidth;
    const int h = m_cachedPlotHeight;
    const double binHz = specSr / static_cast<double>(2 * (bins - 1));
    const float invRange = 1.0f / static_cast<float>(kSpecDbMax - kSpecDbMin);

    if (out.width() != w || out.height() != h ||
        out.format() != QImage::Format_ARGB32_Premultiplied)
    {
        out = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    }
    out.fill(Qt::transparent);

    for (int s = 0; s < w; ++s) {
        const double f = std::clamp(m_colFreq[s], kFreqMin, kFreqMax);
        const double bin = f / binHz;
        const double db = applyAnalyzerSlope(sampleSpectrumSmooth(mag.constData(), bins, bin), f);

        // Vertical extent of the column: from the spectrum height (top) down.
        const double n = std::clamp(
            (db - kSpecDbMin) / (kSpecDbMax - kSpecDbMin), 0.0, 1.0);
        const int top = static_cast<int>(std::round((1.0 - n) * (h - 1)));

        const float u = (static_cast<float>(db) - static_cast<float>(kSpecDbMin)) * invRange;
        const QRgb col = infernoRgb(u);

        for (int y = top; y < h; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
            line[s] = col;
        }
    }
}

void EqCurve::drawSpectrumOutline(QPainter &p, const QVector<QPointF> &poly,
                                   const QColor &fill,
                                   const QColor &stroke)
{
    if (poly.isEmpty()) return;
    const QRectF r = plotRect();
    const double rBottom = r.bottom();

    // Filled area: stroke poly + close to bottom corners.
    QPainterPath fillPath;
    fillPath.moveTo(poly.first());
    for (int i = 1; i < poly.size(); ++i) fillPath.lineTo(poly[i]);
    fillPath.lineTo(r.right(), rBottom);
    fillPath.lineTo(r.left(),  rBottom);
    fillPath.closeSubpath();

    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(fillPath);

    p.setPen(QPen(stroke, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(poly.constData(), poly.size());
}

void EqCurve::drawSpectrumHeatmap(QPainter &p, const QVector<QPointF> &poly,
                                  QImage &cache)
{
    const QRectF r = plotRect();
    const int w = m_cachedPlotWidth;
    const int h = m_cachedPlotHeight;
    if (w <= 0 || h <= 0) return;

    if (!cache.isNull()) {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.drawImage(QPointF(r.left(), r.top()), cache);
        p.setRenderHint(QPainter::Antialiasing, true);
    }

    // Spectrum outline on top — warm off-white so it reads over all inferno tones.
    if (poly.isEmpty()) return;
    p.setPen(QPen(QColor(255, 230, 160, 200), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(poly.constData(), poly.size());
}

void EqCurve::paintEvent(QPaintEvent *)
{
    if (m_cachedPlotWidth <= 0)
        rebuildColumnTables();

    const qreal dpr = devicePixelRatioF();
    const QSize expectedPixels(std::max(1, qRound(width() * dpr)),
                               std::max(1, qRound(height() * dpr)));
    if (m_staticLayersDirty
        || m_backgroundLayer.size() != expectedPixels
        || m_backgroundLayer.devicePixelRatio() != dpr) {
        rebuildStaticLayers();
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.drawPixmap(0, 0, m_backgroundLayer);

    const QRectF r = plotRect();
    const double spectrumAlpha = spectrumInterpolationAlpha();
    buildSpectrumPolyline(m_inSpectrumFromY, m_inSpectrumTargetY,
                          spectrumAlpha, m_scratchPolyA);
    buildSpectrumPolyline(m_outSpectrumFromY, m_outSpectrumTargetY,
                          spectrumAlpha, m_scratchPolyB);
    if (spectrumAlpha >= 1.0) {
        m_spectrumAnimating = false;
        m_animationTimer.stop();
    }

    // Clip subsequent fills/lines to the plot area so spectra don't bleed.
    p.save();
    p.setClipRect(r);

    // --- Spectra ---
    if (m_showHeatmap) {
        const bool outputHeatmap =
            m_showOutputSpectrum && !m_scratchPolyB.isEmpty();
        if (outputHeatmap) {
            drawSpectrumHeatmap(p, m_scratchPolyB, m_heatOutImage);
            if (m_showInputSpectrum && !m_scratchPolyA.isEmpty()) {
                QColor inputStroke(0x4F, 0xC1, 0xE9);
                inputStroke.setAlphaF(0.75);
                p.setPen(QPen(inputStroke, 1.2));
                p.setBrush(Qt::NoBrush);
                p.drawPolyline(m_scratchPolyA.constData(), m_scratchPolyA.size());
            }
        } else if (m_showInputSpectrum && !m_scratchPolyA.isEmpty()) {
            drawSpectrumHeatmap(p, m_scratchPolyA, m_heatInImage);
        }
    } else {
        if (m_showInputSpectrum && !m_scratchPolyA.isEmpty()) {
            QColor fill(0x4F, 0xC1, 0xE9);  fill.setAlphaF(0.18);
            QColor stroke(0x4F, 0xC1, 0xE9); stroke.setAlphaF(0.55);
            drawSpectrumOutline(p, m_scratchPolyA, fill, stroke);
        }
        if (m_showOutputSpectrum && !m_scratchPolyB.isEmpty()) {
            QColor fill(0xE6, 0x7E, 0x22); fill.setAlphaF(0.20);
            QColor stroke(0xE6, 0x7E, 0x22); stroke.setAlphaF(0.60);
            drawSpectrumOutline(p, m_scratchPolyB, fill, stroke);
        }
    }

    p.restore();
    p.drawPixmap(0, 0, m_gridLayer);

    if (m_responseCurvesDirty)
        rebuildResponseCurves();

    // --- Per-band response (faint, in band colour, live dynamic gain) ---
    const int N = m_bands.size();
    if (m_eqEnabled) {
        const int polyCount = std::min(N, static_cast<int>(m_liveBandPolys.size()));
        for (int i = 0; i < polyCount; ++i) {
            const QVector<QPointF> &polyBand = m_liveBandPolys[i];
            if (polyBand.isEmpty()) continue;
            QColor c2 = bandColor(i);
            c2.setAlphaF(0.30);
            p.setPen(QPen(c2, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(polyBand.constData(), polyBand.size());
        }
    }

    // --- Combined static response (no dynamic GR) ---
    if (!m_staticCombinedPoly.isEmpty()) {
        QColor staticColor = theme::kTextDim;
        staticColor.setAlphaF(0.55);
        p.setPen(QPen(staticColor, 1.3, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(m_staticCombinedPoly.constData(), m_staticCombinedPoly.size());
    }

    // --- Combined live response (dynamic GR applied) ---
    if (!m_liveCombinedPoly.isEmpty()) {
        QColor lineCol = m_eqEnabled ? theme::kAccent : theme::kTextDim;
        p.setPen(QPen(lineCol, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(m_liveCombinedPoly.constData(), m_liveCombinedPoly.size());
    }

    // --- Band handles ---
    for (int i = 0; i < N; ++i) {
        const auto &b = m_bands[i];
        const QPointF bp(freqToX(b.freqHz), gainToY(b.gainDb));
        const bool dragging = (i == m_draggingBand);
        const bool hover    = (i == m_hoverBand);
        const bool engaged  = m_eqEnabled && b.enabled;

        QColor fill = bandColor(i);
        if (!engaged) {
            fill = QColor::fromHsl(fill.hslHue(), fill.hslSaturation() / 4, 120);
        }

        const double rad = (hover || dragging) ? kBandDrawRadius + 2.0 : kBandDrawRadius;

        // Soft outer halo so the handle pops over busy spectrum content.
        if (engaged) {
            QColor halo = fill; halo.setAlphaF(0.25);
            p.setPen(Qt::NoPen);
            p.setBrush(halo);
            p.drawEllipse(bp, rad + 4.0, rad + 4.0);
        }

        p.setPen(QPen(theme::kBgDeep, 2.0));
        p.setBrush(fill);
        p.drawEllipse(bp, rad, rad);

        // Centred number in a contrasting shade.
        p.setPen(theme::kBgDeep);
        QFont lf = p.font();
        lf.setPointSizeF(8.0);
        lf.setBold(true);
        p.setFont(lf);
        p.drawText(QRectF(bp.x() - 10, bp.y() - 10, 20, 20), Qt::AlignCenter,
                   QString::number(i + 1));
    }
}

void EqCurve::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    const int hit = hitBand(e->position());
    if (hit >= 0) {
        m_draggingBand = hit;
        emit bandSelected(hit);
        update();
    }
}

void EqCurve::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF pos = e->position();
    if (m_draggingBand >= 0 && m_draggingBand < m_bands.size()) {
        const QRectF r = plotRect();
        const double x = std::max(r.left(), std::min(r.right(), pos.x()));
        const double y = std::max(r.top(), std::min(r.bottom(), pos.y()));
        const double f = std::max<double>(kFreqMin,
                           std::min<double>(kFreqMax, xToFreq(x)));
        const double g = std::max(-kDbRange, std::min(kDbRange, yToGain(y)));

        auto &b = m_bands[m_draggingBand];
        b.freqHz = static_cast<float>(f);
        b.gainDb = static_cast<float>(g);
        emit bandDragged(m_draggingBand,
                         static_cast<float>(f),
                         static_cast<float>(g));
        update();
    } else {
        const int hit = hitBand(pos);
        if (hit != m_hoverBand) {
            m_hoverBand = hit;
            setCursor(hit >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
            update();
        }
    }
}

void EqCurve::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_draggingBand >= 0) {
        m_draggingBand = -1;
        update();
    }
}

void EqCurve::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    const int hit = hitBand(e->position());
    if (hit >= 0) {
        // Cancel any in-progress drag started by the press half of the double-click.
        m_draggingBand = -1;
        emit bandEqReset(hit);
        update();
    }
}

void EqCurve::wheelEvent(QWheelEvent *e)
{
    const int hit = hitBand(e->position());
    if (hit < 0 || hit >= m_bands.size()) {
        e->ignore();
        return;
    }

    const float steps = static_cast<float>(e->angleDelta().y()) / 120.0f;
    if (std::fabs(steps) < 0.001f) {
        e->accept();
        return;
    }

    auto &b = m_bands[hit];
    float q = b.q;
    q *= std::pow(1.1f, steps);
    q = std::clamp(q, 0.1f, 20.0f);
    b.q = q;
    emit bandQAdjusted(hit, q);
    update();
    e->accept();
}

void EqCurve::contextMenuEvent(QContextMenuEvent *e)
{
    const int hit = hitBand(e->pos());
    if (hit < 0 || hit >= m_bands.size()) {
        e->ignore();
        return;
    }

    emit bandSelected(hit);

    QMenu menu(this);
    QAction *peak = menu.addAction(QStringLiteral("Peaking"));
    QAction *lowShelf = menu.addAction(QStringLiteral("Low Shelf"));
    QAction *highShelf = menu.addAction(QStringLiteral("High Shelf"));
    peak->setCheckable(true);
    lowShelf->setCheckable(true);
    highShelf->setCheckable(true);

    const int type = std::clamp(m_bands[hit].type, 0, 2);
    peak->setChecked(type == 0);
    lowShelf->setChecked(type == 1);
    highShelf->setChecked(type == 2);

    menu.addSeparator();
    QAction *resetAction = menu.addAction(QStringLiteral("Reset Band"));

    QAction *selected = menu.exec(e->globalPos());
    if (!selected)
        return;

    if (selected == resetAction) {
        emit bandReset(hit);
        return;
    }

    int newType = type;
    if (selected == peak) newType = 0;
    else if (selected == lowShelf) newType = 1;
    else if (selected == highShelf) newType = 2;

    if (newType != type) {
        m_bands[hit].type = newType;
        emit bandTypeChanged(hit, newType);
        update();
    }
}

} // namespace ui
