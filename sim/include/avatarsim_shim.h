// SPDX-License-Identifier: AGPL-3.0-or-later
//
// avatarsim_shim.h — minimal macOS host replacement for M5Unified, letting the
// REAL KK avatar engine (reference/m5stack-stopwatch-avatar, AGPL-3.0-or-later)
// compile against M5GFX's official SDL2 host panel (lgfx::Panel_sdl).
//
// Engine surface satisfied here:
//   M5.Display  → a 466x466 lgfx::LGFX_Device backed by Panel_sdl (SDL window)
//   M5.update() → polls emulated GPIO buttons + advances touch state machine
//   M5.BtnA/B/C → SDL arrow keys via Panel_sdl's emulated GPIO (BtnA=Left
//                 GPIO39, BtnB=Right GPIO37), through a Button_Class clone of
//                 M5Unified's Button_Class (debounce/hold/click-count)
//   M5.Touch    → SDL mouse mapped through M5Unified's touch state machine
//                 (touch / hold / flick / drag / click-count semantics)
//   M5.Imu      → canned motion (tilt sweep + periodic shake burst)
//   M5.In_I2C   → stub (IO-expander vibration motor is hardware-only)
//   Serial      → stdout + stdin (select()-based non-blocking poll)
//   String      → std::string subset used by the engine/demo
//   millis/micros/delay/random → M5GFX SDL platform + small shims
//   min/max/abs → std algorithms/abs (engine calls them unqualified on ints)
//
// Drawing surface used by the engine (all from M5GFX/LovyanGFX): fillRect,
// fillScreen, fillCircle, fillRoundRect, fillTriangle, drawLine, drawWideLine,
// drawCircle, setFont, setTextSize, setTextDatum, setTextColor, drawString,
// color565, width, height, startWrite, endWrite, setRotation, setBrightness.
//
// This file is part of the Hermes Watch host simulator (Phase 0).
// Host-only: refuses to compile on ESP/Arduino targets.
#pragma once

#include <M5GFX.h>  // lgfx, Panel_sdl, fonts, datum, TFT_* colors (MIT/FreeBSD)

#if defined ( ESP_PLATFORM ) || defined ( ARDUINO )
#error "avatarsim_shim.h is host-only; firmware builds use the real M5Unified."
#endif

#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Arduino-style global min/max/abs so the engine's unqualified calls on ints
// compile exactly as under the Arduino core (which defines them as macros).
using std::min;
using std::max;
using std::abs;

// ---------------------------------------------------------------------------
// Arduino runtime builtins the engine calls unqualified. The M5GFX SDL
// platform provides millis/micros/delay in namespace lgfx (SDL_GetTicks
// based); hoist them to the global namespace like the Arduino core does.
using lgfx::millis;
using lgfx::micros;
using lgfx::delay;
using lgfx::delayMicroseconds;

// Arduino random(min,max) → uniform on [min, max) via rand().
inline long random(long minimum, long maximum) {
  if (maximum <= minimum) return minimum;
  return minimum + std::rand() % (maximum - minimum);
}
inline long random(long maximum) { return random(0, maximum); }
inline void randomSeed(unsigned long seed) {
  std::srand(static_cast<unsigned>(seed) + 1u);
}

namespace m5 {

// Forward-declare the shim API so global-scope using-declarations (below, at
// the file end) can hoist M5/Serial/String into the Arduino-style global
// namespace exactly like M5Unified.h does on the ESP side.
class String;   // full definition follows
struct M5_Class;
extern M5_Class M5;         // defined in avatarsim_main.cpp
class SerialClass;
extern SerialClass Serial;  // defined in avatarsim_main.cpp

// ---------------------------------------------------------------------------
// String (Arduino-flavored subset used by the engine/demo).
class String {
 public:
  String() = default;
  String(const char* text) : buffer_(text) {}
  String(const std::string& text) : buffer_(text) {}

  const char* c_str() const { return buffer_.c_str(); }
  size_t length() const { return buffer_.size(); }

  void trim() {
    const size_t first = buffer_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { buffer_.clear(); return; }
    const size_t last = buffer_.find_last_not_of(" \t\r\n");
    buffer_ = buffer_.substr(first, last - first + 1);
  }

  void toLowerCase() {
    std::transform(buffer_.begin(), buffer_.end(), buffer_.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });
  }

  void remove(unsigned index, unsigned count) {
    if (index >= buffer_.size()) { buffer_.clear(); return; }
    buffer_.erase(index, count);
  }

