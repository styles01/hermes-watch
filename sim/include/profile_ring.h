// SPDX-License-Identifier: AGPL-3.0-or-later
//
// profile_ring.h — Hermes Watch Phase 1: profile carousel (Layer 1 of
// design/DESIGN-LOCKED.md), implemented for the host sim (and reusable on
// device). The KK avatar engine stays UNMODIFIED: the carousel is main-side
// UI state; `swipe` re-targets the engine's public swipe-offset API
// (setSwipeOffset / releaseSwipe / commitSwipe) — swipe animates the card
// slide, commit = activate next/prev profile.

#pragma once

#include "avatarsim_shim.h"

#if defined ( ESP_PLATFORM ) || defined ( ARDUINO )
#error "profile_ring.h is host sim UI code (Phase 1 sim-first)."
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace prof {

// Profile set v1. Names are generic by default; rename to your agents here.
constexpr size_t kProfileCount = 5;
constexpr const char* kProfileNames[kProfileCount] = {
    "mais", "oracle", "lara", "loca", "venus",
};
// Agent-state dot colors (idle=grey, listening=amber, thinking=blue,
// speaking=green) — driven by bridge events in Phase 3; static here.
constexpr uint16_t kStateDotColors[kProfileCount] = {
    0x8410, 0x8410, 0x8410, 0x8410, 0xFFE0,  // active agent = warm dot
};

struct Ring {
  int8_t activeIndex = 4;  // active agent on face top
  int8_t focusIndex = 4;   // card currently focused (pre-commit)
  bool activated = false;  // one-shot activation echo for the bridge stub

  static int wrap(int index) {
    if (index < 0) return kProfileCount - 1;
    if (index >= static_cast<int>(kProfileCount)) return 0;
    return index;
  }

  const char* activeName() const {
    return kProfileNames[static_cast<size_t>(activeIndex)];
  }

  // Swipe commit: direction -1/1 advances the active profile (same math as
  // the engine's expression browse in the reference main.cpp).
  void commitSwipe(int8_t direction) {
    activeIndex = static_cast<int8_t>(wrap(activeIndex + (direction < 0 ? -1 : 1)));
    focusIndex = activeIndex;
    activated = true;
  }
};

// Draw ONE card: mini round frame + state dot + name. Active card gets the
// brighter rim; horizontally offset by `cardOffset` px (swipe animation).
inline void drawCard(int cardCenterX, int cardCenterY, const char* name,
                     uint16_t dotColor, bool isActive, int cardOffset) {
  auto& gfx = M5.Display;
  const int cardW = 132;
  const int cardH = 44;
  const int x = cardCenterX - cardW / 2 + cardOffset;
  const int y = cardCenterY - cardH / 2;

  gfx.fillRoundRect(x, y, cardW, cardH, 18, 0x1082);
  gfx.drawRoundRect(x, y, cardW, cardH, 18,
                    isActive ? 0xFFE0 : 0x2104);
  gfx.fillCircle(x + 20, cardCenterY, 8, dotColor);
  gfx.setTextDatum(middle_left);
  gfx.setTextFont(&fonts::efontCN_16);
  gfx.setTextColor(TFT_WHITE, 0x1082);
  gfx.drawString(name, x + 40, cardCenterY);
  gfx.setTextDatum(top_left);
}

// Whole-row ribbon draw: 5 cards evenly spaced; `swipeOffsetPx` slides the
// row. Cards wrap around the ring so the track never runs out (carousel).
inline int cardOuter() { return 160; }

inline void drawRibbon(const Ring& ring, int swipeOffsetPx, int spacingPx) {
  auto& gfx = M5.Display;
  const int w = gfx.width();
  const int h = gfx.height();
  const int centerY = static_cast<int>(h * 0.80f);
  static bool dbg = std::getenv("AVSIM_DEBUG_UI") != nullptr;
  static bool dbg_fired_once = false;

  // Clear the ribbon band only (dirty-rect friendly; the avatar lives above).
  const int bandH = 64;
  gfx.fillRect(0, centerY - bandH / 2, w, bandH,
               std::getenv("AVSIM_DEBUG_UI") ? 0x003300u : 0x0000u);

  // Active card slides with the drag and springs home on release. Neighbor
  // cards fill both sides (wrap-around infinite track).
  for (int k = -2; k <= 2; ++k) {
    const int profileIndex = Ring::wrap(ring.activeIndex + k);
    int x = w / 2 + k * spacingPx + swipeOffsetPx;
    // wrap-around: keep neighbor cards gliding across the edges
    if (x < -spacingPx / 2) x += spacingPx * kProfileCount;
    if (x > w + spacingPx / 2) x -= spacingPx * kProfileCount;
    if (x < -cardOuter() || x > w + cardOuter()) continue;
    if (dbg && !dbg_fired_once) {
      Serial.printf("[ui] card slot=%d idx=%d name=%s x=%d\n", k,
                    static_cast<int>(profileIndex),
                    kProfileNames[profileIndex], x);
    }
    if (k == 2) dbg_fired_once = true;
    drawCard(x, centerY, kProfileNames[profileIndex],
             kStateDotColors[profileIndex], k == 0, 0);
  }
}

}  // namespace}  // namespace prof