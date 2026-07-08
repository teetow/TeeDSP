#include "EqCurve.h"

#include "../Theme.h"

#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QResizeEvent>
#include <QScreen>
#include <QVarLengthArray>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm")
#endif

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

// Set TEEDSP_FPS_LOG=1 to append per-second pipeline rates (FFT target
// arrivals, animation ticks, paints) to %TEMP%\teedsp_fps.log. Measures where
// the spectrum display actually loses frame rate instead of guessing.
struct FpsProbe {
    const bool enabled = qEnvironmentVariableIsSet("TEEDSP_FPS_LOG");
    QElapsedTimer clock;
    int spectra = 0, animating = 0, animTicks = 0, paints = 0;
    int rasters = 0, respRebuilds = 0, layerRebuilds = 0;
    int plotW = 0, plotH = 0;
    double refreshHz = 0.0;
    qint64 animIntervalNs = 0;
    qint64 nsBg = 0, nsSpec = 0, nsRest = 0;
    qint64 nsEnsure = 0, nsBlit = 0, nsStroke = 0, nsResp = 0, nsLayer = 0;

    void flush()
    {
        if (!enabled) return;
        if (!clock.isValid()) { clock.start(); return; }
        if (clock.elapsed() < 1000) return;
        QFile f(QDir::temp().filePath(QStringLiteral("teedsp_fps.log")));
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            const double n = std::max(1, paints);
            f.write(QStringLiteral("spectra=%1 animStarts=%2 animTicks=%3 "
                                   "paints=%4 refreshHz=%5 animIntervalMs=%6 "
                                   "bgMs=%7 specMs=%8 restMs=%9 "
                                   "rasters=%10 resp=%11 layer=%12 plot=%13x%14\n")
                        .arg(spectra).arg(animating).arg(animTicks).arg(paints)
                        .arg(refreshHz, 0, 'f', 1)
                        .arg(static_cast<double>(animIntervalNs) / 1e6, 0, 'f', 2)
                        .arg(static_cast<double>(nsBg) / 1e6 / n, 0, 'f', 2)
                        .arg(static_cast<double>(nsSpec) / 1e6 / n, 0, 'f', 2)
                        .arg(static_cast<double>(nsRest) / 1e6 / n, 0, 'f', 2)
                        .arg(rasters).arg(respRebuilds).arg(layerRebuilds)
                        .arg(plotW).arg(plotH)
                        .toUtf8());
            f.write(QStringLiteral("  ensureMs=%1 blitMs=%2 strokeMs=%3 "
                                   "respMs=%4 layerMs=%5\n")
                        .arg(static_cast<double>(nsEnsure) / 1e6 / n, 0, 'f', 2)
                        .arg(static_cast<double>(nsBlit) / 1e6 / n, 0, 'f', 2)
                        .arg(static_cast<double>(nsStroke) / 1e6 / n, 0, 'f', 2)
                        .arg(static_cast<double>(nsResp) / 1e6 / n, 0, 'f', 2)
                        .arg(static_cast<double>(nsLayer) / 1e6 / n, 0, 'f', 2)
                        .toUtf8());
        }
        spectra = animating = animTicks = paints = 0;
        rasters = respRebuilds = layerRebuilds = 0;
        nsBg = nsSpec = nsRest = 0;
        nsEnsure = nsBlit = nsStroke = nsResp = nsLayer = 0;
        clock.restart();
    }
};
FpsProbe g_fpsProbe;

bool sameBand(const EqBandData &a, const EqBandData &b)
{
    return a.enabled == b.enabled
        && a.type == b.type
        && a.freqHz == b.freqHz
        && a.q == b.q
        && a.gainDb == b.gainDb
        && a.dynThresholdDb == b.dynThresholdDb
        // Live GR jitters by hundredths of a dB every telemetry tick; exact
        // comparison made every tick rebuild the response curves + layer.
        // Error stays bounded: once drift from the stored value reaches the
        // epsilon the update goes through.
        && std::abs(a.dynGainReductionDb - b.dynGainReductionDb) < 0.05f;
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
        const bool stale = !m_spectrumInterpolationClock.isValid()
            || m_spectrumInterpolationClock.elapsed() > kSpectrumStaleMs;
        if (m_spectrumAnimating && isVisible() && !stale) {
            ++g_fpsProbe.animTicks;
            update();
        } else {
            m_spectrumAnimating = false;
            stopAnimation();
        }
    });
}

