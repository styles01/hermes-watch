# Gateway — the `gadget` platform plugin

`gateway/` is a Hermes-agent platform plugin (MIT). It is our production fork of
Adolanium/hermes-gadget-sdk's plugin with substantial additions: paced TTS playback pump
with stall recovery, whole-file TTS delivery mode for voice devices, reasoning-prose
stripping tuned for streamed replies, OTA queue + version-tagged image inspection, and
the serial-style CLI.

## Install
Copy `gateway/` into your Hermes profile's `plugins/` directory (or configure it per your
Hermes version's plugin mechanism — see Hermes docs), then restart Hermes. The hub listens
on `:8765` (LAN). Give it a fixed LAN IP/DNS name; devices store the host directly.

## Pairing
1. Device boots → `pairing{code}` appears on its screen and in the gateway log.
2. Approve: `hermes pairing approve gadget <code>` (or the allowed-users list).
3. Device gets `paired`, claims home if it's the first device, and stays paired in NVS.

## CLI
```
hermes gadget list
hermes gadget info --device "Hermes Watch"
hermes gadget update --device "Hermes Watch" --latest     # OTA
hermes gadget update --device ... --image firmware.bin
```

## Files
adapter.py (platform wiring) · hub.py (device hub + audio pump) · protocol.py (hg.v1
helpers) · ota.py (image inspect + stream + queue) · store.py (device registry) ·
cli.py (host-side CLI) · textfmt.py (reply shaping) · audio.py/imaging.py/releases.py/
compat.py/runtime.py/setup.py/tools.py (support).
