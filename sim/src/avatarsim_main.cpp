// SPDX-License-Identifier: AGPL-3.0-or-later
//
// avatarsim_main.cpp — host SDL2 main for the Hermes Watch avatar simulator.
//
// Compiles the REAL KK avatar engine sources (reference/m5stack-stopwatch-
// avatar, AGPL-3.0-or-later) against a minimal M5Unified shim:
//   - engine math is untouched (blink/PRNG/gaze/expressions run as on device)
//   - demo app logic (main.cpp) is re-implemented here because the original
//     main.cpp also drives M5IOE1 (vibration expander, ESP-only); its exact
//     interaction/touch/shake logic is reproduced below with hardware-only
//     writes stubbed out.
//
// M5GFX's SDL harness (lgfx::Panel_sdl::main) owns the SDL event pump and run
// loop: user_code setup()/loop() runs on a dedicated SDL thread, window close
// (or SIGINT) stops it. See sim/BUILD.md for the exact compile command.
//
// This file is part of the Hermes Watch host simulator (Phase 0).

#include <signal.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "avatarsim_shim.h"
#include "profile_ring.h"
#include "avatar_engine.h"

#if defined ( ESP_PLATFORM ) || defined ( ARDUINO )
#error "avatarsim_main.cpp is host-only."
#endif

// ---------------------------------------------------------------------------
// Engine + shim instances.
static AvatarEngine avatar;

