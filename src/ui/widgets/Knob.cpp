#include "Knob.h"

#include "../Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWheelEvent>

#include <cmath>

namespace ui {

namespace {

// Arc geometry — Qt/math convention (CCW from 3 o'clock).
//   start = 225°  (lower-left, 7:30)
//   sweep = -270° (CW direction, going over the top to lower-right)
//   end   = -45°  (lower-right, 4:30)
//   mid   = 90°   (12 o'clock — used as the bipolar origin angle)
constexpr double kArcStartDeg = 225.0;
constexpr double kArcSweepDeg = -270.0;
constexpr double kArcMidDeg   = kArcStartDeg + kArcSweepDeg * 0.5; // = 90

constexpr double kDragPixelsForFullRange = 220.0;
constexpr double kFineDragMultiplier = 0.2;

} // namespace

Knob::Knob(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void Knob::setRange(double min, double max, Scale scale)
{
    m_min = min;
    m_max = max;
    m_scale = scale;
    if (m_value < m_min) m_value = m_min;
    if (m_value > m_max) m_value = m_max;
    if (!m_bipolarOriginExplicit)
        m_bipolarOrigin = (m_min + m_max) * 0.5;
    update();
}

void Knob::setDefaultValue(double v) { m_default = v; }

void Knob::setValue(double v)
{
    v = std::max(m_min, std::min(m_max, v));
    if (std::abs(v - m_value) < 1e-9) return;
    m_value = v;
    update();
    emit valueChanged(m_value);
}

void Knob::setLabel(const QString &text) { m_label = text; update(); }
void Knob::setUnit(const QString &text)  { m_unit = text;  update(); }
void Knob::setDecimals(int d)            { m_decimals = d; update(); }

void Knob::setPolarity(Polarity p)
{
    if (m_polarity == p) return;
    m_polarity = p;
    update();
}

void Knob::setBipolarOrigin(double v)
{
    m_bipolarOrigin = v;
    m_bipolarOriginExplicit = true;
    update();
}

double Knob::normFromValue(double v) const
{
    if (m_max <= m_min) return 0.0;
    if (m_scale == Scale::Log && m_min > 0.0) {
        return (std::log(v) - std::log(m_min)) / (std::log(m_max) - std::log(m_min));
    }
    return (v - m_min) / (m_max - m_min);
}

double Knob::valueFromNorm(double n) const
{
    n = std::max(0.0, std::min(1.0, n));
    if (m_scale == Scale::Log && m_min > 0.0) {
        return std::exp(std::log(m_min) + n * (std::log(m_max) - std::log(m_min)));
    }
    return m_min + n * (m_max - m_min);
}

void Knob::setNormalized(double n) { setValue(valueFromNorm(n)); }

QString Knob::formatValue() const
{
    QString s = QString::number(m_value, 'f', m_decimals);
    if (!m_unit.isEmpty()) s += QLatin1Char(' ') + m_unit;
    return s;
}

// Maps a value to its angular position on the arc. Unipolar: linear in
// normalized value space across the full sweep. Bipolar: split at 12 o'clock
// so each side scales independently — handy for asymmetric ranges where the
// origin isn't the geometric midpoint.
double Knob::angleForValue(double v) const
{
    if (m_polarity == Polarity::Unipolar) {
        return kArcStartDeg + kArcSweepDeg * normFromValue(v);
    }
    if (v >= m_bipolarOrigin) {
        const double range = m_max - m_bipolarOrigin;
        if (range <= 0.0) return kArcMidDeg;
        const double frac = (v - m_bipolarOrigin) / range;
        return kArcMidDeg + (kArcSweepDeg * 0.5) * frac;     // toward lower-right
    }
    const double range = m_bipolarOrigin - m_min;
    if (range <= 0.0) return kArcMidDeg;
    const double frac = (m_bipolarOrigin - v) / range;
    return kArcMidDeg - (kArcSweepDeg * 0.5) * frac;         // toward lower-left
}

void Knob::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int w = width();
    const int h = height();
    const int labelH = 14;
    const int valueH = 14;
    const int knobArea = h - labelH - valueH - 4;
    const int knobSize = std::min(knobArea, w) - 8;
    if (knobSize <= 8) return;

    const QRectF knobRect(
        (w - knobSize) / 2.0,
        labelH + 2,
        knobSize,
        knobSize);

    // --- Label on top ---
    p.setPen(theme::kTextSecondary);
    QFont f = p.font();
    f.setPointSizeF(7.5);
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::PercentageSpacing, 105);
    p.setFont(f);
    p.drawText(QRectF(0, 0, w, labelH), Qt::AlignCenter, m_label);

