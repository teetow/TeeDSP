#include "BipolarGainMeter.h"
#include "../Theme.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr double kNeutralEpsilonDb = 0.05;
const QColor kNeutralColor{0x5E, 0x63, 0x6E};  // gray — no meaningful correction
const QColor kBoostColor{0x2E, 0xCC, 0x71};    // green — boosting
const QColor kCutColor{0xE7, 0x4C, 0x3C};      // red — cutting
} // namespace

BipolarGainMeter::BipolarGainMeter(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void BipolarGainMeter::setRangeDb(double rangeDb)
{
    m_rangeDb = std::max(0.1, rangeDb);
    update();
}

void BipolarGainMeter::setGainDb(double gainDb)
{
    if (std::abs(gainDb - m_gainDb) < 1e-3) return;
    m_gainDb = gainDb;
    update();
}

void BipolarGainMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    constexpr double kStroke = 1.0;
    const QRectF full(kStroke * 0.5, kStroke * 0.5,
                       width() - kStroke, height() - kStroke);

    // Sunken well, matching the other meters. The 0.5px offset here is the
    // standard trick for a crisp 1px stroke (it centers the stroke on a
    // single pixel row/column).
    p.setPen(QPen(theme::kBorderSoft, kStroke));
    p.setBrush(theme::kBgSunken);
    p.drawRect(full);

    // Fill geometry uses whole-pixel integer coordinates deliberately, never
    // the border's half-pixel offset above: with antialiasing off, a rect
    // whose edge sits exactly on a .5 boundary rasterizes to whichever of
    // two adjacent pixel widths the rounding happens to land on — different
    // frames could (and did) pick different sides, reading as the bar
    // randomly flickering between two widths.
    const int marginX = 1;
    const int fillLeft = marginX;
    const int fillWidth = std::max(0, width() - marginX * 2);
    const int centerY = height() / 2;
    const int halfH = height() / 2 - 1;
    const double frac = std::clamp(m_gainDb / m_rangeDb, -1.0, 1.0);

    p.setPen(Qt::NoPen);
    if (std::abs(m_gainDb) < kNeutralEpsilonDb) {
        // Neutral: a short gray tick at center instead of a colored fill.
        p.setBrush(kNeutralColor);
        p.drawRect(fillLeft, centerY - 2, fillWidth, 5);
        return;
    }

    const int fillH = static_cast<int>(std::lround(std::abs(frac) * halfH));
    if (frac > 0.0) {
        p.setBrush(kBoostColor);
        p.drawRect(fillLeft, centerY - fillH, fillWidth, fillH);
    } else {
        p.setBrush(kCutColor);
        p.drawRect(fillLeft, centerY, fillWidth, fillH);
    }

    // Center reference line, drawn after the fill so it stays visible.
    p.setPen(QPen(theme::kBorderSoft, kStroke));
    p.drawLine(0, centerY, width(), centerY);
}

} // namespace ui
