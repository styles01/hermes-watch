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

## v1.1.0 — device firmware v0.5.29 (2026-10-10)

### Added
- **Approval cards** (`prompt` frames): agent yes/no questions render as an APPROVAL
  card — A=YES, tap=B NO, hold-B=defer (re-raises in 45 s; ttl expiry announces itself).
- **Readable reply cards**: the full reply (up to 448 chars) lands as a paged 8-row
  card with page tags; B pages (last page hold-B = answer by voice), A dismisses,
  auto-shows after audio drains, 12 s auto-dismiss.
- **WAITING-ON-USER state**: magenta slow-pulse ring while the hub waits on the
  device's answer; device watchdogs (turn-stuck / read-idle) stand down when parked.
- **Tool-progress ticker**: server `status` frames paint a live progress line under
  the disc (1 Hz, second-boundary gated); honest "WORKING Ns" fallback during turns.
- **Transcript echo**: the user's transcribed words surface in the status band
  ("YOU: …") during thinking.
- Server-side: `waiting` flag on status frames + `level` on notices (error styling),
  turn-id replay when a device connection is replaced mid-turn, pump-abort awaits,
  turn-end matched by identity, completion proceeds for unpaired sessions.
- `docs/SERVER-SETUP.md` — the complete server-side guide (plugin install, profile
  routing + toolset binding, pairing, env keys, Google Home helper).

### Fixed
- **Render stall**: prompt/reply cards repainted EVERY loop pass (full-card redraw
  saturated SPI — the ring visibly dropped below 24 fps); cards now paint once and
  freeze, redrawing only on page flips / content changes.
- **Overlay bleed**: cards now erase the stale ring annulus + ring label band +
  ticker strip on paint (panel-ownership law).
- Deferred prompts never re-raised (timer had no consumer) — fixed.
- Commands-screen: hold-to-send now REQUIRES two holds (arm, then confirm) — no more
  accidental billed agent turns; cursor position persists across screen entries.
- Face-truth: streaming replies show THINKING (not LISTENING); local stop shows
  STOPPED instantly; streaming errors surface with `level="error"`.
- Hub: de-hardcoded the Google Home helper path (`GOOGLE_HOME_CMD` env / auto-resolve).

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
