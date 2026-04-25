#include "EqCurve.h"

#include "../Theme.h"

#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

constexpr double kBandHitRadius = 14.0;
constexpr double kBandDrawRadius = 8.0;

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

double magDbAt(const BiquadCoefs &c, double hz, double sr)
{
    const double w = 2.0 * M_PI * hz / sr;
    const double cw  = std::cos(w);
    const double cw2 = std::cos(2.0 * w);
    const double sw  = std::sin(w);
    const double sw2 = std::sin(2.0 * w);

    const double numRe = c.b0 + c.b1 * cw + c.b2 * cw2;
    const double numIm = -(c.b1 * sw + c.b2 * sw2);
    const double denRe = 1.0 + c.a1 * cw + c.a2 * cw2;
    const double denIm = -(c.a1 * sw + c.a2 * sw2);

    const double numMag2 = numRe * numRe + numIm * numIm;
    const double denMag2 = denRe * denRe + denIm * denIm;
    if (denMag2 < 1e-30 || numMag2 < 1e-30) return -120.0;
    return 10.0 * std::log10(numMag2 / denMag2);
}

} // namespace

EqCurve::EqCurve(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
}

void EqCurve::setSampleRate(double sr)    { m_sampleRate = sr; update(); }
void EqCurve::setBands(const QVector<EqBandData> &bands) { m_bands = bands; update(); }
void EqCurve::setEqEnabled(bool enabled)  { m_eqEnabled = enabled; update(); }

void EqCurve::setInputSpectrum(const QVector<float> &mag, double sr, int /*fftSize*/)
{
    m_inSpec = mag;
    m_inSpecSr = sr;
    update();
}

void EqCurve::setOutputSpectrum(const QVector<float> &mag, double sr, int /*fftSize*/)
{
    m_outSpec = mag;
    m_outSpecSr = sr;
    update();
}

void EqCurve::clearSpectra()
{
    m_inSpec.clear();
    m_outSpec.clear();
    update();
}

void EqCurve::setShowInputSpectrum(bool show)  { m_showInputSpectrum = show;  update(); }
void EqCurve::setShowOutputSpectrum(bool show) { m_showOutputSpectrum = show; update(); }

void EqCurve::setShowHeatmap(bool show)
{
    if (m_showHeatmap == show) return;
    m_showHeatmap = show;
    update();
}

void EqCurve::pushHeatmapFrame(const QVector<float> &magDb, double sr)
{
    if (magDb.isEmpty()) return;
    m_heatmapSr = sr;
    m_heatmapFrames.push_back(magDb);
    while (static_cast<int>(m_heatmapFrames.size()) > kHeatmapHistory)
        m_heatmapFrames.pop_front();
    if (m_showHeatmap) update();
}