namespace {

constexpr uint8_t kIoeAddress = 0x4F;  // unused on host (no IO expander)
constexpr uint32_t kDiagnosticRefreshIntervalMs = 100;
constexpr uint32_t kImuInteractionIntervalMs = 20;
constexpr uint32_t kShakeReleaseDelayMs = 650;
constexpr uint32_t kShakeSequenceTimeoutMs = 1500;
constexpr uint32_t kShakeMaximumSwingGapMs = 480;
constexpr uint32_t kShakeMinimumSwingGapMs = 90;
constexpr float kStrongShakeAxisDelta = 0.32f;
constexpr float kStrongShakeJerk = 0.50f;
constexpr uint8_t kRequiredShakeSwings = 4;
constexpr uint16_t kImuCalibrationSamples = 30;
constexpr int16_t kGestureDirectionLockPx = 12;
constexpr int16_t kGestureCommitPx = 52;
constexpr uint16_t kSwipeTransitionMs = 160;
constexpr int kRoundBezelAlphaRows = 4;   // extra rows per edge for the mask
constexpr int kRingInner = 228;           // ring radius for the bezel ring

enum class GestureAxis : uint8_t { None, Horizontal, Vertical };

bool vibrationReady = false;   // stays false on host (no IO expander)
bool diagnosticMode = false;
bool diagnosticToggleLatched = false;
uint32_t vibrationStopsAtMs = 0;
uint32_t lastDiagnosticRefreshMs = 0;
uint32_t lastImuInteractionMs = 0;
uint32_t shakeQuietStartedMs = 0;
uint16_t imuCalibrationCount = 0;
float filteredAccelX = 0.0f;
float filteredAccelY = 0.0f;
float neutralAccelX = 0.0f;
float neutralAccelY = 0.0f;
float previousAccelX = 0.0f;
float previousAccelY = 0.0f;
float previousAccelZ = 0.0f;
float shakeEnergy = 0.0f;
bool shakeReactionActive = false;
uint8_t shakeSwingCount = 0;
int8_t previousShakeDirection = 0;
uint32_t shakeSequenceStartedMs = 0;
uint32_t lastStrongShakeMs = 0;
m5::String lastDiagnosticEvent = "Waiting for input";
m5::String serialCommand;
GestureAxis gestureAxis = GestureAxis::None;
bool gestureCommitted = false;
prof::Ring profileRing;   // Phase 1: profile carousel state
int profileSwipePx = 0;   // live drag offset for the card ribbon (px)
bool roundMaskEnabled = true;
uint32_t lastOverlayMs_ = 0;

bool reached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

void resetShakeSequence() {
  shakeSwingCount = 0;
  previousShakeDirection = 0;
  shakeSequenceStartedMs = 0;
  lastStrongShakeMs = 0;
}

void setFont() {
  M5.Display.setFont(&fonts::efontCN_16);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(top_left);
}

void drawDiagnosticLine(int y, const m5::String& label, const m5::String& value,
                        uint16_t color = TFT_WHITE) {
  M5.Display.fillRect(46, y, 374, 22, TFT_BLACK);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.drawString(label.c_str(), 46, y);
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.drawString(value.c_str(), 156, y);
}

void drawDiagnosticFrame() {
  M5.Display.fillScreen(TFT_BLACK);
  const int cx = M5.Display.width() / 2;
  const int cy = M5.Display.height() / 2;
  M5.Display.drawCircle(cx, cy, min(cx, cy) - 3, TFT_DARKGREY);
  M5.Display.drawCircle(cx, cy, min(cx, cy) - 12, TFT_NAVY);

  setFont();
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString("Hardware Check", cx, 45);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.drawString("Hold A+B to return", cx, 72);
  M5.Display.setTextDatum(top_left);

  drawDiagnosticLine(112, "Display",
                     m5::String(std::to_string(M5.Display.width()) + " x " +
                                std::to_string(M5.Display.height())),
                     TFT_GREEN);
  drawDiagnosticLine(146, "IMU", "initializing...");
  drawDiagnosticLine(180, "Touch", M5.Touch.isEnabled() ? "enabled" : "not detected");
  drawDiagnosticLine(214, "Vibration", vibrationReady ? "ready" : "not detected");
  drawDiagnosticLine(264, "Last event", lastDiagnosticEvent, TFT_YELLOW);

  M5.Display.setTextDatum(top_center);
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Display.drawString("A: vibrate", cx, 340);
  M5.Display.drawString("B: redraw", cx, 370);
  M5.Display.drawString("Touch: show coordinates", cx, 400);
  M5.Display.setTextDatum(top_left);
}

void setDiagnosticEvent(const m5::String& event) {
  lastDiagnosticEvent = event;
  drawDiagnosticLine(264, "Last event", lastDiagnosticEvent, TFT_YELLOW);
  Serial.println(event);
}

void stopVibration() {
  // Hardware-only on device (IO expander PWM); nothing to do on host.
  vibrationStopsAtMs = 0;
}

void startVibration(uint8_t strength = 160, uint16_t durationMs = 45) {
  (void)strength;
  (void)durationMs;
  // No haptics on the host sim.
}

void updateVibration(uint32_t nowMs) {
  (void)nowMs;
}

void setupVibration() { vibrationReady = false; }

void trigger(ExpressionId expression, uint32_t nowMs,
             uint16_t firstTransitionMs = 0) {
  avatar.show(expression, nowMs, true, firstTransitionMs);
  startVibration();
  Serial.printf("Expression: %s\n", avatar.activeName());
}

// --- Touch interaction (same gesture logic as reference main.cpp) ----------
void handleProductTouch(uint32_t nowMs) {
  if (!M5.Touch.isEnabled()) return;
  const auto touch = M5.Touch.getDetail(0);

  if (touch.wasPressed()) {
    gestureAxis = GestureAxis::None;
    gestureCommitted = false;
  }

  if (touch.wasHold() && gestureAxis == GestureAxis::None &&
      !gestureCommitted) {
    gestureCommitted = true;
    avatar.releaseTouch();
    trigger(ExpressionId::Angry, nowMs);
    return;
  }

  if (touch.isPressed()) {
    const bool moving = touch.isFlicking() || touch.isDragging();
    const int deltaX = moving ? touch.distanceX() : 0;
    const int deltaY = moving ? touch.distanceY() : 0;

    if (gestureAxis == GestureAxis::None &&
        max(abs(deltaX), abs(deltaY)) >= kGestureDirectionLockPx) {
      gestureAxis = abs(deltaX) >= abs(deltaY) ? GestureAxis::Horizontal
                                               : GestureAxis::Vertical;
    }

    if (gestureAxis == GestureAxis::Horizontal) {
      avatar.releaseTouch();
      const int8_t direction = deltaX > 0 ? -1 : 1;
      // Phase 1: the swipe targets the PROFILE CAROUSEL (Layer 1), not
      // expression browsing. The avatar ride gets a fraction of the drag as
      // parallax via the engine's swipe-offset hook; the card ribbon follows
      // 1:1 in drawProfileUi().
      avatar.setSwipeOffset(deltaX * 0.25f, 0.0f,
                            gestureCommitted ? 0 : direction);
      profileSwipePx = deltaX;
      if (!gestureCommitted && abs(deltaX) >= kGestureCommitPx) {
        avatar.commitSwipe(direction, nowMs, kSwipeTransitionMs);
        profileRing.commitSwipe(direction);
        gestureCommitted = true;
        startVibration(125, 35);
        Serial.printf("Swipe commit: %s (profile %d) dx=%d\n",
                      profileRing.activeName(),
                      static_cast<int>(profileRing.activeIndex), deltaX);
      }
      return;
    }

    if (gestureAxis == GestureAxis::Vertical) {
      avatar.releaseTouch();
      avatar.setSwipeOffset(0.0f, deltaY, 0);
      if (!gestureCommitted && abs(deltaY) >= kGestureCommitPx) {
        trigger(deltaY > 0 ? ExpressionId::Sleepy
                           : ExpressionId::Surprised,
                nowMs, kSwipeTransitionMs);
        gestureCommitted = true;
        Serial.printf("Vertical swipe commit: %s dy=%d\n",
                      avatar.activeName(), deltaY);
      }
      return;
    }

    if (!gestureCommitted) avatar.setTouchTarget(touch.x, touch.y);
  }

  if (touch.wasReleased()) {
    const bool consumed =
        gestureCommitted || gestureAxis != GestureAxis::None;
    avatar.releaseTouch();
    avatar.releaseSwipe();
    profileSwipePx = 0;
    gestureAxis = GestureAxis::None;
    gestureCommitted = false;
    if (consumed) return;
  }

  if (gestureCommitted) {
    return;
  }

  if (touch.wasClicked()) {
    trigger(touch.getClickCount() >= 2 ? ExpressionId::Surprised
                                       : ExpressionId::Happy,
            nowMs);
  }
}

// --- IMU interaction (canned motion from the shim; identical processing) ---
void handleImuInteraction(uint32_t nowMs) {
  if (nowMs - lastImuInteractionMs < kImuInteractionIntervalMs) return;
  lastImuInteractionMs = nowMs;
  if (!M5.Imu.update()) return;

  const auto imu = M5.Imu.getImuData();
  // StopWatch BMI270 swap: accel.y→screen X, accel.x→screen Y (see reference).
  const float screenAccelX = imu.accel.y;
  const float screenAccelY = imu.accel.x;
  const float screenAccelZ = imu.accel.z;
  const float screenGyroX = imu.gyro.y;
  const float screenGyroY = imu.gyro.x;
  constexpr float kFilterAmount = 0.32f;
  if (imuCalibrationCount == 0) {
    filteredAccelX = screenAccelX;
    filteredAccelY = screenAccelY;
    previousAccelX = screenAccelX;
    previousAccelY = screenAccelY;
    previousAccelZ = screenAccelZ;
  } else {
    filteredAccelX += (screenAccelX - filteredAccelX) * kFilterAmount;
    filteredAccelY += (screenAccelY - filteredAccelY) * kFilterAmount;
  }

  if (imuCalibrationCount < kImuCalibrationSamples) {
    neutralAccelX += screenAccelX / kImuCalibrationSamples;
    neutralAccelY += screenAccelY / kImuCalibrationSamples;
    ++imuCalibrationCount;
    if (imuCalibrationCount == kImuCalibrationSamples) {
      Serial.printf("IMU interaction ready: neutral x=%.2f y=%.2f\n",
                    neutralAccelX, neutralAccelY);
    }
  } else {
    neutralAccelX += (filteredAccelX - neutralAccelX) * 0.00015f;
    neutralAccelY += (filteredAccelY - neutralAccelY) * 0.00015f;
    avatar.setTiltTarget(-(filteredAccelX - neutralAccelX) / 0.28f,
                         -(filteredAccelY - neutralAccelY) / 0.28f,
                         -screenGyroY / 180.0f, screenGyroX / 180.0f);
  }

  const float deltaX = screenAccelX - previousAccelX;
  const float deltaY = screenAccelY - previousAccelY;
  const float deltaZ = screenAccelZ - previousAccelZ;
  const float jerk = fabsf(deltaX) + fabsf(deltaY) + fabsf(deltaZ);
  previousAccelX = screenAccelX;
  previousAccelY = screenAccelY;
  previousAccelZ = screenAccelZ;
  shakeEnergy = shakeEnergy * 0.72f + jerk * 0.28f;

  if (imuCalibrationCount < kImuCalibrationSamples) return;

  const float shakeIntensity = max(
      0.0f, min(1.0f, (shakeEnergy - 0.055f) / 0.48f));
  const float shakeDirectionX =
      max(-1.0f, min(1.0f, -deltaX / 0.28f));
  const float shakeDirectionY =
      max(-1.0f, min(1.0f, -deltaY / 0.28f));
  avatar.setShakeTarget(shakeDirectionX, shakeDirectionY, shakeIntensity);

  if (!shakeReactionActive) {
    if (shakeSwingCount > 0 &&
        (nowMs - shakeSequenceStartedMs > kShakeSequenceTimeoutMs ||
         nowMs - lastStrongShakeMs > kShakeMaximumSwingGapMs)) {
      resetShakeSequence();
    }

    if (shakeSwingCount == 0 && jerk >= kStrongShakeJerk) {
      if (fabsf(deltaX) >= kStrongShakeAxisDelta) {
        shakeSwingCount = 1;
        previousShakeDirection = deltaX >= 0.0f ? 1 : -1;
        shakeSequenceStartedMs = nowMs;
        lastStrongShakeMs = nowMs;
        Serial.printf("IMU horizontal swing 1/%u delta=%.2f\n",
                      kRequiredShakeSwings, deltaX);
      }
    } else if (shakeSwingCount > 0) {
      const int8_t direction = deltaX >= 0.0f ? 1 : -1;
      const uint32_t swingGapMs = nowMs - lastStrongShakeMs;
      const bool strongEnough = jerk >= kStrongShakeJerk &&
                                fabsf(deltaX) >= kStrongShakeAxisDelta;
      if (strongEnough && direction != previousShakeDirection &&
          swingGapMs >= kShakeMinimumSwingGapMs &&
          swingGapMs <= kShakeMaximumSwingGapMs) {
        ++shakeSwingCount;
        previousShakeDirection = direction;
        lastStrongShakeMs = nowMs;
        Serial.printf("IMU horizontal swing %u/%u delta=%.2f gap=%lums\n",
                      shakeSwingCount, kRequiredShakeSwings, deltaX,
                      static_cast<unsigned long>(swingGapMs));
      }
    }

    if (shakeSwingCount >= kRequiredShakeSwings) {
      shakeReactionActive = true;
      shakeQuietStartedMs = 0;
      resetShakeSequence();
      avatar.play(ExpressionId::Dizzy, nowMs,
                  AvatarEngine::PlaybackMode::Loop, false, 150);
      startVibration(190, 80);
      Serial.printf("IMU repeated horizontal shake -> DIZZY energy=%.2f\n",
                    shakeEnergy);
    }
    return;
  }

  if (shakeIntensity > 0.10f) {
    shakeQuietStartedMs = 0;
    return;
  }

  if (shakeQuietStartedMs == 0) {
    shakeQuietStartedMs = nowMs;
  } else if (nowMs - shakeQuietStartedMs >= kShakeReleaseDelayMs) {
    shakeReactionActive = false;
    shakeQuietStartedMs = 0;
    resetShakeSequence();
    if (avatar.activeExpression() == ExpressionId::Dizzy) {
      avatar.show(avatar.baseExpression(), nowMs, false, 260);
    }
    Serial.println("IMU shake settled -> BASE");
  }
}

void handleSerialCommands(uint32_t nowMs) {
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      if (serialCommand.length() == 0) continue;
      if (serialCommand.startsWith("profile ")) {
        m5::String token = serialCommand;
        token.remove(0, 8);
        token.trim();
        bool matched = false;
        for (size_t i = 0; i < prof::kProfileCount; ++i) {
          if (token == prof::kProfileNames[i]) {
            profileRing.activeIndex = static_cast<int8_t>(i);
            profileRing.focusIndex = static_cast<int8_t>(i);
            profileSwipePx = 0;
            avatar.invalidate();
            matched = true;
            break;
          }
        }
        if (matched) {
          Serial.printf("ACTIVATED %d %s\n",
                        static_cast<int>(profileRing.activeIndex),
                        profileRing.activeName());
        } else {
          Serial.printf("Unknown profile: %s\n", token.c_str());
        }
        serialCommand = "";
        continue;
      }
      if (avatar.showFromCommand(serialCommand, nowMs)) {
        startVibration();
        Serial.printf("Command accepted: %s\n", avatar.activeName());
      } else {
        Serial.printf("Unknown command: %s\n", serialCommand.c_str());
      }
      serialCommand = "";
    } else if (serialCommand.length() < 32) {
      serialCommand += character;
    }
  }
}

