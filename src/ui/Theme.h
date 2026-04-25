#pragma once

#include <QColor>
#include <QString>

namespace ui {

// Centralized colour palette and stylesheet for the TeeDSP UI.
// Values live here (not in a .qss file) so a single rebuild captures any
// tweak and there is no runtime asset-loading path to maintain.
namespace theme {

// ---- Core palette -----------------------------------------------------------
inline const QColor kBgDeep     { 0x16, 0x17, 0x1B };  // window background
inline const QColor kBgPanel    { 0x22, 0x24, 0x2A };  // group/panel fill
inline const QColor kBgSunken   { 0x0E, 0x0F, 0x13 };  // plot/meter backdrops
inline const QColor kBorder     { 0x34, 0x36, 0x3D };
inline const QColor kBorderSoft { 0x2A, 0x2C, 0x33 };

inline const QColor kTextPrimary   { 0xEA, 0xEC, 0xF0 };
inline const QColor kTextSecondary { 0x9A, 0xA0, 0xAE };
inline const QColor kTextDim       { 0x5E, 0x63, 0x6E };

// Accent colours keyed to meaning, not prettiness.
inline const QColor kAccent     { 0x4F, 0xC1, 0xE9 };  // teal — engaged control
inline const QColor kAccentDim  { 0x2E, 0x7A, 0x96 };
inline const QColor kWarn       { 0xE6, 0x7E, 0x22 };  // orange — gain reduction
inline const QColor kOk         { 0x2E, 0xCC, 0x71 };  // green — engine running

// Resolved path of the external stylesheet, if present.
QString stylesheetPath();

// Loaded stylesheet text from `stylesheetPath()`. Empty when not found.
QString globalStylesheet();

} // namespace theme
} // namespace ui
