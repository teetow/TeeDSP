#include "SpectralGainMeter.h"

#include "../Theme.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr float kMaxGainDb = 6.0f;

// Chart insets: room for dB labels on the left and frequency labels below.
constexpr double kLeftAxisPx   = 22.0;
constexpr double kRightPadPx   = 6.0;
constexpr double kTopPadPx     = 4.0;
constexpr double kBottomAxisPx = 14.0;

// Band centre frequencies, matching dsp::SpectralLeveler's detector bands.
constexpr float kBandHz[SpectralGainMeter::kBandCount] = {
    90.0f, 150.0f, 240.0f, 400.0f, 660.0f,
    1100.0f, 1800.0f, 2900.0f, 4800.0f, 8000.0f,
};

// A legible subset to label along the bottom (labelling all ten collides).
constexpr int kLabelBands[] = {0, 3, 5, 7, 9};

QString hzLabel(float hz)
{
    if (hz >= 1000.0f) {
        const float k = hz / 1000.0f;
        if (k == std::floor(k))
            return QString::number(static_cast<int>(k)) + QStringLiteral("k");
        return QString::number(k, 'f', 1) + QStringLiteral("k");
    }
    return QString::number(static_cast<int>(hz));
}

QRectF plotRect(const QWidget *w)
{
    const QRectF well(0.5, 0.5, w->width() - 1.0, w->height() - 1.0);
    return well.adjusted(kLeftAxisPx, kTopPadPx, -kRightPadPx, -kBottomAxisPx);
}
} // namespace

SpectralGainMeter::SpectralGainMeter(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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

    const QRectF plot = plotRect(this);
    if (plot.width() <= 2.0 || plot.height() <= 2.0)
        return;

    const double centreY = std::floor(plot.center().y()) + 0.5;
    const double halfH   = plot.height() * 0.5;

    QFont f = p.font();
    f.setPointSizeF(7.0);
    p.setFont(f);
    const QFontMetricsF fm(f);

    // dB reference lines. 0 is brightest; ±6/±3 fainter. Label only 0/±6 to
    // keep a short chart uncluttered.
    for (int db : {6, 3, 0, -3, -6}) {
        const double y = std::floor(centreY - (double(db) / kMaxGainDb) * halfH) + 0.5;
        p.setPen(QPen(db == 0 ? theme::kTextDim : theme::kBorderSoft, 1.0));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        if (db == 6 || db == 0 || db == -6) {
            const QString t = (db > 0 ? QStringLiteral("+") : QString())
                            + QString::number(db);
            p.setPen(theme::kTextDim);
            p.drawText(QRectF(0.0, y - 7.0, kLeftAxisPx - 3.0, 14.0),
                       Qt::AlignRight | Qt::AlignVCenter, t);
        }
    }

    const double slotWidth  = plot.width() / kBandCount;
    const double barWidth   = std::max(2.0, slotWidth - 4.0);
    const double usableHalf = std::max(1.0, halfH - 1.0);

    for (int b = 0; b < kBandCount; ++b) {
        const double slotLeft = plot.left() + slotWidth * b;

        if (b == m_hoverBand) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 16));
            p.drawRect(QRectF(slotLeft, plot.top(), slotWidth, plot.height()));
        }

        const float gain = m_active ? m_gains[b] : 0.0f;
        const double h = std::abs(gain) / kMaxGainDb * usableHalf;
        if (h > 0.1) {
            const double x = slotLeft + (slotWidth - barWidth) * 0.5;
            const QRectF bar(x, gain >= 0.0f ? centreY - h : centreY, barWidth, h);
            p.setPen(Qt::NoPen);
            p.setBrush(gain >= 0.0f ? theme::kAccent : theme::kWarn);
            p.drawRect(bar);
        }
    }

    // Frequency labels, centred under their slot and clamped to the widget.
    p.setPen(theme::kTextDim);
    for (int b : kLabelBands) {
        const QString t  = hzLabel(kBandHz[b]);
        const double tw  = fm.horizontalAdvance(t) + 2.0;
        const double cx  = plot.left() + slotWidth * (b + 0.5);
        const double tx  = std::clamp(cx - tw * 0.5, 1.0, width() - tw - 1.0);
        p.drawText(QRectF(tx, plot.bottom() + 1.0, tw, kBottomAxisPx - 1.0),
                   Qt::AlignHCenter | Qt::AlignVCenter, t);
    }

    // Hover readout: exact band frequency + gain, top-right of the chart.
    if (m_active && m_hoverBand >= 0 && m_hoverBand < kBandCount) {
        const float g = m_gains[m_hoverBand];
        const QString t = hzLabel(kBandHz[m_hoverBand]) + QStringLiteral("  ")
                        + (g >= 0.0f ? QStringLiteral("+") : QString())
                        + QString::number(g, 'f', 1) + QStringLiteral(" dB");
        p.setPen(g >= 0.0f ? theme::kAccent : theme::kWarn);
        p.drawText(plot.adjusted(2.0, 1.0, -2.0, 0.0),
                   Qt::AlignRight | Qt::AlignTop, t);
    }
}

void SpectralGainMeter::mouseMoveEvent(QMouseEvent *event)
{
    const QRectF plot = plotRect(this);
    const QPointF pos = event->position();
    int band = -1;
    if (plot.width() > 0.0 && pos.x() >= plot.left() && pos.x() < plot.right()
        && pos.y() >= plot.top() && pos.y() <= plot.bottom()) {
        band = static_cast<int>((pos.x() - plot.left()) / (plot.width() / kBandCount));
        band = std::clamp(band, 0, kBandCount - 1);
    }
    if (band != m_hoverBand) {
        m_hoverBand = band;
        update();
    }
}

void SpectralGainMeter::leaveEvent(QEvent *)
{
    if (m_hoverBand != -1) {
        m_hoverBand = -1;
        update();
    }
}

} // namespace ui
