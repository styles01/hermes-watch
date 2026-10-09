// SPDX-License-Identifier: AGPL-3.0-or-later
//
// avatarsim_shim.cpp — definitions for the host shim's global objects.
// Part of the Hermes Watch host simulator (Phase 0); AGPL-3.0-or-later
// provenance: compiles against engine sources from
// reference/m5stack-stopwatch-avatar (see ../engine/ and LICENSE.kk-avatar).

#include "avatarsim_shim.h"

#if defined ( ESP_PLATFORM ) || defined ( ARDUINO )
#error "avatarsim_shim.cpp is host-only."
#endif

namespace m5 {

// Global singletons the engine/demo reference:
//   M5     — the M5Unified-style facade (Display/BtnA/B/C/Touch/Imu/In_I2C);
//            AvatarDisplay sits first in M5_Class so M5.Display is ready
//            before setup() Panel_sdl creates the SDL window (first frame).
//   Serial — stdout-based serial replacement.
M5_Class M5;
SerialClass Serial;

}  // namespace m5