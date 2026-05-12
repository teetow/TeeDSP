#pragma once

#include <guiddef.h>

// {B7E1A0C0-7E5D-4D8B-9E2A-1C4F8D3A2B11}
// Stable CLSID for the TeeDSP MFX (mode-effect, post-mix) APO.
// Persisted in the Windows registry and in endpoint property stores —
// changing it after install would orphan registrations on user machines.
extern "C" const GUID CLSID_TeeDspApoMfx;
