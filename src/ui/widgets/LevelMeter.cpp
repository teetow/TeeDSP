#include "LevelMeter.h"

#include "../Theme.h"

#include <QDateTime>
#include <QLinearGradient>
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

void LevelMeter::setReductionDb(double db)
{
    db = std::max(0.0, std::min(kMaxDb, db));
    if (std::abs(db - m_valueDb) < 1e-3 && std::abs(db - m_peakDb) < 1e-3) {
        // Even if numerically similar, we still want the peak to decay.
        update();
        return;
    }
    m_valueDb = db;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
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

    // Fill — gradient from accent (low GR) to warn (heavy GR).
    QLinearGradient g(full.left(), 0, full.right(), 0);
    g.setColorAt(0.0, theme::kAccent);
    g.setColorAt(0.5, QColor(0xF1, 0xC4, 0x0F));   // amber middle
    g.setColorAt(1.0, theme::kWarn);

    QRectF fillRect = full.adjusted(1.5, 1.5, 0, -1.5);
    fillRect.setWidth((full.width() - 2.0) * frac);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(fillRect, 2, 2);

    // Peak tick
    if (peakFrac > 0.0) {
        const double x = full.left() + 1.5 + (full.width() - 2.0) * peakFrac;
        p.setPen(QPen(theme::kTextPrimary, 1.4));
        p.drawLine(QPointF(x, full.top() + 1.5),
                   QPointF(x, full.bottom() - 1.5));
    }
}

} // namespace ui
