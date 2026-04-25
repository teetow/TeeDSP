#include "LevelMeter.h"

#include "../Theme.h"

#include <QDateTime>
#include <QPainter>

#include <cmath>

namespace ui {

namespace {
constexpr qint64 kPeakHoldMs = 800;
constexpr double kPeakFalloffDbPerMs = 0.02;
}

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void LevelMeter::setLevelDbfs(double dbfs)
{
    // Map -60..0 dBFS → 0..kMaxDb
    const double mapped = std::max(0.0, (dbfs + 60.0) * (kMaxDb / 60.0));
    setReductionDb(mapped);
}

void LevelMeter::setBarColor(BarColor c)
{
    if (m_barColor == c) return;
    m_barColor = c;
    update();
}

void LevelMeter::setReductionDb(double db)
{
    db = std::max(0.0, std::min(kMaxDb, db));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double prevValue = m_valueDb;
    const double prevPeak = m_peakDb;

    // Global meter feel: instant attack, ~60 ms exponential release.
    constexpr double kReleaseTauMs = 60.0;
    double dtMs = (m_lastUpdateMs > 0)
        ? static_cast<double>(now - m_lastUpdateMs)
        : 8.0;
    if (dtMs <= 0.0) dtMs = 1.0;
    m_lastUpdateMs = now;
    const double alpha = 1.0 - std::exp(-dtMs / kReleaseTauMs);
    if (db > m_valueDb) m_valueDb = db;
    else                m_valueDb += alpha * (db - m_valueDb);

    if (db > m_peakDb) {
        m_peakDb = db;
        m_peakStampMs = now;
    } else if (m_peakStampMs > 0) {
        const qint64 dt = now - m_peakStampMs;
        if (dt > kPeakHoldMs) {
            const double drop = (dt - kPeakHoldMs) * kPeakFalloffDbPerMs;
            m_peakDb = std::max(m_valueDb, m_peakDb - drop);
            m_peakStampMs = now;
        }
    }

    // Skip the repaint when nothing visibly changed — the meter is polled at
    // ~125 Hz, but most ticks have idle audio and no peak motion.
    if (std::abs(m_valueDb - prevValue) < 1e-3 &&
        std::abs(m_peakDb - prevPeak)  < 1e-3) {
        return;
    }
    update();
}

void LevelMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRectF full(0.5, 0.5, width() - 1.0, height() - 1.0);

    // Sunken well
    p.setPen(QPen(theme::kBorderSoft, 1.0));
    p.setBrush(theme::kBgSunken);
    p.drawRoundedRect(full, 3, 3);

    if (m_valueDb <= 0.0 && m_peakDb <= 0.0) return;

    const double frac = std::min(1.0, m_valueDb / kMaxDb);
    const double peakFrac = std::min(1.0, m_peakDb / kMaxDb);

    // Fill — color depends on bar role.
    QBrush fillBrush;
    if (m_barColor == BarColor::Input) {
        fillBrush = theme::kAccent;                     // blue
    } else if (m_barColor == BarColor::Output) {
        fillBrush = theme::kOk;                         // green
    } else {
        // GainReduction — amber/yellow
        fillBrush = QColor(0xF1, 0xC4, 0x0F);
    }

    QRectF fillRect = full.adjusted(1.5, 1.5, 0, -1.5);
    fillRect.setWidth((full.width() - 2.0) * frac);
    p.setPen(Qt::NoPen);
    p.setBrush(fillBrush);
    p.drawRoundedRect(fillRect, 2, 2);

    // Peak tick
    if (peakFrac > 0.0 && m_barColor != BarColor::GainReduction) {
        const double xRaw = full.left() + 1.5 + (full.width() - 2.0) * peakFrac;
        // Snap to pixel center with a cosmetic 1px pen to keep visual width
        // consistent as the peak moves.
        const double x = std::floor(xRaw) + 0.5;
        QPen tickPen(theme::kTextPrimary, 1.0);
        tickPen.setCosmetic(true);
        p.setPen(tickPen);
        p.drawLine(QPointF(x, full.top() + 1.5),
                   QPointF(x, full.bottom() - 1.5));
    }
}

} // namespace ui