    // --- Background arc (full sweep, unfilled track) ---
    QPen bgPen(theme::kBorderSoft, 3.0, Qt::SolidLine, Qt::FlatCap);
    p.setPen(bgPen);
    p.setBrush(Qt::NoBrush);
    const QRectF arcRect = knobRect.adjusted(4, 4, -4, -4);
    p.drawArc(arcRect,
              static_cast<int>(kArcStartDeg * 16.0),
              static_cast<int>(kArcSweepDeg * 16.0));

    // --- Filled arc up to current value ---
    const double valueAngle = angleForValue(m_value);
    const double originAngle = (m_polarity == Polarity::Unipolar) ? kArcStartDeg : kArcMidDeg;
    const double fillSpanDeg = valueAngle - originAngle;
    if (std::abs(fillSpanDeg) > 0.05) {
        QPen fgPen(theme::kAccent, 3.0, Qt::SolidLine, Qt::FlatCap);
        p.setPen(fgPen);
        p.drawArc(arcRect,
                  static_cast<int>(originAngle * 16.0),
                  static_cast<int>(fillSpanDeg * 16.0));
    }

    // For bipolar, draw a small tick at 12 o'clock so the centre is unambiguous
    // even at value == origin (when there's no fill to look at).
    if (m_polarity == Polarity::Bipolar) {
        const double rad = kArcMidDeg * M_PI / 180.0;
        const QPointF center = arcRect.center();
        const double r = arcRect.width() / 2.0;
        const QPointF outer(center.x() + std::cos(rad) * (r + 1.5),
                            center.y() - std::sin(rad) * (r + 1.5));
        const QPointF inner(center.x() + std::cos(rad) * (r - 4.5),
                            center.y() - std::sin(rad) * (r - 4.5));
        p.setPen(QPen(theme::kTextDim, 1.4, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(inner, outer);
    }

    // --- Inner dial + centre pointer ---
    const QPointF center = knobRect.center();
    const double radius = knobRect.width() / 2.0 - 8.0;

    QRadialGradient grad(center, radius);
    grad.setColorAt(0.0, QColor(0x2A, 0x2C, 0x33));
    grad.setColorAt(1.0, QColor(0x16, 0x17, 0x1B));
    p.setBrush(grad);
    p.setPen(QPen(theme::kBorder, 1.0));
    p.drawEllipse(center, radius, radius);

    // Pointer
    const double rad = valueAngle * M_PI / 180.0;
    const QPointF tip(center.x() + std::cos(rad) * (radius - 3.0),
                      center.y() - std::sin(rad) * (radius - 3.0));
    const QPointF inner(center.x() + std::cos(rad) * (radius * 0.45),
                        center.y() - std::sin(rad) * (radius * 0.45));
    QPen pointerPen(theme::kAccent, 2.5, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pointerPen);
    p.drawLine(inner, tip);

    // --- Value label below ---
    p.setPen(theme::kTextPrimary);
    QFont vf = p.font();
    vf.setCapitalization(QFont::MixedCase);
    vf.setPointSizeF(8.5);
    vf.setLetterSpacing(QFont::PercentageSpacing, 100);
    p.setFont(vf);
    p.drawText(QRectF(0, h - valueH, w, valueH), Qt::AlignCenter, formatValue());
}

void Knob::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragStart = e->pos();
        m_dragStartNorm = normFromValue(m_value);
        m_shiftHeld = e->modifiers().testFlag(Qt::ShiftModifier);
        setCursor(Qt::BlankCursor);
        e->accept();
    }
}

void Knob::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_dragging) return;
    m_shiftHeld = e->modifiers().testFlag(Qt::ShiftModifier);
    const double dy = static_cast<double>(m_dragStart.y() - e->pos().y());
    const double sensitivity = m_shiftHeld ? kFineDragMultiplier : 1.0;
    const double dn = (dy / kDragPixelsForFullRange) * sensitivity;
    setNormalized(m_dragStartNorm + dn);
}

void Knob::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(Qt::PointingHandCursor);
        e->accept();
    }
}

void Knob::mouseDoubleClickEvent(QMouseEvent *)
{
    setValue(m_default);
}

void Knob::wheelEvent(QWheelEvent *e)
{
    const double steps = e->angleDelta().y() / 120.0;
    const double shiftMul = e->modifiers().testFlag(Qt::ShiftModifier) ? 0.1 : 1.0;
    const double dn = steps * 0.02 * shiftMul;
    setNormalized(normFromValue(m_value) + dn);
    e->accept();
}

} // namespace ui
