# Hermes Voice-Gadget Integration — Backend Learnings

Lessons from bringing up the Hermes Watch (`<device-id>`) against a live Hermes gateway.
These are the **gateway-side contracts** the firmware depends on — the things that cost
us a day of debugging and that the README's protocol docs don't spell out.

## 1. Voice replies: TTS pipeline

- The gateway turns the agent's **text** reply into speech per device: the gadget
  adapter synthesizes locally (its configured TTS), sends `audio.start {stream, rate,
  format:"pcm16"}`, binary PCM16LE frames (mono, hub-paced at `PLAYBACK_LEAD_S` =
  0.5 s ahead of wall clock), then `audio.end {stream}`.
- The device must **drain** its ring buffer after `audio.end` before declaring
  playback done. **`audio.end` arrives mid-frame often** (not on a 20 ms boundary):
  the drain gate must flush a **zero-padded partial tail** or the device wedges in
  "speaking" and a minute later a watchdog fires (`PLAYBACK STOPPED` on screen).
  *This was the "blue ring hang" of v0.5.13.*
- Device screen state after a watchdog or after playback-done must be restored
  **explicitly**: the idle ring state erased its label band but never the arc annulus
  (205–230 px), leaving a stale blue ring on an otherwise black screen. Idle = erase
  arc + label band + status band.

## 2. Reply clamp for spoken output

- `VOICE_REPLY_MAX_CHARS = 400` (adapter) — clamps device-bound text at a sentence
  edge (plus `_strip_reasoning_prose` before synthesis). The full-length agent reply
  is logged (the gateway logs the raw size) but the device gets the clamp. If your
  agent's persona is chatty (a strategist brain, not a voice assistant brain), the
  watch speaks the *head of an essay* — garble in, garble out. Fix the *prompt*, not
  the clamp.

## 3. Persona and tools for routed device sessions

- Per-turn profile routing (`device_profiles` / `agent_profiles`) sends the turn to
  the target profile's home — **that profile's persona, config, and toolset apply**.
  A full-featured agent profile with no voice-mode rules produces monologues and
  zero tool calls. Fix without code: a **channel override** in the routed profile's
  `config.yaml`:
  ```yaml
  platforms:
    gadget:
      channel_overrides:
        <device-id>:
          system_prompt: >-
            VOICE MODE: 1-2 short sentences, actions via tool, never discuss internals.
  ```
  Caveat: overrides are read from the runner's **in-memory** config — a running
  gateway picks the change up on its next config load (boot), not mid-run.
- Keep a *voice-scoped* prompt on any device-facing profile: ≤30 spoken words,
  act-don't-essay, never mention restarts/firmware/debug internals. The brain echoing
  your debugging vocabulary ("I've staged a fix") back as speech is a prompt problem.

## 4. Auth: the silent-refusal trap (adapter delegation)

- A device turn's `SessionSource` can carry **`role_authorized=True`** — set it when
  the TRANSPORT has already authenticated the device by its own means (the gadget hub:
  per-device HMAC key challenge + operator pairing). The authorization mixin admits
  adapter-verified sources **before** home-scoped pairing/allowlist checks.
- Why it matters: when the turn is re-homed to a routed profile, the canonical
  identity changes and authorization reads land in a scope where the device id is in
  **no allowlist** — every turn is then refused silently (2 ms `turn.start`/`turn.end`
  "success" brackets, no agent run, no log at INFO). Commands (`/status`) still run
  and echo a pairing-code reply — that contrast is the tell.
- Belt-and-braces: the hub drops text/audio from **unpaired** devices with a notice.

## 5. STT for short voice clips

- The local STT path on macOS resolves to the **openai-whisper CLI** (faster-whisper
  is not bundled into the gateway env; whisper-cpp is Windows-ARM-only).
- `stt.local.model` must be a **catalog** name (`base|tiny|small|medium|large-v3|turbo`)
  — an unknown name silently falls back to `base` (`_normalize_local_model`). Watch
  logs: `Transcribed … via local STT command (base, N chars)` — if you set turbo but
  the log says `base`, the name fallback bit you.
- `base` cannot handle watch-grade audio reliably: it produced "a coffee machine",
  "turn off the car switching" from clear speech. `turbo` + `language: en` (never
  auto-detect on 1–3 s clips) transcribed the same recordings perfectly.
- The saved upload wavs live in `<hermes-home>/cache/audio/` — re-transcribe an old
  clip offline to A/B an STT change without touching the device.

## 6. Device-side watchdogs that earned their keep

| Watchdog | Trigger | Action |
|---|---|---|
| mic stall | micOpen, no good `record()` for 3.5 s | `Mic.end` → re-open (×2) → clean `audio.cancel` |
| audio stall | playStream, no inbound frames 5 s | stop speaker, reset, surface on screen |
| playback cap | one stream > 90 s | stop speaker, reset, face restore |
| drain stall | playEnded, ring not draining 3 s | stop speaker, reset, face restore |
| turn stall | turnActive, no hub frame 180 s | clear turn state |

Every watchdog path must end in **return-to-idle** (voice reset + resting face +
ring/status erase). A watchdog that resets state but not the *screen* leaves the user
staring at a ghost.

Also: a dead WebSocket must clear **all** turn state (`s_turnActive`, listening flag)
or the next PTT reports "resource busy" forever.

## 7. OTA to devices through a live gateway

Firmware updates stage under `plugin-data/gadget/updates/<device>/` (bin + json) via
the hub's `ota` module and stream to the device on its next check-in — **zero gateway
restarts, zero downtime**. Compile → inspect image (version/sha) → stage → poll the
status file. The device applies, reconnects, and reports its new version in `hello`.