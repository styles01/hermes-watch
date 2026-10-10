## [0.5.14] - 2026-10-09

### Fixed
- **Blue-ring idle wedge**: ring IDLE now erases the arc annulus + status band; watch
  returns to a fully black screen between turns (`PLAYBACK STOPPED` text no longer lingers).
- **TTS drain**: `audio.end` mid-frame no longer strands the device in SPEAKING —
  partial tail frame is zero-padded and flushed; drain completes ~40 ms after `audio.end`.
- **Watchdog restores**: all voice watchdogs (mic stall, audio stall, 90 s cap, drain
  stall) now restore the resting face + clear status via `VH.playbackIdle`.

### Added
- `docs/GATEWAY-LEARNINGS.md` — gateway-side integration lessons (TTS pipeline, reply
  clamp, profile routing prompts, adapter-delegation auth, STT model selection, watchdogs).

# Changelog

## v1.0.0 — first public release (device firmware v0.5.9)
- Device: 4-screen circular UI (avatar / agents / settings / quick commands), KK avatar
  engine, PTT voice turns, live reply ticker, tap-to-stop + touch-hold cancel, playback
  watchdogs, state-coded ring (idle = no ring), tap-cycled persistent faces, IMU tilt,
  diagnostics view, WiFi OTA with rollback + version tags.
- Gateway: gadget platform plugin (device hub, pairing, audio pacing, whole-file TTS
  mode, reasoning-strip, OTA queue).
- Sim: SDL desktop simulator sharing the engine.
- History note: public history starts at v1.0.0 — earlier version tags (v0.1…v0.5.9)
  continue inside the firmware itself (`HGVER=` tag).
