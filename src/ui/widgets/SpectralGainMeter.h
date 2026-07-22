#pragma once

#include <QWidget>

#include <array>

namespace ui {

// Ten compact bipolar bars for the speech spectral leveler's live correction,
// low band to high, left to right. A bar above centre is adding that region;
// a bar below centre is attenuating it. The display intentionally uses the
// processor's ±6 dB operating range.
class SpectralGainMeter : public QWidget
{
public:
    static constexpr int kBandCount = 10;

    explicit SpectralGainMeter(QWidget *parent = nullptr);

    void setGainsDb(const std::array<float, kBandCount> &gains, bool active);

    QSize sizeHint() const override { return {150, 48}; }
    QSize minimumSizeHint() const override { return {120, 42}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::array<float, kBandCount> m_gains{};
    bool m_active = false;
};

} // namespace ui
