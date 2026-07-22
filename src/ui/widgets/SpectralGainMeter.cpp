#include "SpectralGainMeter.h"

#include "../Theme.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr float kMaxGainDb = 6.0f;
}

SpectralGainMeter::SpectralGainMeter(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void SpectralGainMeter::setGainsDb(const std::array<float, kBandCount> &gains, bool active)
{
    std::array<float, kBandCount> clamped{};
    for (int b = 0; b < kBandCount; ++b)
        clamped[b] = std::clamp(gains[b], -kMaxGainDb, kMaxGainDb);

    if (m_active == active && clamped == m_gains)
        return;
    m_gains = clamped;
    m_active = active;
    update();
}

void SpectralGainMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRectF well(0.5, 0.5, width() - 1.0, height() - 1.0);
    p.setPen(QPen(theme::kBorderSoft, 1.0));
    p.setBrush(theme::kBgSunken);
    p.drawRoundedRect(well, 3.0, 3.0);

    const QRectF chart = well.adjusted(4.0, 3.0, -4.0, -3.0);
    const double centreY = std::floor(chart.center().y()) + 0.5;
    p.setPen(QPen(theme::kTextDim, 1.0));
    p.drawLine(QPointF(chart.left(), centreY), QPointF(chart.right(), centreY));

    const double slotWidth = chart.width() / kBandCount;
    const double barWidth = std::max(2.0, slotWidth - 3.0);
    const double halfHeight = std::max(1.0, (chart.height() * 0.5) - 1.0);
    for (int b = 0; b < kBandCount; ++b) {
        const double x = chart.left() + slotWidth * b + (slotWidth - barWidth) * 0.5;
        const float gain = m_active ? m_gains[b] : 0.0f;
        const double h = std::abs(gain) / kMaxGainDb * halfHeight;
        if (h > 0.1) {
            const QRectF bar(x, gain >= 0.0f ? centreY - h : centreY, barWidth, h);
            p.setPen(Qt::NoPen);
            p.setBrush(gain >= 0.0f ? theme::kAccent : theme::kWarn);
            p.drawRect(bar);
        }
    }
}

} // namespace ui