void refreshDiagnosticSensors() {
  if (M5.Imu.update()) {
    const auto imu = M5.Imu.getImuData();
    char values[64];
    snprintf(values, sizeof(values), "H %.2f  V %.2f  Z %.2f", imu.accel.y,
             imu.accel.x, imu.accel.z);
    drawDiagnosticLine(146, "IMU", values, TFT_GREEN);
  } else {
    drawDiagnosticLine(146, "IMU", "no data", TFT_RED);
  }

  if (M5.Touch.getCount() > 0) {
    const auto touch = M5.Touch.getDetail(0);
    drawDiagnosticLine(180, "Touch",
                       "x " + std::to_string(touch.x) + "  y " +
                           std::to_string(touch.y),
                       TFT_GREEN);
  } else {
    drawDiagnosticLine(180, "Touch",
                       M5.Touch.isEnabled() ? "enabled" : "not detected",
                       M5.Touch.isEnabled() ? TFT_GREEN : TFT_RED);
  }
}

void setDiagnosticMode(bool enabled) {
  diagnosticMode = enabled;
  stopVibration();
  if (diagnosticMode) {
    drawDiagnosticFrame();
    Serial.println("Diagnostic mode entered");
  } else {
    avatar.invalidate();
    Serial.println("Expression mode entered");
  }
}

