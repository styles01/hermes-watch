# Hermes Watch — avatar SDL simulator (Phase 0)

Native macOS build of the REAL KK avatar engine (`engine/avatar_engine.{cpp,h}`,
copied verbatim from `reference/m5stack-stopwatch-avatar`, **AGPL-3.0-or-later**;
see `LICENSE.kk-avatar`) running against a minimal M5Unified shim
(`include/avatarsim_shim.h`, entry `include/M5Unified.h`) on M5GFX's official
`lgfx::Panel_sdl` host panel — no PlatformIO, no CMake, one clang++ command.

## Build

```bash
sim/build.sh                      # binary -> sim/hermes-watch-avatar-sim
```

Requires: brew `sdl2` (Homebrew prefix assumed `~/homebrew`; override in the
script), M5GFX 0.2.26 (Arduino libraries dir in the script), clang++ arm64.
Flags: `-DM5GFX_SHOW_FRAME -DM5GFX_BACK_COLOR=0x222222u` draw the window frame.

## Run

```bash
AVSIM_DEEP_PIPELINE=1 sim/hermes-watch-avatar-sim        # interactive window
```

- Window is 466x466 with a round bezel/mask overlay (corners blacked, rim ring).
- **Mouse = touch** (tap / drag / hover through the real Touch_Class state
  machine: wasPressed/wasClicked/wasHold/isFlicking, hold 650 ms, flick 10 px).
- **←/→ arrows = BtnA/BtnB** (Panel_sdl emulated GPIO39/37, active-LOW —
  idle HIGH; matches the physical buttons).
- Serial-replacement: **stdout prints, stdin = commands** (pipe or tty):

```
idle listening thinking happy excited curious confused angry surprised sad sleepy dizzy
once|loop|pingpong <expression>          # playback-mode test
Hold A+B 1s = hardware diagnostics screen
```

## Environment switches (vendored M5GFX patch)

`Panel_sdl.cpp` carries three small env-gated patches, all inert by default:
- `AVSIM_DEEP_PIPELINE=1` — **required for 60 fps here**: replaces the
  per-primitive 1 ms semaphore handshake (`post in → wait out ≈ 1 ms`) with a
  never-blocking user thread + pump-side full drain. Without it the engine's
  per-pixel eye rasterization (~1.5k pixel-locks/frame) costs ~0.13 ms per lock
  → ~400 ms/render (~1.3 fps). With it: **fps=60.0, render_avg=0.22 ms,
  frame_avg=16.67 ms** (locked to VSYNC).
- `AVSIM_NO_VSYNC=1` — create the renderer without `SDL_RENDERER_PRESENTVSYNC`
  (headless/agent runs where an occluded window can stall presents).
- `AVSIM_CAPTURE_AT=<n>` / `AVSIM_CAPTURE_AFTER_MS=<ms>` + `AVSIM_CAPTURE_PATH=<f>`
  — SDL-side screenshot: after the nth pump pass (or after n ms) saves one BMP
  via `SDL_RenderReadPixels` (no macOS TCC screen-recording permission needed).

## Verified (2026-10-05 session)

- 60 fps-class: `[avatar perf] fps=60.0 render_avg=0.22ms render_max=0.74ms
  frame_avg=16.67ms` for minutes-long runs.
- Serial commands accepted (`Command accepted: HAPPY`), engine morphs visible:
  HAPPY = wide squashed horizontal capsule/arc eyes (keyframes w290/h72/rot−112).
- Screenshot: SDL-side capture of idle + happy states (466x466 PNG from BMP).

## Layout

```
sim/
  build.sh                 # one-command build (hand-written clang++ line)
  include/M5Unified.h      # shim entry (engine code includes <M5Unified.h>)
  include/avatarsim_shim.h # M5_Class/Display/Btn/Touch/Imu/In_I2C/Serial/String
  src/avatarsim_main.cpp   # demo harness (re-implements reference main.cpp;
                           #  M5IOE1 vibration stubbed) via Panel_sdl::main
  src/avatarsim_shim.cpp   # shim globals (m5::M5, m5::Serial)
  src/avatarsim_probe*.cpp # perf-verification probes (harness-only, no engine)
  engine/                  # UNMODIFIED avatar engine + original main example
  LICENSE.kk-avatar        # AGPL-3.0-or-later (engine upstream)
```

Shim is host-only: `#error` guard rejects `ESP_PLATFORM`/`ARDUINO`.

## Notes / gotchas

- `lgfx::Panel_sdl::main(user_func, msec_step_exec)` — pass **0**, not the
  example's 128: the debugger-detect thread + step-exec path serializes
  rendering behind 60 Hz semaphore handshakes on this harness (vendored patch
  also refuses to spawn the detector when step_exec is 0).
- M5Unified semantics on host: `M5.update()` drives BtnA/B (GPIO) + Touch
  (Panel_sdl `getTouchRaw`) with the same state machines as the real lib.
- The engine never draws outside the center band, so the round-mask overlay
  can safely re-black corners at ~22 Hz with 4 fillRects + rim circles.
- Re-compile for device after each sim change (RAM/flash budgets stay honest) —
  see the StopWatch ops playbook.
