# AGENTS.md — Hermes Watch (read before ANY work here)

> **Non-negotiable, learned the hard way across 10+ gateway restarts (2026-10-09).**
> If you take one thing from this file: **the plugin's own seams are hot-reloadable;
> core gateway mechanisms generally are not. Pick plugin seams first.**

## The seams (verified in code — which changes need what)

| Change | Where | Hot? | How to activate |
|---|---|---|---|
| Voice-mode prompt wording | gadget plugin's `prompt_context()` (hub.py) | **YES — plugin code** | edit + `reload-plugins` (handlers-only, no restart) |
| Adapter/hub behavior (auth stamp, routing, frame handling) | gadget plugin `adapter.py`/`hub.py` | **YES — plugin code** | edit + `reload-plugins` (devices reconnect ~1 s) |
| STT model/language for routed device turns | `<routed profile home>/config.yaml` `stt:` block | **YES — profile config** (signature cache, invalidate-on-write) | edit → next turn live. NOTE: routed turns read the *profile home* config, NOT the transport home |
| TTS / reply clamp / speech pacing | gadget plugin `adapter.py` (VOICE_REPLY_MAX_CHARS, cap_voice_reply) | **YES — plugin code** | edit + `reload-plugins` |
| Device firmware | firmware/ | **YES — OTA** | stage in `plugin-data/gadget/updates/<device>/` via hub `ota` module; device pulls on check-in. NEVER ask permission, never wait |
| `channel_overrides` (core runner config) | runner `__init__` boot snapshot (`run.py` ~3455) | **NO — boot-frozen** | requires gateway restart. (Docs claim per-turn fresh — doc-vs-behavior BUG, do not trust) |
| Identity / authz / session-key derivation | core import-time binding | **NO** | gateway restart (correctness-first; do not monkey-patch live) |
| OTA staging | `plugin-data/gadget/updates/` | hot always | file-based queue |

## THE binding (verify before anything else)
**Gadget turns resolve their toolset from `platform_toolsets.gadget` in the ROUTED
profile's config. The default composite `hermes-gadget` does NOT exist in the toolsets
registry → `resolve_toolset('hermes-gadget')` = [] → the brain gets ZERO tools and can
only write prose (tool_turns=0, essays, "I can't do that").** Telegram works because
`hermes-telegram` is a registered builtin composite (60 tools). Fix (2026-10-09): an
explicit `platform_toolsets.gadget:` list in the venus profile config → 23 tools resolve.
Config reads are per-turn (signature-cached) so this edit is HOT. Probe proof: session
20261009_202356_f66d5304 went from tool_turns=0 (first turn) to 5 terminal tool-turns
(after the config edit) on the SAME session.

## Rules (each one paid for in outages)

1. **Before touching core config for a device problem, STOP.** Ask: "is this a plugin
   seam?" The plugin already owns: per-device spoken-mode prompt (`prompt_context`),
   reply clamping, auth stamping (`role_authorized`), profile routing stamps, OTA.
   Core config is almost never the right lever for device UX.
2. **Routed turns resolve config against the ROUTED PROFILE's home** (`get_config_path`
   = profile home under `_profile_runtime_scope`). An edit in the transport
   `config.yaml` is invisible to routed device turns — write profile-home config
   (e.g. `stt:`) into the profile, hot.
3. **`reload-plugins` is the approved no-restart activation path** (handlers-only,
   control-socket verb). Use it; full gateway restarts are a last resort (user:
   "extremely, extremely disruptive") — batch any that remain and ask once.
4. **Voice replies are SPOKEN.** Cap words in the *prompt* (30 spoken words max,
   act-don't-discuss, never mention restarts/firmware/'staged fixes'), clamp at a
   sentence edge in the adapter (belt), strip reasoning prose before synthesis
   (braces). The brain echoing your debugging vocabulary back as speech is a prompt
   failure — fix the prompt seam, not the user's workflow.
5. **STT is shared core code** (`run_inbound` → transcription_tools) but its model
   comes from the profile-scope config: catalog names only (`base|tiny|small|medium|
   large-v3|turbo`) — unknown names **silently fall back to `base`**. Always
   `language: en` for device clips (auto-detect misfires on 1–3 s clips). Saved
   uploads live in `<hermes-home>/cache/audio/*.wav` — re-transcribe offline to A/B
   an STT change without touching the device.
6. **Every voice watchdog path must end in return-to-idle** (voice reset + resting
   face + erase ring arc + erase status bands). A watchdog that resets state but not
   the screen leaves a ghost (the "blue ring of death": the done-gate sat AFTER an
   `if (!rUsed) return;` — a drained ring early-returned forever and SPEAKING never
   released. Order your gates: done-check BEFORE empty-check).
7. **`audio.end` arrives mid-frame.** The device drain must flush a zero-padded
   partial tail or it wedges in SPEAKING.
8. **OTA: just flash.** Compile → inspect image (version/sha) → stage → poll status
   file. Standing user order: never wait, never ask.
9. **Never full-restart without explicit user OK.** Never install toolchains/packages
   without explicit OK. Voice notes to Telegram = .ogg/.opus. Never relay Google's
   audio to Telegram — decode locally, answer in text.
10. **Device-side bugs reproduce via the fake-watch probe** (WS client + HMAC key
    from devices.json, works against the live hub without disturbing the device).
    `/status` from the probe exercises the auth gate end-to-end — it surfaces silent
    refusals that voice turns swallow without logging.

## Known-open gateway bugs (upstreamable)
- `channel_overrides` documented as per-turn fresh; actually boot-cached in the
  `GatewayConfig` snapshot. Fix: signature check + rebuild per turn (or a 60 s tick)
  + a log line on reload.
- Config system has no single hot/cold contract: the `load_config()` dict path is
  hot, `runner.config` dataclass is cold, identity binds at import — every change
  is archaeology. Unify or document.
- Authz re-homes authorization reads when identity routing re-homes the source;
  adapter-verified devices (HMAC + operator pairing) need `role_authorized` stamped
  by the adapter to dodge the trap. Make role-verification a protocol-level property.