void handleDiagnosticInput(uint32_t nowMs) {
  if (M5.BtnA.wasClicked()) {
    setDiagnosticEvent("Button A clicked");
    startVibration(180, 70);
  }
  if (M5.BtnB.wasClicked()) {
    lastDiagnosticEvent = "Button B clicked";
    drawDiagnosticFrame();
    Serial.println(lastDiagnosticEvent);
  }

  const auto touch = M5.Touch.getDetail(0);
  if (touch.wasPressed()) {
    setDiagnosticEvent("Touch " + std::to_string(touch.x) + "," +
                       std::to_string(touch.y));
  }

  if (nowMs - lastDiagnosticRefreshMs >= kDiagnosticRefreshIntervalMs) {
    lastDiagnosticRefreshMs = nowMs;
    refreshDiagnosticSensors();
  }
}

void handleDiagnosticToggle() {
  const bool bothHeld = M5.BtnA.pressedFor(1000) && M5.BtnB.pressedFor(1000);
  if (bothHeld && !diagnosticToggleLatched) {
    diagnosticToggleLatched = true;
    setDiagnosticMode(!diagnosticMode);
  } else if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
    diagnosticToggleLatched = false;
  }
}

// Round-bezel/round-mask pass: blacks out the four corner areas outside the
// inscribed circle and paints a thin bezel ring, so the 466x466 square SDL
// window reads as the StopWatch's round face. The engine only ever draws eyes
// in the central band (dirty-rect clear + full clear both black), so the
// corner regions are always black on hardware content — 4 cheap fillRects
// re-assert the mask each throttle tick without touching eye pixels.
void applyRoundOverlay() {
  if (!roundMaskEnabled) return;
  const uint32_t now = millis();
  if (now - lastOverlayMs_ < 45) return;  // ~22 Hz, well under render cost
  lastOverlayMs_ = now;

  const int w = M5.Display.width();
  const int h = M5.Display.height();
  const int radius = min(w, h) / 2;  // 233
  if (w != h || w == 0) return;

  const int side = w - 2 * radius;  // 0 when exactly inscribed (466)
  // Corner wedges outside the circle: cover via quarter-square fills; the
  // circle inscribed at (233,233) r=233 exactly touches all edges, so the
  // four fillRects re-blacken the corner remainder each pass.
  if (side > 0) {
    M5.Display.fillRect(0, 0, radius + side, radius + side, TFT_BLACK);
  }
  M5.Display.drawCircle(w / 2, h / 2, radius - 1, 0x08421);  // subtle rim
  M5.Display.drawCircle(w / 2, h / 2, radius - 2, 0x08421);  // subtle rim
}

}  // namespace

