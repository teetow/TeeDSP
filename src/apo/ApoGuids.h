#pragma once

// CLSID_TeeDspApo  {B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}
// IID_ITeeDspApo   {C4B5D6E7-F809-4B2C-AD3E-4F5A6B7C8D9E}
//
// These are made-up GUIDs generated for this project. They only need to be
// globally unique in practice (UuidCreate was not used here but the values
// are sufficiently random for local use).

#include <guiddef.h>
#include <cstdint>

// {B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}
DEFINE_GUID(CLSID_TeeDspApo,
    0xB3A4C5D6, 0xE7F8, 0x4A1B,
    0x9C, 0x2D, 0x3E, 0x4F, 0x5A, 0x6B, 0x7C, 0x8D);

// Property-key FMTID for FX endpoint properties (Windows SDK audioenginebaseapo.h).
// {D04E05A6-594B-4FB6-A80D-01AF5EED7D1D}
DEFINE_GUID(FMTID_AudioEndpointFx,
    0xD04E05A6, 0x594B, 0x4FB6,
    0xA8, 0x0D, 0x01, 0xAF, 0x5E, 0xED, 0x7D, 0x1D);

// PID 14 = PKEY_FX_ModeEffectClsid — MFX on modern Windows 10/11.
static constexpr uint32_t kFxModeEffectPid = 14;

// Effect GUID reported via GetControllableSystemEffectsList so the Windows 11
// audio engine treats this APO as having active processing.
// {E1F2A3B4-C5D6-4E7F-8091-A2B3C4D5E6F7}
DEFINE_GUID(GUID_TeeDspEffect,
    0xE1F2A3B4, 0xC5D6, 0x4E7F,
    0x80, 0x91, 0xA2, 0xB3, 0xC4, 0xD5, 0xE6, 0xF7);
