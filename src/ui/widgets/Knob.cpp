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
constexpr double kStartAngleDeg = 135.0;     // bottom-left
constexpr double kSweepAngleDeg = 270.0;     // full travel
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

void Knob::setNormalized(double n)
{
    setValue(valueFromNorm(n));
}

QString Knob::formatValue() const
{
    QString s = QString::number(m_value, 'f', m_decimals);
    if (!m_unit.isEmpty()) s += QLatin1Char(' ') + m_unit;
    return s;
}

void Knob::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int w = width();
    const int h = height();

    // Layout: label (top), knob circle (middle), value (bottom).
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

    // --- Background arc (full sweep) ---
    QPen bgPen(theme::kBorderSoft, 3.0, Qt::SolidLine, Qt::FlatCap);
    p.setPen(bgPen);
    p.setBrush(Qt::NoBrush);
    // Qt arc API: startAngle and spanAngle in 1/16ths of a degree, CCW from 3 o'clock.
    // Our 135° origin is lower-left; span 270° going CCW reaches lower-right.
    const int startA = static_cast<int>((270.0 - kStartAngleDeg) * 16.0);       // -> 135*16 CCW origin
    const int spanA  = static_cast<int>(-kSweepAngleDeg * 16.0);
    p.drawArc(knobRect.adjusted(4, 4, -4, -4), startA, spanA);

    // --- Filled arc up to current value ---
    const double n = std::max(0.0, std::min(1.0, normFromValue(m_value)));
    QPen fgPen(theme::kAccent, 3.0, Qt::SolidLine, Qt::FlatCap);
    p.setPen(fgPen);
    const int spanValA = static_cast<int>(-kSweepAngleDeg * n * 16.0);
    p.drawArc(knobRect.adjusted(4, 4, -4, -4), startA, spanValA);

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
    const double angleDeg = kStartAngleDeg - kSweepAngleDeg * n; // CCW sweep: -270*n
    // Convert: kStartAngleDeg=135 means lower-left; sweeping CCW (negative) goes to lower-right at n=1
    // We want: n=0 -> lower-left, n=1 -> lower-right. The math below uses standard math coords (CCW positive).
    // Rework: pointer should sweep from 225° (lower-left) to -45° (lower-right) going CW.
    const double sweepStartMath = 225.0;                 // lower-left in math coords (0° = 3 o'clock, CCW)
    const double sweepEndMath   = -45.0;                 // lower-right
    const double angle = sweepStartMath + (sweepEndMath - sweepStartMath) * n;
    const double rad = angle * M_PI / 180.0;
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
    const double dy = static_cast<double>(m_dragStart.y() - e->pos().y()); // up = positive
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
