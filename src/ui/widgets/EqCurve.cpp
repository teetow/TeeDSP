#include "EqCurve.h"

#include "../Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <cmath>

namespace ui {

namespace {

constexpr double kBandHitRadius = 10.0;
constexpr double kBandDrawRadius = 5.5;

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

QRectF EqCurve::plotRect() const
{
    return QRectF(36, 10, width() - 46, height() - 26);
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

double EqCurve::bandMagDb(const EqBandData &b, double hz) const
{
    if (!b.enabled) return 0.0;
    const BiquadCoefs c = coefsFor(b.type, b.freqHz, b.q, b.gainDb, m_sampleRate);
    return magDbAt(c, hz, m_sampleRate);
}

double EqCurve::combinedMagDb(double hz) const
{
    if (!m_eqEnabled) return 0.0;
    double sum = 0.0;
    for (const auto &b : m_bands) sum += bandMagDb(b, hz);
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

void EqCurve::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF full(0.5, 0.5, width() - 1.0, height() - 1.0);

    // Plot well
    p.setPen(QPen(theme::kBorderSoft, 1.0));
    p.setBrush(theme::kBgSunken);
    p.drawRoundedRect(full, 5, 5);

    const QRectF r = plotRect();

    // --- Grid: vertical freq lines ---
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

    // --- Grid: horizontal dB lines ---
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

    // --- Per-band curves (faint) ---
    if (m_eqEnabled) {
        for (int i = 0; i < m_bands.size(); ++i) {
            const auto &b = m_bands[i];
            if (!b.enabled) continue;
            const BiquadCoefs c = coefsFor(b.type, b.freqHz, b.q, b.gainDb, m_sampleRate);
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
            QColor c2 = theme::kAccentDim;
            c2.setAlphaF(0.35);
            p.setPen(QPen(c2, 1.2));
            p.drawPath(path);
        }
    }

    // --- Combined response ---
    {
        QPainterPath path;
        bool started = false;
        const int steps = static_cast<int>(r.width());
        for (int s = 0; s <= steps; ++s) {
            const double x = r.left() + s;
            const double f = xToFreq(x);
            const double y = gainToY(combinedMagDb(f));
            if (!started) { path.moveTo(x, y); started = true; }
            else          { path.lineTo(x, y); }
        }
        // Fill under curve with soft gradient
        QPainterPath fill = path;
        fill.lineTo(r.right(), gainToY(0));
        fill.lineTo(r.left(),  gainToY(0));
        fill.closeSubpath();
        QColor fillCol = theme::kAccent;
        fillCol.setAlphaF(0.12);
        p.setPen(Qt::NoPen);
        p.setBrush(fillCol);
        p.drawPath(fill);

        QColor lineCol = m_eqEnabled ? theme::kAccent : theme::kTextDim;
        p.setPen(QPen(lineCol, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    // --- Band handles ---
    for (int i = 0; i < m_bands.size(); ++i) {
        const auto &b = m_bands[i];
        const QPointF bp(freqToX(b.freqHz), gainToY(b.gainDb));
        const bool dragging = (i == m_draggingBand);
        const bool hover    = (i == m_hoverBand);
        const bool engaged  = m_eqEnabled && b.enabled;

        QColor fill = engaged ? theme::kAccent : theme::kTextDim;
        if (dragging) fill = theme::kWarn;

        p.setPen(QPen(theme::kBgDeep, 2.0));
        p.setBrush(fill);
        const double rad = hover || dragging ? kBandDrawRadius + 1.5 : kBandDrawRadius;
        p.drawEllipse(bp, rad, rad);

        p.setPen(theme::kBgDeep);
        QFont lf = p.font();
        lf.setPointSizeF(7.0);
        lf.setBold(true);
        p.setFont(lf);
        p.drawText(QRectF(bp.x() - 8, bp.y() - 8, 16, 16), Qt::AlignCenter,
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

} // namespace ui
