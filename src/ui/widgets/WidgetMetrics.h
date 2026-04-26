#pragma once

namespace ui::widget_metrics {

namespace knob {
constexpr int kLabelHeightPx = 14;
constexpr int kValueHeightPx = 14;
constexpr int kVerticalGapPx = 0;
constexpr int kOuterInsetPx = 8;
constexpr int kKnobTopOffsetPx = 2;
constexpr int kArcInsetPx = 4;
constexpr double kArcThicknessPx = 3.0;
constexpr double kBipolarTickOuterOffsetPx = 1.5;
constexpr double kBipolarTickInnerOffsetPx = 4.5;
constexpr double kBipolarTickThicknessPx = 1.4;
constexpr double kDialInsetPx = 8.0;
constexpr double kPointerTipInsetPx = 3.0;
constexpr double kPointerInnerFrac = 0.45;
constexpr double kPointerThicknessPx = 2.5;
constexpr double kLabelFontPt = 7.5;
constexpr double kValueFontPt = 8.5;
} // namespace knob

namespace level_meter {
constexpr double kOuterStrokePx = 1.0;
constexpr double kCornerRadiusPx = 3.0;
constexpr double kFillInsetLeftPx = 1.5;
constexpr double kFillInsetTopPx = 1.5;
constexpr double kFillInsetBottomPx = 1.5;
constexpr double kFillRadiusPx = 2.0;
constexpr double kPeakTickThicknessPx = 1.0;
constexpr double kPeakTickInsetPx = 1.5;
constexpr double kReleaseTauMs = 60.0;
} // namespace level_meter

namespace meter_runtime {
constexpr float kReleaseTauMs = 60.0f;
constexpr float kInitialDtMs = 8.0f;

constexpr float kDbMeterMin = -90.0f;
constexpr float kDbMeterMax = 0.0f;
constexpr float kLufsMeterMin = -70.0f;
constexpr float kLufsMeterMax = 0.0f;

constexpr float kSilenceDisplayDbfs = -100.0f;
constexpr float kLufsDisplayFloor = -69.0f;

constexpr float kSpectrumFalloffTauMs = 300.0f;
} // namespace meter_runtime

} // namespace ui::widget_metrics