EqCurve::~EqCurve()
{
    setTimerResolutionRaised(false);
}

void EqCurve::stopAnimation()
{
    m_animationTimer.stop();
    setTimerResolutionRaised(false);
}

void EqCurve::setTimerResolutionRaised(bool raised)
{
    if (m_timerResolutionRaised == raised) return;
    m_timerResolutionRaised = raised;
#ifdef Q_OS_WIN
    // Windows quantizes timers to ~15.6 ms by default, which flattens both
    // the 17 ms FFT feed and the refresh-paced animation timer to a fraction
    // of their intended rates. Raise resolution only while animating so the
    // process doesn't hold a wakeup-heavy setting when idle or hidden.
    if (raised) timeBeginPeriod(1); else timeEndPeriod(1);
#endif
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

    ++g_fpsProbe.spectra;
    rebuildSpectrumTargets(true);
    if (m_spectrumAnimating)
        ++g_fpsProbe.animating;
    // With the animation timer running, the next tick (≤ one frame away)
    // repaints with these targets anyway — a second update() here would just
    // add a wasted full repaint per FFT arrival.
    if ((m_showInputSpectrum || m_showOutputSpectrum)
        && !m_animationTimer.isActive())
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
    m_spectrumImage = QImage();
    m_inSpectrumFromY.clear();
    m_inSpectrumTargetY.clear();
    m_outSpectrumFromY.clear();
    m_outSpectrumTargetY.clear();
    m_spectrumAnimating = false;
    stopAnimation();
    update();
}

void EqCurve::setShowInputSpectrum(bool show)
{
    if (m_showInputSpectrum == show) return;
    m_showInputSpectrum = show;
    update();
}

void EqCurve::setShowOutputSpectrum(bool show)
{
    if (m_showOutputSpectrum == show) return;
    m_showOutputSpectrum = show;
    update();
}

void EqCurve::setShowHeatmap(bool show)
{
    if (m_showHeatmap == show) return;
    m_showHeatmap = show;
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
        m_responseLayerDirty = true;
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
    }

    // 2 px sampling: response curves are smooth, and both the biquad
    // magnitude math and the antialiased stroking scale with point count.
    constexpr int kCurveStep = 2;
    const int pointBudget = w / kCurveStep + 2;
    m_staticCombinedPoly.clear();
    m_staticCombinedPoly.reserve(pointBudget);
    m_liveCombinedPoly.clear();
    m_liveCombinedPoly.reserve(pointBudget);
    for (int i = 0; i < nClamp; ++i) {
        if (bandActive[i]) m_liveBandPolys[i].reserve(pointBudget);
    }

    const double yScale = r.height() / (2.0 * kDbRange);
    const auto dbToY = [&r, yScale](double db) {
        return r.bottom() - (db + kDbRange) * yScale;
    };
    // The faint per-band curves are context only — sample them at half the
    // combined-curve density again; their magDbAt values are needed for the
    // combined sums at every step regardless, so this only trims stroke work.
    const auto sampleColumn = [&](int s, bool bandPointsToo) {
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
            if (bandPointsToo)
                m_liveBandPolys[i].append(QPointF(r.left() + s, dbToY(liveDb)));
            liveSumDb += liveDb;
            staticSumDb += staticDb;
        }

        m_staticCombinedPoly.append(QPointF(r.left() + s, dbToY(staticSumDb)));
        m_liveCombinedPoly.append(QPointF(r.left() + s, dbToY(liveSumDb)));
    };
    for (int s = 0; s < w; s += kCurveStep)
        sampleColumn(s, s % (2 * kCurveStep) == 0);
    if ((w - 1) % kCurveStep != 0)
        sampleColumn(w - 1, true);

    ++g_fpsProbe.respRebuilds;
    m_responseCurvesDirty = false;
    m_responseLayerDirty = true;
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
            / (m_spectrumIntervalMs * 1'000'000.0),
        0.0, 1.0);
}