  String& operator+=(char c) { buffer_ += c; return *this; }

  void remove(unsigned index) {
    if (index < buffer_.size()) buffer_.resize(index);
  }

  bool startsWith(const String& prefix) const {
    return buffer_.rfind(prefix.buffer_, 0) == 0;
  }
  bool startsWith(const char* prefix) const {
    return buffer_.rfind(prefix, 0) == 0;
  }

  bool operator==(const String& other) const { return buffer_ == other.buffer_; }
  bool operator==(const char* other) const { return buffer_ == other; }

  String& operator+=(const String& other) { buffer_ += other.buffer_; return *this; }

 private:
  std::string buffer_;
};

// ---------------------------------------------------------------------------
// touch_state_t + touch_detail_t — mirrors m5::touch_detail_t semantics
// (state bit layout and transitions copied from M5Unified Touch_Class.hpp,
// MIT): wasPressed = state==touch_begin, wasClicked = state==touch_end,
// wasReleased = (state & (mask_touch|mask_change)) == mask_change, hold after
// msec_hold_, flick/drag claim the point when movement passes flick_thresh.
enum touch_state_t : std::uint8_t {
  touch_none        = 0b0000,
  touch_touch       = 0b0001,
  touch_touch_end   = 0b0010,
  touch_touch_begin = 0b0011,

  touch_hold        = 0b0101,
  touch_hold_end    = 0b0110,
  touch_hold_begin  = 0b0111,

  touch_flick       = 0b1001,
  touch_flick_end   = 0b1010,
  touch_flick_begin = 0b1011,

  touch_drag        = 0b1101,
  touch_drag_end    = 0b1110,
  touch_drag_begin  = 0b1111,

  touch_mask_touch   = 0b0001,
  touch_mask_change  = 0b0010,
  touch_mask_holding = 0b0100,
  touch_mask_moving  = 0b1000,
};

struct touch_detail_t : public m5gfx::touch_point_t {
  std::int16_t prev_x = 0;
  std::int16_t prev_y = 0;
  std::int16_t base_x = 0;
  std::int16_t base_y = 0;

  std::uint32_t base_msec = 0;
  touch_state_t state = touch_none;
  std::uint8_t click_count = 0;

  inline int deltaX() const { return x - prev_x; }
  inline int deltaY() const { return y - prev_y; }
  inline int distanceX() const { return x - base_x; }
  inline int distanceY() const { return y - base_y; }
  inline bool isPressed() const { return state & touch_mask_touch; }
  inline bool wasPressed() const { return state == touch_touch_begin; }
  inline bool wasClicked() const { return state == touch_touch_end; }
  inline bool isReleased() const { return !(state & touch_mask_touch); }
  inline bool wasReleased() const {
    return (state & (touch_mask_touch | touch_mask_change)) == touch_mask_change;
  }
  inline bool isHolding() const {
    return (state & (touch_mask_touch | touch_mask_holding)) ==
           (touch_mask_touch | touch_mask_holding);
  }
  inline bool wasHold() const { return state == touch_hold_begin; }
  inline bool wasFlickStart() const { return state == touch_flick_begin; }
  // flick/drag both set mask_moving while the point stays pressed and the
  // hold state has not claimed it — matching M5Unified's mask arithmetic.
  inline bool isFlicking() const {
    return (state & (touch_mask_moving | touch_mask_touch)) ==
               (touch_mask_moving | touch_mask_touch) &&
           !(state & touch_mask_holding);
  }
  inline bool isDragging() const {
    return (state & (touch_mask_moving | touch_mask_touch)) ==
               (touch_mask_moving | touch_mask_touch) &&
           !(state & touch_mask_holding);
  }
  inline std::uint8_t getClickCount() const { return click_count; }
};

// ---------------------------------------------------------------------------
// Button_Class — mirrors m5::Button_Class (debounce, hold transition, click
// counting, pressedFor). Covers the API the engine/demo touches.
class Button_Class {
 public:
  bool wasClicked() const { return state_ == state_clicked; }
  bool wasHold() const { return reported_hold_; }
  bool isPressed() const { return press_ != 0; }
  bool isReleased() const { return press_ == 0; }
  bool wasReleased() const { return old_press_ && press_ == 0; }
  bool pressedFor(std::uint32_t ms) const {
    return (press_ && last_msec_ - last_change_ >= ms);
  }

  void setDebounceThresh(std::uint32_t msec) { msec_debounce_ = msec; }
  void setHoldThresh(std::uint32_t msec) { msec_hold_ = msec; }