// ---------------------------------------------------------------------------
// setup/loop — same structure and interaction logic as the reference main.cpp
// (M5IOE1/vibration stubbed). Runs on Panel_sdl's user thread.
void setup() {
  signal(SIGINT, SIG_DFL);
  M5.begin();
  Serial.begin(115200);

  M5.Display.init();  // allocates the Panel_sdl framebuffer + SDL window def
  M5.Display.setRotation(0);
  M5.Display.setBrightness(150);
  M5.Touch.setHoldThresh(650);
  M5.Touch.setFlickThresh(10);
  setupVibration();

  if (!avatar.begin()) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("Avatar buffer failed", M5.Display.width() / 2,
                          M5.Display.height() / 2);
    Serial.println("Avatar sprite allocation failed");
  }

  Serial.println("Hermes Watch avatar simulator (SDL host)");
  Serial.println("Mouse = touch (tap/drag/hover), LEFT/RIGHT arrows = BtnA/BtnB");
  Serial.println("Commands: idle listening thinking happy excited curious "
                 "confused angry surprised sad sleepy dizzy");
  Serial.println("Playback test: once|loop|pingpong <expression>");
  Serial.println("Hold A+B for hardware diagnostics; close window to exit");
}

void loop() {
  M5.update();
  const uint32_t nowMs = millis();
  updateVibration(nowMs);
  handleDiagnosticToggle();

  if (diagnosticMode) {
    handleDiagnosticInput(nowMs);
  } else {
    if (M5.BtnA.wasClicked()) {
      profileRing.commitSwipe(-1);
      profileSwipePx = 0;
      avatar.invalidate();
      startVibration(125, 35);
      Serial.printf("BtnA: profile -> %s\n", profileRing.activeName());
    }
    if (M5.BtnB.wasClicked()) {
      profileRing.commitSwipe(1);
      profileSwipePx = 0;
      avatar.invalidate();
      startVibration(125, 35);
      Serial.printf("BtnB: profile -> %s\n", profileRing.activeName());
    }
    handleProductTouch(nowMs);
    handleImuInteraction(nowMs);
    handleSerialCommands(nowMs);
    avatar.update(nowMs);
  }

  prof::drawRibbon(profileRing, profileSwipePx, 168);
  if (profileRing.activated) {
    profileRing.activated = false;
    Serial.printf("ACTIVATED %d %s\n",
                  static_cast<int>(profileRing.activeIndex),
                  profileRing.activeName());
  }

  applyRoundOverlay();

  delay(1);
}

// ---------------------------------------------------------------------------
// SDL harness entry — lgfx::Panel_sdl::main owns the event loop, spawns
// user_func (setup + loop) on its own thread, and returns once the window is
// closed. Mirrors M5GFX examples/PlatformIO_SDL/src/sdl_main.cpp.
__attribute__((weak))
int user_func(bool* running) {
  setup();
  do {
    loop();
  } while (*running);
  return 0;
}

int main(int, char**) {
  // The second argument (ms per step-exec tick) matches the official SDL
  // example's value; it only affects debugger stepping behavior.
  // msec_step_exec = 0: Panel_sdl's debugger-stepping path is left off (its
  // detector also serialized normal rendering behind 60 Hz presents on
  // macOS). The official example still works fine with stepping disabled.
  return lgfx::Panel_sdl::main(user_func, 0);
}

#if 0  // keep the ESP-IDF app_main block out of the host build
extern "C" {
int app_main(int, char**);
}
#endif