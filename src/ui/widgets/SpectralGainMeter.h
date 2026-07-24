#pragma once

#include <QWidget>

#include <array>

namespace ui {

// A full-width readout of the speech spectral leveler's live correction, one
// bipolar bar per band, low band (left) to high (right). A bar above the
// centre line is adding energy to that region; below is attenuating it. The
// chart is framed by dB reference lines (0, ±3, ±6 — the processor's operating
// range) and labelled with a subset of band centre frequencies; hovering a
// band shows its exact centre frequency and gain.
class SpectralGainMeter : public QWidget
{
public:
    static constexpr int kBandCount = 10;

    explicit SpectralGainMeter(QWidget *parent = nullptr);

    void setGainsDb(const std::array<float, kBandCount> &gains, bool active);

    QSize sizeHint() const override { return {240, 104}; }
    QSize minimumSizeHint() const override { return {160, 88}; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    std::array<float, kBandCount> m_gains{};
    bool m_active = false;
    int m_hoverBand = -1;
};

} // namespace ui