  void setRawState(std::uint32_t msec, bool press_raw) {
    bool disable_db = (msec - last_msec_) > msec_debounce_;
    std::uint8_t old_press = press_;
    old_press_ = old_press != 0;
    reported_hold_ = false;
    if (raw_press_ != press_raw) {
      raw_press_ = press_raw;
      last_raw_change_ = msec;
    }
    if (disable_db || msec - last_raw_change_ >= msec_debounce_) {
      if (press_raw != (0 != old_press)) {
        last_change_ = msec;
      }
      if (press_raw) {
        std::uint32_t hold_period = msec - last_change_;
        last_hold_period_ = hold_period;
        if (old_press == 0) {
          press_ = 1;
        } else if (old_press == 1 && hold_period >= msec_hold_) {
          press_ = 2;
          reported_hold_ = true;
        }
      } else {
        if (old_press == 1) {
          state_ = state_clicked;  // fires wasClicked() for exactly one frame
          ++click_count_;
          last_clicked_ = msec;
        }
        press_ = 0;
      }
    }
    // Click-count timeout: clear the count only when the sequence window has
    // fully elapsed with no press, so getClickCount() stays readable during
    // the frame that reports the tap.
    if (click_count_ > 0 && press_ == 0 && msec - last_clicked_ > msec_hold_) {
      click_count_ = 0;
      state_ = state_nochange;
    }
    last_msec_ = msec;
  }

 private:
  enum button_state_t : std::uint8_t {
    state_nochange = 0,
    state_clicked  = 1,
    state_hold     = 2,
  };
  std::uint32_t msec_debounce_ = 10;
  std::uint32_t msec_hold_ = 500;
  std::uint32_t last_msec_ = 0;
  std::uint32_t last_raw_change_ = 0;
  std::uint32_t last_change_ = 0;
  std::uint32_t last_hold_period_ = 0;
  std::uint32_t last_clicked_ = 0;
  std::uint32_t click_count_ = 0;
  bool raw_press_ = false;
  bool old_press_ = false;
  bool reported_hold_ = false;
  std::uint8_t press_ = 0;
  button_state_t state_ = state_nochange;
};

// ---------------------------------------------------------------------------
// Touch_Class — clone of m5::Touch_Class::update: pulls raw points from the
// active panel (SDL mouse via Panel_sdl::getTouchRaw), converts to screen
// coordinates, and advances the per-point tap/hold/flick/drag/click-count
// state machine. SDL supplies a single touch point (the mouse); the engine
// reads detail index 0 on hardware too.
class Touch_Class {
 public:
  static constexpr std::size_t TOUCH_MAX_POINTS = 5;
  static constexpr std::uint32_t TOUCH_MIN_UPDATE_MSEC = 4;

  bool isEnabled() const { return gfx_ != nullptr; }
  std::uint8_t getCount() const { return detail_count_; }
  const touch_detail_t& getDetail(std::size_t index = 0) const {
    return touch_detail_[raw_point_[index].id < TOUCH_MAX_POINTS
                             ? raw_point_[index].id : 0];
  }
  void setHoldThresh(std::uint16_t msec) { msec_hold_ = msec; }
  void setFlickThresh(std::uint16_t distance) { flick_thresh_ = distance; }
  void begin(m5gfx::LGFX_Device* gfx) { gfx_ = gfx; }

  std::uint8_t update(std::uint32_t msec) {
    if (msec - last_msec_ <= TOUCH_MIN_UPDATE_MSEC) {
      if (detail_count_ == 0) { return 0; }
      std::uint8_t count = 0;
      for (std::size_t i = 0; i < TOUCH_MAX_POINTS; ++i) {
        count += update_detail(&touch_detail_[i], msec) ? 1 : 0;
      }
      detail_count_ = count;
      return count;
    }

    last_msec_ = msec;
    std::uint8_t count = gfx_->getTouchRaw(raw_point_, TOUCH_MAX_POINTS);
    if (!count && !detail_count_) { return 0; }

    std::uint32_t updated_id = 0;
    if (count) {
      m5gfx::touch_point_t tp[TOUCH_MAX_POINTS];
      std::memcpy(tp, raw_point_, sizeof(m5gfx::touch_point_t) * count);
      gfx_->convertRawXY(tp, count);
      for (std::size_t i = 0; i < count; ++i) {
        if (tp[i].id < TOUCH_MAX_POINTS) {
          updated_id |= 1 << tp[i].id;
          update_detail(&touch_detail_[tp[i].id], msec, true, &tp[i]);
        }
      }
    }
    for (std::size_t i = 0; i < TOUCH_MAX_POINTS; ++i) {
      if (!(updated_id & (1 << i))) {
        update_detail(&touch_detail_[i], msec, false, nullptr);
      }
    }

    std::uint8_t active = 0;
    for (std::size_t i = 0; i < TOUCH_MAX_POINTS; ++i) {
      active += touch_detail_[i].isPressed() ? 1 : 0;
    }
    detail_count_ = active;
    return active;
  }