void EqCurve::clearHeatmap()
{
    m_heatmapFrames.clear();
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

double EqCurve::bandMagDb(const EqBandData &b, double hz, bool includeDynamic) const
{
    if (!b.enabled) return 0.0;
    const double dynamicOffsetDb = includeDynamic ? b.dynGainReductionDb : 0.0;
    const double effectiveGainDb = b.gainDb - dynamicOffsetDb;
    const BiquadCoefs c = coefsFor(b.type, b.freqHz, b.q, effectiveGainDb, m_sampleRate);
    return magDbAt(c, hz, m_sampleRate);
}

double EqCurve::combinedMagDb(double hz, bool includeDynamic) const
{
    if (!m_eqEnabled) return 0.0;
    double sum = 0.0;
    for (const auto &b : m_bands) sum += bandMagDb(b, hz, includeDynamic);
    return sum;
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

void EqCurve::drawSpectrum(QPainter &p, const QVector<float> &mag,
                           double specSr, const QColor &fill,
                           const QColor &stroke) const
{
    if (mag.isEmpty()) return;
    const QRectF r = plotRect();

    QPainterPath path;
    bool started = false;
    const int bins = mag.size();
    if (bins < 2) return;
    const double binHz = specSr / static_cast<double>(2 * (bins - 1));

    // Walk the plot's X resolution and pick the matching bin (or interpolate).
    const int steps = static_cast<int>(r.width());
    for (int s = 0; s <= steps; ++s) {
        const double x = r.left() + s;
        const double f = std::clamp(xToFreq(x), kFreqMin, kFreqMax);
        const double bin = f / binHz;
        const int b0 = std::clamp(static_cast<int>(std::floor(bin)), 0, bins - 1);
        const int b1 = std::min(b0 + 1, bins - 1);
        const double t = bin - b0;
        const double db = mag[b0] * (1.0 - t) + mag[b1] * t;
        const double y = specDbToY(db);
        if (!started) { path.moveTo(x, y); started = true; }
        else          { path.lineTo(x, y); }
    }

    QPainterPath fillPath = path;
    fillPath.lineTo(r.right(), r.bottom());
    fillPath.lineTo(r.left(),  r.bottom());
    fillPath.closeSubpath();

    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(fillPath);

    p.setPen(QPen(stroke, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void EqCurve::drawSpectrumHeatmap(QPainter &p, const QVector<float> &mag,
                                   double specSr) const
{
    if (mag.isEmpty()) return;
    const QRectF r = plotRect();
    const int bins = mag.size();
    if (bins < 2) return;
    const double binHz = specSr / static_cast<double>(2 * (bins - 1));
    const float invRange = 1.0f / static_cast<float>(kSpecDbMax - kSpecDbMin);

    const int steps = static_cast<int>(r.width());

    // Build outline path while drawing per-column inferno bars in one pass.
    QPainterPath outline;
    p.setRenderHint(QPainter::Antialiasing, false);
    for (int s = 0; s <= steps; ++s) {
        const double x = r.left() + s;
        const double f = std::clamp(xToFreq(x), kFreqMin, kFreqMax);
        const double bin = f / binHz;
        const int b0 = std::clamp(static_cast<int>(std::floor(bin)), 0, bins - 1);
        const int b1 = std::min(b0 + 1, bins - 1);
        const double frac = bin - b0;
        const double db = mag[b0] * (1.0 - frac) + mag[b1] * frac;
        const double y  = specDbToY(db);

        const float u = (static_cast<float>(db) - static_cast<float>(kSpecDbMin)) * invRange;
        p.setPen(QPen(QColor(infernoRgb(u)), 1.0, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(x, y), QPointF(x, r.bottom() + 1.0));

        if (s == 0) outline.moveTo(x, y);
        else        outline.lineTo(x, y);
    }

    // Spectrum outline on top — warm off-white so it reads over all inferno tones.
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(255, 230, 160, 200), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(outline);
}

void EqCurve::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF full(0.5, 0.5, width() - 1.0, height() - 1.0);

    p.setPen(QPen(theme::kBorderSoft, 1.0));
    p.setBrush(theme::kBgSunken);
    p.drawRoundedRect(full, 5, 5);

    const QRectF r = plotRect();

    // Clip subsequent fills/lines to the plot area so spectra don't bleed.
    p.save();
    p.setClipRect(r);

    // --- Spectra ---
    // When heatmap mode is on, each spectrum column is filled with an
    // inferno-gradient bar whose color encodes its magnitude — peaks pop out
    // immediately. When off, the conventional flat-fill + stroke is used.
    if (m_showInputSpectrum && !m_inSpec.isEmpty()) {
        if (m_showHeatmap) {
            drawSpectrumHeatmap(p, m_inSpec, m_inSpecSr);
        } else {
            QColor fill(0x4F, 0xC1, 0xE9);  fill.setAlphaF(0.18);
            QColor stroke(0x4F, 0xC1, 0xE9); stroke.setAlphaF(0.55);
            drawSpectrum(p, m_inSpec, m_inSpecSr, fill, stroke);
        }
    }
    if (m_showOutputSpectrum && !m_outSpec.isEmpty()) {
        if (m_showHeatmap) {
            drawSpectrumHeatmap(p, m_outSpec, m_outSpecSr);
        } else {
            QColor fill(0xE6, 0x7E, 0x22); fill.setAlphaF(0.20);
            QColor stroke(0xE6, 0x7E, 0x22); stroke.setAlphaF(0.60);
            drawSpectrum(p, m_outSpec, m_outSpecSr, fill, stroke);
        }
    }

    p.restore();

    // --- Grid: vertical freq lines + labels ---
    p.setPen(QPen(theme::kBorderSoft, 1.0, Qt::SolidLine));
    const double freqTicks[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
    QFont fLabel = p.font();
    fLabel.setPointSizeF(7.5);
    p.setFont(fLabel);
    for (double f : freqTicks) {
        const double x = freqToX(f);
        if (x < r.left() + 1 || x > r.right() - 1) continue;
        p.setPen(QPen(theme::kBorderSoft, 1.0, Qt::DotLine));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(theme::kTextDim);
        QString lbl = (f >= 1000.0) ? QStringLiteral("%1k").arg(f / 1000.0, 0, 'g', 2)
                                    : QStringLiteral("%1").arg(f, 0, 'f', 0);
        p.drawText(QRectF(x - 20, r.bottom() + 2, 40, 14), Qt::AlignCenter, lbl);
    }

    // --- Grid: horizontal dB lines + labels ---
    const double dbTicks[] = {-18, -12, -6, 0, 6, 12, 18};
    for (double db : dbTicks) {
        const double y = gainToY(db);
        if (db == 0.0) {
            p.setPen(QPen(theme::kBorder, 1.2, Qt::SolidLine));
        } else {
            p.setPen(QPen(theme::kBorderSoft, 1.0, Qt::DotLine));
        }
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(theme::kTextDim);
        p.drawText(QRectF(0, y - 8, r.left() - 2, 16),
                   Qt::AlignRight | Qt::AlignVCenter,
                   (db > 0) ? QStringLiteral("+%1").arg(db, 0, 'f', 0)
                            : QStringLiteral("%1").arg(db, 0, 'f', 0));
    }

    // --- Per-band response (faint, in band colour, live dynamic gain included) ---
    if (m_eqEnabled) {
        for (int i = 0; i < m_bands.size(); ++i) {
            const auto &b = m_bands[i];
            if (!b.enabled) continue;
            const BiquadCoefs c = coefsFor(b.type, b.freqHz, b.q, b.gainDb - b.dynGainReductionDb, m_sampleRate);
            QPainterPath path;
            bool started = false;
            const int steps = static_cast<int>(r.width());
            for (int s = 0; s <= steps; ++s) {
                const double x = r.left() + s;
                const double f = xToFreq(x);
                const double y = gainToY(magDbAt(c, f, m_sampleRate));
                if (!started) { path.moveTo(x, y); started = true; }
                else          { path.lineTo(x, y); }
            }
            QColor c2 = bandColor(i);
            c2.setAlphaF(0.30);
            p.setPen(QPen(c2, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    }

    // --- Combined static response (no dynamic GR) ---
    {
        QPainterPath staticPath;
        bool started = false;
        const int steps = static_cast<int>(r.width());
        for (int s = 0; s <= steps; ++s) {
            const double x = r.left() + s;
            const double f = xToFreq(x);
            const double y = gainToY(combinedMagDb(f, false));
            if (!started) { staticPath.moveTo(x, y); started = true; }
            else          { staticPath.lineTo(x, y); }
        }

        QColor staticColor = theme::kTextDim;
        staticColor.setAlphaF(0.55);
        p.setPen(QPen(staticColor, 1.3, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPath(staticPath);
    }

    // --- Combined live response (dynamic GR applied) ---
    {
        QPainterPath path;
        bool started = false;
        const int steps = static_cast<int>(r.width());
        for (int s = 0; s <= steps; ++s) {
            const double x = r.left() + s;
            const double f = xToFreq(x);
            const double y = gainToY(combinedMagDb(f, true));
            if (!started) { path.moveTo(x, y); started = true; }
            else          { path.lineTo(x, y); }
        }
        QColor lineCol = m_eqEnabled ? theme::kAccent : theme::kTextDim;
        p.setPen(QPen(lineCol, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    // --- Right-side detector dB scale labels (spectrum reference) ---
    p.setPen(theme::kTextDim);
    const double scTicks[] = {-60.0, -48.0, -36.0, -24.0, -12.0, 0.0};
    for (double db : scTicks) {
        const double y = specDbToY(db);
        p.drawText(QRectF(r.right() + 2, y - 8, 30, 16),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(db, 'f', 0));
    }

    // --- Band handles ---
    for (int i = 0; i < m_bands.size(); ++i) {
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
        emit bandReset(hit);
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