void EqCurve::rebuildSpectrumTargets(bool animate)
{
    const double oldAlpha = animate ? spectrumInterpolationAlpha() : 1.0;
    if (animate && m_spectrumInterpolationClock.isValid()) {
        const double arrivalMs =
            static_cast<double>(m_spectrumInterpolationClock.nsecsElapsed())
                / 1'000'000.0;
        if (arrivalMs < kSpectrumStaleMs) {
            m_spectrumIntervalMs = std::clamp(
                0.75 * m_spectrumIntervalMs + 0.25 * arrivalMs, 5.0, 120.0);
        }
    }
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
    ++m_spectraGeneration;

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
        stopAnimation();
        return;
    }
    setTimerResolutionRaised(true);

    const QScreen *currentScreen = screen();
    const double refreshHz = std::clamp(
        currentScreen ? currentScreen->refreshRate() : 60.0,
        30.0, 240.0);
    const auto interval = std::chrono::nanoseconds(
        static_cast<qint64>(std::llround(1'000'000'000.0 / refreshHz)));
    g_fpsProbe.refreshHz = refreshHz;
    g_fpsProbe.animIntervalNs = interval.count();
    if (m_animationTimer.interval() != interval)
        m_animationTimer.setInterval(interval);
    if (!m_animationTimer.isActive())
        m_animationTimer.start();
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

void EqCurve::ensureSpectrumImage(int source, double alpha)
{
    if (!m_spectrumImage.isNull()
        && m_spectrumImage.width() == m_cachedPlotWidth
        && m_spectrumImage.height() == m_cachedPlotHeight
        && m_spectrumImageSource == source
        && m_spectrumImageGeneration == m_spectraGeneration
        && m_spectrumImageAlpha == alpha) {
        return;
    }
    renderSpectrumImage(source, alpha, m_spectrumImage);
    ++g_fpsProbe.rasters;
    m_spectrumImageSource = source;
    m_spectrumImageGeneration = m_spectraGeneration;
    m_spectrumImageAlpha = alpha;
}

// Premultiplied SourceOver of two premultiplied colors.
static inline QRgb premulOver(QRgb src, QRgb dst)
{
    const int ia = 255 - qAlpha(src);
    return qRgba(qRed(src)   + (qRed(dst)   * ia + 127) / 255,
                 qGreen(src) + (qGreen(dst) * ia + 127) / 255,
                 qBlue(src)  + (qBlue(dst)  * ia + 127) / 255,
                 qAlpha(src) + (qAlpha(dst) * ia + 127) / 255);
}

// Scale a premultiplied color by a 0..256 coverage factor.
static inline QRgb premulScale(QRgb c, int cov256)
{
    return qRgba((qRed(c) * cov256) >> 8, (qGreen(c) * cov256) >> 8,
                 (qBlue(c) * cov256) >> 8, (qAlpha(c) * cov256) >> 8);
}

// source bits: 1 = input visible, 2 = output visible, 4 = heatmap mode.
void EqCurve::renderSpectrumImage(int source, double alpha, QImage &out)
{
    const int w = m_cachedPlotWidth;
    const int h = m_cachedPlotHeight;
    const bool heatmap = (source & 4) != 0;

    const auto usable = [w](const QVector<float> &from, const QVector<float> &target) {
        return from.size() >= w && target.size() >= w;
    };
    const bool haveIn  = (source & 1)
        && usable(m_inSpectrumFromY,  m_inSpectrumTargetY);
    const bool haveOut = (source & 2)
        && usable(m_outSpectrumFromY, m_outSpectrumTargetY);
    if (w <= 0 || h <= 0 || (!haveIn && !haveOut)) {
        out = QImage();
        m_spectrumImageTop = 0;
        return;
    }

    if (out.width() != w || out.height() != h ||
        out.format() != QImage::Format_ARGB32_Premultiplied)
    {
        out = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    }

    // The from/target arrays already carry the projected per-column levels;
    // inverting projectSpectrum's Y mapping recovers the normalized dB, so
    // the FFT resampling + slope math never reruns here.
    const QRectF r = plotRect();
    const double rTop = r.top();
    const double bottom = r.bottom();
    const double invHeight = 1.0 / r.height();
    const float a = static_cast<float>(alpha);
    const auto columnY = [&](const QVector<float> &from,
                             const QVector<float> &target, int s) {
        return static_cast<double>(from[s] + a * (target[s] - from[s]));
    };
    const auto columnLevel = [&](const QVector<float> &from,
                                 const QVector<float> &target, int s) {
        return std::clamp((bottom - columnY(from, target, s)) * invHeight,
                          0.0, 1.0);
    };
    const auto levelToTop = [h](double n) {
        return static_cast<int>(std::round((1.0 - n) * (h - 1)));
    };
    const auto outlineMinRow = [&](const QVector<float> &from,
                                   const QVector<float> &target) {
        double minY = h;
        for (int s = 0; s < w; ++s)
            minY = std::min(minY, columnY(from, target, s) - rTop);
        return std::max(0, static_cast<int>(minY) - 2);
    };

    // All rows the fills or outlines will touch must be part of the cleared +
    // blitted band; rows above it keep stale pixels and are never blitted.
    int minTop = h;

    QVarLengthArray<int, 4096> topIn, topOut;
    QVarLengthArray<QRgb, 4096> cols;
    if (heatmap) {
        const bool outPrimary = haveOut;
        const QVector<float> &from   = outPrimary ? m_outSpectrumFromY   : m_inSpectrumFromY;
        const QVector<float> &target = outPrimary ? m_outSpectrumTargetY : m_inSpectrumTargetY;
        topOut.resize(w);
        cols.resize(w);
        for (int s = 0; s < w; ++s) {
            const double n = columnLevel(from, target, s);
            topOut[s] = levelToTop(n);
            minTop = std::min(minTop, topOut[s]);
            cols[s] = infernoRgb(static_cast<float>(n));
        }
        // The comparison outline of the secondary spectrum can sit anywhere
        // above the heatmap — widen the cleared band to cover it.
        if (outPrimary && haveIn)
            minTop = std::min(minTop,
                              outlineMinRow(m_inSpectrumFromY, m_inSpectrumTargetY));
        minTop = std::max(0, minTop - 2);

        for (int y = minTop; y < h; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int s = 0; s < w; ++s)
                line[s] = (y >= topOut[s]) ? cols[s] : 0;
        }
    } else {
        // Constant-color translucent fills, input under output — same colors
        // and compositing the old QPainterPath fills produced, but as column
        // fills. The overlap color is precomputed premultiplied SourceOver.
        const QRgb cIn  = qPremultiply(qRgba(0x4F, 0xC1, 0xE9, 46));  // αF 0.18
        const QRgb cOut = qPremultiply(qRgba(0xE6, 0x7E, 0x22, 51));  // αF 0.20
        const QRgb cBoth = premulOver(cOut, cIn);
        topIn.resize(w);
        topOut.resize(w);
        for (int s = 0; s < w; ++s) {
            topIn[s]  = haveIn  ? levelToTop(columnLevel(m_inSpectrumFromY,
                                                         m_inSpectrumTargetY, s))  : h;
            topOut[s] = haveOut ? levelToTop(columnLevel(m_outSpectrumFromY,
                                                         m_outSpectrumTargetY, s)) : h;
            minTop = std::min({minTop, topIn[s], topOut[s]});
        }
        minTop = std::max(0, minTop - 2);
        for (int y = minTop; y < h; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int s = 0; s < w; ++s) {
                const bool belowIn  = y >= topIn[s];
                const bool belowOut = y >= topOut[s];
                line[s] = belowIn ? (belowOut ? cBoth : cIn)
                                  : (belowOut ? cOut : 0);
            }
        }
    }

    // Spectrum outlines, rasterized as coverage-weighted vertical spans per
    // column. QPainter's antialiased stroker cost explodes on jagged FFT
    // polylines (measured ~3 ms per outline); this is O(w) regardless of
    // jaggedness and visually equivalent at 1.2 px width.
    const auto blendOutline = [&](const QVector<float> &from,
                                  const QVector<float> &target, QRgb c) {
        constexpr double kHalfWidth = 0.7;
        double yPrev = columnY(from, target, 0) - rTop;
        double yCur = yPrev;
        for (int s = 0; s < w; ++s) {
            const double yNext = (s + 1 < w)
                ? columnY(from, target, s + 1) - rTop : yCur;
            const double mPrev = 0.5 * (yCur + yPrev);
            const double mNext = 0.5 * (yCur + yNext);
            const double lo = std::clamp(
                std::min({yCur, mPrev, mNext}) - kHalfWidth,
                static_cast<double>(minTop), static_cast<double>(h));
            const double hi = std::clamp(
                std::max({yCur, mPrev, mNext}) + kHalfWidth,
                static_cast<double>(minTop), static_cast<double>(h));
            const int py0 = static_cast<int>(lo);
            const int py1 = std::min(h - 1, static_cast<int>(hi));
            for (int py = py0; py <= py1; ++py) {
                const double cov = std::min(static_cast<double>(py) + 1.0, hi)
                                 - std::max(static_cast<double>(py), lo);
                if (cov <= 0.0) continue;
                QRgb *px = reinterpret_cast<QRgb *>(out.scanLine(py)) + s;
                const QRgb src = premulScale(
                    c, static_cast<int>(cov * 256.0 + 0.5));
                *px = premulOver(src, *px);
            }
            yPrev = yCur;
            yCur = yNext;
        }
    };

    if (heatmap) {
        // Warm off-white primary outline reads over all inferno tones.
        const QVector<float> &from   = haveOut ? m_outSpectrumFromY   : m_inSpectrumFromY;
        const QVector<float> &target = haveOut ? m_outSpectrumTargetY : m_inSpectrumTargetY;
        blendOutline(from, target, qPremultiply(qRgba(255, 230, 160, 200)));
        if (haveOut && haveIn)
            blendOutline(m_inSpectrumFromY, m_inSpectrumTargetY,
                         qPremultiply(qRgba(0x4F, 0xC1, 0xE9, 191)));  // αF 0.75
    } else {
        if (haveIn)
            blendOutline(m_inSpectrumFromY, m_inSpectrumTargetY,
                         qPremultiply(qRgba(0x4F, 0xC1, 0xE9, 140)));  // αF 0.55
        if (haveOut)
            blendOutline(m_outSpectrumFromY, m_outSpectrumTargetY,
                         qPremultiply(qRgba(0xE6, 0x7E, 0x22, 153)));  // αF 0.60
    }

    m_spectrumImageTop = minTop;
}


void EqCurve::paintEvent(QPaintEvent *)
{
    ++g_fpsProbe.paints;
    g_fpsProbe.plotW = m_cachedPlotWidth;
    g_fpsProbe.plotH = m_cachedPlotHeight;
    g_fpsProbe.flush();
    QElapsedTimer perf;
    if (g_fpsProbe.enabled) perf.start();

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

    const qint64 perfBg = g_fpsProbe.enabled ? perf.nsecsElapsed() : 0;

    const QRectF r = plotRect();
    const double spectrumAlpha = spectrumInterpolationAlpha();
    // Note: interpolation completing (alpha == 1) does NOT stop the animation
    // timer — the next FFT target usually arrives within a frame or two, and
    // stop/start churn costs a full OS timer quantum per restart. The timeout
    // lambda stops the timer once targets go stale.

    // Clip to the plot area so spectra don't bleed.
    p.save();
    p.setClipRect(r);

    // --- Spectra ---
    // Everything that moves per frame — fills, heatmap, and outlines — is
    // rasterized into m_spectrumImage by ensureSpectrumImage. Integer-aligned
    // drawImage keeps the blit on the fast path; fractional offsets push Qt's
    // raster engine through per-pixel filtering.
    const bool haveIn  = m_showInputSpectrum  && !m_inSpectrumTargetY.isEmpty();
    const bool haveOut = m_showOutputSpectrum && !m_outSpectrumTargetY.isEmpty();
    const int source = (haveIn ? 1 : 0) | (haveOut ? 2 : 0)
                     | (m_showHeatmap ? 4 : 0);
    if ((source & 3) != 0) {
        ensureSpectrumImage(source, spectrumAlpha);
        const qint64 tEnsure = g_fpsProbe.enabled ? perf.nsecsElapsed() : 0;
        if (!m_spectrumImage.isNull()) {
            const int top = std::clamp(m_spectrumImageTop, 0,
                                       m_spectrumImage.height());
            const int rows = m_spectrumImage.height() - top;
            if (rows > 0) {
                p.setRenderHint(QPainter::Antialiasing, false);
                p.drawImage(QPoint(static_cast<int>(r.left()),
                                   static_cast<int>(r.top()) + top),
                            m_spectrumImage,
                            QRect(0, top, m_spectrumImage.width(), rows));
                p.setRenderHint(QPainter::Antialiasing, true);
            }
        }
        if (g_fpsProbe.enabled) {
            g_fpsProbe.nsEnsure += tEnsure - perfBg;
            g_fpsProbe.nsBlit += perf.nsecsElapsed() - tEnsure;
        }
    }

    p.restore();

    const qint64 perfSpec = g_fpsProbe.enabled ? perf.nsecsElapsed() : 0;

    p.drawPixmap(0, 0, m_gridLayer);

    if (m_responseCurvesDirty) {
        const qint64 t0 = g_fpsProbe.enabled ? perf.nsecsElapsed() : 0;
        rebuildResponseCurves();
        if (g_fpsProbe.enabled)
            g_fpsProbe.nsResp += perf.nsecsElapsed() - t0;
    }
    if (m_responseLayerDirty
        || m_responseLayerHover != m_hoverBand
        || m_responseLayerDrag != m_draggingBand
        || m_responseLayer.size() != expectedPixels
        || m_responseLayer.devicePixelRatio() != dpr) {
        const qint64 t0 = g_fpsProbe.enabled ? perf.nsecsElapsed() : 0;
        rebuildResponseLayer(dpr, expectedPixels);
        if (g_fpsProbe.enabled)
            g_fpsProbe.nsLayer += perf.nsecsElapsed() - t0;
    }
    p.drawPixmap(0, 0, m_responseLayer);

    if (g_fpsProbe.enabled) {
        const qint64 total = perf.nsecsElapsed();
        g_fpsProbe.nsBg += perfBg;
        g_fpsProbe.nsSpec += perfSpec - perfBg;
        g_fpsProbe.nsRest += total - perfSpec;
    }
}

void EqCurve::rebuildResponseLayer(qreal dpr, const QSize &pixelSize)
{
    if (m_responseLayer.size() != pixelSize
        || m_responseLayer.devicePixelRatio() != dpr) {
        m_responseLayer = QPixmap(pixelSize);
        m_responseLayer.setDevicePixelRatio(dpr);
    }
    m_responseLayer.fill(Qt::transparent);

    QPainter p(&m_responseLayer);
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(font());

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

    ++g_fpsProbe.layerRebuilds;
    m_responseLayerHover = m_hoverBand;
    m_responseLayerDrag = m_draggingBand;
    m_responseLayerDirty = false;
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