 private:
  bool update_detail(touch_detail_t* det, std::uint32_t msec, bool pressed,
                     m5gfx::touch_point_t* tp) {
    touch_state_t tm = det->state;
    if (tm == touch_none && !pressed) {
      return false;
    }
    tm = static_cast<touch_state_t>(tm & ~touch_mask_change);
    if (pressed) {
      det->prev_x = det->x;
      det->prev_y = det->y;
      det->size = tp->size;
      det->id = tp->id;
      if (!(tm & touch_mask_moving)) {
        if (tm & touch_mask_touch) {
          if (std::abs(det->base_x - tp->x) > flick_thresh_ ||
              std::abs(det->base_y - tp->y) > flick_thresh_) {
            det->prev_x = det->base_x;
            det->prev_y = det->base_y;
            tm = static_cast<touch_state_t>(tm | touch_flick_begin |
                                            touch_mask_moving);
          } else if ((tm == touch_touch) &&
                     (msec - det->base_msec > msec_hold_)) {
            tm = touch_hold_begin;
          }
        } else {
          static_cast<m5gfx::touch_point_t&>(*det) = *tp;
          tm = touch_touch_begin;
          if (msec - det->base_msec > msec_hold_ ||
              std::abs(det->base_x - tp->x) > ((flick_thresh_ + 1) << 2) ||
              std::abs(det->base_y - tp->y) > ((flick_thresh_ + 1) << 2)) {
            det->click_count = 0;
          }
          det->base_msec = msec;
          det->base_x = tp->x;
          det->base_y = tp->y;
          det->prev_x = det->base_x;
          det->prev_y = det->base_y;
        }
      }
      if (tm & touch_mask_moving) {
        det->x = tp->x;
        det->y = tp->y;
      }
    } else {
      tm = (tm & touch_mask_touch)
               ? static_cast<touch_state_t>((tm | touch_mask_change) &
                                            ~touch_mask_touch)
               : touch_none;
      if (tm == touch_touch_end) {
        det->base_msec = msec;
        det->click_count++;
      }
    }
    det->state = tm;
    return true;
  }

  bool update_detail(touch_detail_t* det, std::uint32_t msec) {
    touch_state_t tm = det->state;
    if (tm == touch_none) {
      return false;
    }
    tm = static_cast<touch_state_t>(tm & ~touch_mask_change);
    if (tm & touch_mask_touch) {
      det->prev_x = det->x;
      det->prev_y = det->y;
      if ((tm == touch_touch) && (msec - det->base_msec > msec_hold_)) {
        tm = touch_hold_begin;
      }
    } else {
      tm = touch_none;
    }
    det->state = tm;
    return true;
  }

  std::uint32_t last_msec_ = 0;
  std::int32_t flick_thresh_ = 8;
  std::uint32_t msec_hold_ = 500;
  m5gfx::LGFX_Device* gfx_ = nullptr;
  touch_detail_t touch_detail_[TOUCH_MAX_POINTS];
  m5gfx::touch_point_t raw_point_[TOUCH_MAX_POINTS];
  std::uint8_t detail_count_ = 0;
};

// ---------------------------------------------------------------------------
// IMU_Class — canned motion replacing the BMI270: a slow circular tilt sweep
// (the default) so tilt-follow/gaze motion is visibly exercised on the sim,
// plus a periodic X-axis shake burst calibrated against the demo's DIZZY
// thresholds (jerk ≥ 0.50, |Δx| ≥ 0.32 g, alternating swings inside 90–480 ms,
// 4 swings). Accelerometer format matches M5Unified's getImuData() layout
// (accel/gyro .x/.y/.z); the demo's screen mapping (accel.y→screenX,
// accel.x→screenY) applies unchanged.
struct imu_data_t {
  struct { float x; float y; float z; } accel;
  struct { float x; float y; float z; } gyro;
};

