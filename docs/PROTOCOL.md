# hg.v1 wire protocol (device ⇄ hub)

One plain-TCP WebSocket to `<gateway>:8765/gadget`, subprotocol `hermes-gadget.v1`.
All frames are JSON text on the default channel; binary frames are 4-byte prefixed
(channel, stream-id, seq LE16).

## Handshake
1. device → `hello{name, board, firmware, caps{display, mic, speaker, inputs, talk_mode, ota}}`
2. hub → `challenge{nonce, enrolled}` → device → `auth.hmac{…}` → hub → `welcome{paired}`
3. unpaired devices get `pairing{code}`; approve hub-side, `paired` follows.

## Core frames (device ⇄ hub)
- `text{id, text}` — a conversational turn from the device
- `turn.start / reply{text} / reply.delta{text} / turn.end` — agent turn lifecycle
- `transcript{text}` — live STT echo of the device utterance
- `notice{text, ttl_s}`, `prompt{id, title, text, ttl_s}` → `prompt.reply{id, answer}`
- `audio.start{stream, rate, format} / audio.end / audio.abort / audio.cancel` (TTS + upload both)
- binary ch 0x01: PCM16 mic upload (device→hub) and TTS play (hub→device), stream-tagged
- `cancel` — barge-in; `session.new` — fresh session; `ping/pong{ts}` — heartbeat
- `action{name,args}` → `action.result` (device caps.actions), `state/event` passthrough
- binary ch 0x03: OTA frames (docs/OTA.md)

## Auth
Per-device 32-byte key (base64 in the gateway store). Device control frames carry an
HMAC-SHA256 over `hermes-gadget/v1|<device_id>|<nonce>`; the OTA offer adds
`|ota|<id>|<nonce>|<sha256>|<size>`. Keys live only in the gateway store + device NVS —
never in this repo; pairing approval is a manual, hub-side step.

Full frame docs live upstream: Adolanium/hermes-gateway-sdk `docs/protocol.md` (MIT).
