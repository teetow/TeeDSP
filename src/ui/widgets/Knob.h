#pragma once

#include <QString>
#include <QWidget>

namespace ui {

// Compact rotary knob. Vertical drag adjusts value; double-click resets to
// default; wheel also adjusts (Shift = fine). Supports linear and log scales.
class Knob : public QWidget
{
    Q_OBJECT

public:
    enum class Scale { Linear, Log };

    explicit Knob(QWidget *parent = nullptr);

    void setRange(double min, double max, Scale scale = Scale::Linear);
    void setDefaultValue(double v);
    void setValue(double v);
    double value() const { return m_value; }

    void setLabel(const QString &text);    // e.g. "Threshold"
    void setUnit(const QString &text);     // e.g. "dB"
    void setDecimals(int decimals);        // value label precision

    QSize sizeHint() const override { return {72, 92}; }
    QSize minimumSizeHint() const override { return {64, 84}; }

signals:
    void valueChanged(double v);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    double normFromValue(double v) const;    // [0,1]
    double valueFromNorm(double n) const;
    void setNormalized(double n);
    QString formatValue() const;

    double m_min = 0.0;
    double m_max = 1.0;
    double m_value = 0.0;
    double m_default = 0.0;
    Scale m_scale = Scale::Linear;

    QString m_label;
    QString m_unit;
    int m_decimals = 2;

    bool m_dragging = false;
    QPoint m_dragStart;
    double m_dragStartNorm = 0.0;
    bool m_shiftHeld = false;
};

} // namespace ui