class IMU_Class {
 public:
  enum class Mode { Off, Static, Sweep };

  void begin(Mode mode) {
    mode_ = mode;
    start_ms_ = m5gfx::millis();
    enabled_ = mode != Mode::Off;
  }
  void setMode(Mode mode) { mode_ = mode; }
  bool isEnabled() const { return enabled_; }
  bool update() { return enabled_; }
  const imu_data_t& getImuData() {
    const std::uint32_t ms = m5gfx::millis() - start_ms_;
    data_.gyro.x = 0.0f;
    data_.gyro.y = 0.0f;
    data_.gyro.z = 0.0f;
    if (mode_ == Mode::Sweep) {
      // Device held flat, slowly rotating: screen-space tilt (accel.y→X,
      // accel.x→Y) sweeps a ±0.09 g circle — inside the 0.28 g full-travel
      // band, so the eyes wander in a slow circle without pinning.
      const float phase = (ms % 9000) / 9000.0f * 6.2831853f;
      float ax = -0.09f * std::cos(phase);
      float ay =  0.09f * std::sin(phase);
      // Every ~4.2 s a 340 ms horizontal shake burst: alternating sign flips
      // with |Δx| = 0.64 g per flip (jerk ≈ 1.28) — comfortably beyond every
      // engine threshold (kStrongShakeAxisDelta 0.32, kStrongShakeJerk 0.50).
      const std::uint32_t shake_ms = ms % 4200;
      if (shake_ms < 340) {
        constexpr std::uint32_t half_period = 85;  // 340 ms / 4 flips
        const float gate = ((shake_ms / half_period) % 2 ? -0.40f : 0.40f) *
                           0.80f;
        ax += gate;
        data_.gyro.x = gate > 0 ? -120.0f : 120.0f;
        data_.gyro.y = gate > 0 ? 120.0f : -120.0f;
      }
      data_.accel.x = ax;
      data_.accel.y = ay;
      data_.accel.z = 1.0f;
    } else {  // Static: device flat on a table
      data_.accel.x = 0.0f;
      data_.accel.y = 0.0f;
      data_.accel.z = 1.0f;
    }
    return data_;
  }

 private:
  bool enabled_ = false;
  Mode mode_ = Mode::Sweep;
  std::uint32_t start_ms_ = 0;
  imu_data_t data_{};
};

// ---------------------------------------------------------------------------
// Serial — stdout writer + select()-based stdin reader for the demo's serial
// command path ("idle", "happy", "loop dizzy", ...).
class SerialClass {
 public:
  void begin(unsigned long) {}

  void print(const char* text) { std::fputs(text, stdout); }
  void print(const String& text) { std::fputs(text.c_str(), stdout); }
  void println(const char* text) { std::fprintf(stdout, "%s\n", text); }
  void println(const String& text) { std::fprintf(stdout, "%s\n", text.c_str()); }
  void println() { std::fputc('\n', stdout); }

  template <typename... Args>
  void printf(const char* format, Args... args) {
    std::fprintf(stdout, format, args...);
    std::fflush(stdout);
  }

  int available() {
    refill();
    return static_cast<int>(buffer_length_ - buffer_index_);
  }

  int read() {
    refill();
    if (buffer_index_ >= buffer_length_) return -1;
    return static_cast<unsigned char>(buffer_[buffer_index_++]);
  }

 private:
  void refill() {
    if (buffer_index_ < buffer_length_) return;
    buffer_length_ = 0;
    buffer_index_ = 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv {0, 0};
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) return;
    ssize_t n;
    while ((n = ::read(STDIN_FILENO, chunk_, sizeof(chunk_))) > 0) {
      for (ssize_t i = 0; i < n; ++i) {
        if (buffer_length_ < sizeof(buffer_)) {
          buffer_[buffer_length_++] = chunk_[i];
        }
      }
      fd_set more;
      FD_ZERO(&more);
      FD_SET(STDIN_FILENO, &more);
      struct timeval more_tv {0, 0};
      if (select(STDIN_FILENO + 1, &more, nullptr, nullptr, &more_tv) <= 0) break;
    }
  }

  char buffer_[128];
  char chunk_[64];
  size_t buffer_index_ = 0;
  size_t buffer_length_ = 0;
};

// ---------------------------------------------------------------------------
// In_I2C_Class — the engine/demo's vibration writes go through M5.In_I2C (the
// IO-expander path on device). Hardware-only; the stub returns failure so the
// demo skips haptics on the sim.
class In_I2C_Class {
 public:
  bool writeRegister(int, std::uint8_t, const std::uint8_t*, size_t,
                     std::uint32_t) {
    return false;
  }
};

// ---------------------------------------------------------------------------
// AvatarDisplay — a concrete LGFX_Device running lgfx::Panel_sdl at 466x466
// (the StopWatch's round AMOLED geometry). board_M5StopWatch exists in M5GFX
// 0.2.26's boards.hpp, but the SDL autodetect switch has no case for it (it
// falls through to the 320x240 default), so the simulator configures the
// panel itself, replicating M5GFX::autodetect()'s host-side setup: 466x466,
// 1:1 scaling, rotation 0, Touch_sdl attached with full-screen extents.
//
// ROUND GEOMETRY: Panel_sdl (0.2.26) has no shaped-window support, so the
// round device face is emulated by drawing a bezel + mask pass on the canvas
// itself after each engine frame (applyRoundOverlay() in avatarsim_main.cpp):
// corner/background pixels outside the inscribed circle are blanked and a
// thin bezel ring marks the physical glass edge.
class AvatarDisplay : public m5gfx::LGFX_Device {
 public:
  AvatarDisplay() {
    auto* panel = new m5gfx::Panel_sdl();
    panel_.reset(panel);
    auto cfg = panel->config();
    cfg.memory_width = 466;
    cfg.memory_height = 466;
    cfg.panel_width = 466;
    cfg.panel_height = 466;
    cfg.bus_shared = false;
    panel->config(cfg);
    panel->setWindowTitle("Hermes Watch - avatar sim");
    panel->setScaling(1, 1);
    panel->setRotation(0);
    this->panel(panel_.get());

    auto* touch = new m5gfx::Touch_sdl();
    touch_.reset(touch);
    auto touch_cfg = touch->config();
    touch_cfg.x_min = 0;
    touch_cfg.x_max = 465;
    touch_cfg.y_min = 0;
    touch_cfg.y_max = 465;
    touch_cfg.bus_shared = false;
    touch->config(touch_cfg);
    panel->touch(touch_.get());
  }

  // Exposed so main can identify the SDL panel if ever needed.
  m5gfx::Panel_sdl* sdlPanel() { return panel_.get(); }

 private:
  std::shared_ptr<m5gfx::Panel_sdl> panel_;
  std::shared_ptr<m5gfx::ITouch> touch_;
};

// ---------------------------------------------------------------------------
// M5 facade. The engine/demo references satisfied:
//   M5.config(), M5.begin(), M5.update(msec), M5.Display, M5.BtnA/B/C,
//   M5.Touch, M5.Imu, M5.In_I2C.
struct M5_Class {
  AvatarDisplay Display;
  Button_Class BtnA;
  Button_Class BtnB;
  Button_Class BtnC;
  Touch_Class Touch;
  IMU_Class Imu;
  In_I2C_Class In_I2C;

  void begin() {
    Touch.begin(&Display);
    Imu.begin(imu_mode_);
  }
  void update(std::uint32_t msec) {
    // Panel_sdl maps arrow keys to emulated GPIO pins by default
    // (LEFT=39, DOWN=38, RIGHT=37, UP=36). Emulated GPIOs idle HIGH (1) and
    // the pressed key reads LOW (0) — same active-low wiring as the physical
    // StopWatch buttons. StopWatch mapping on the sim:
    //   BtnA (left)   = GPIO39 = SDL LEFT arrow
    //   BtnB (right)  = GPIO37 = SDL RIGHT arrow
    //   BtnC (center) = no keyboard mapping on the sim (unused by engine)
    BtnA.setRawState(msec, !m5gfx::gpio_in(39));
    BtnB.setRawState(msec, !m5gfx::gpio_in(37));
    BtnC.setRawState(msec, false);
    Touch.update(msec);
  }
  void update() { update(millis()); }

  void setImuMode(IMU_Class::Mode mode) { imu_mode_ = mode; }

 private:
  IMU_Class::Mode imu_mode_ = IMU_Class::Mode::Sweep;
};

extern AvatarDisplay Display_host_instance;
extern M5_Class M5;
extern SerialClass Serial;

}  // namespace m5

// Hoist the M5Unified-style global surface (M5, Serial, String, enums) into
// the enclosing scope — on ESP, M5Unified.h does exactly this; the engine and
// demo code rely on those names being unqualified.
using m5::String;
using m5::M5;
using m5::Serial;