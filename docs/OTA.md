# Over-the-air updates

Channels: OTA control frames share the JSON control surface; firmware bytes ride binary
channel 0x03 as 4 KB chunks (stream+seq header, like audio).

Flow (all HMAC-authorized):
1. hub → `ota.offer{stream, size, sha256, version}` (version/board come from the image's
   app-descriptor tags `HGVER=` / `HGBOARD=` — Arduino builds report a generic project
   name, so the tags are the source of truth)
2. device → `ota.ready{nonce}` (fresh 16-byte random, base64)
3. hub → `ota.begin{mac = HMAC-SHA256(key, "hermes-gadget/v1|ota|<id>|<nonce>|<sha256>|<size>")}`
4. device → `ota.ack{offset:0}`; hub streams binary chunks; device ACKs every 16 KB and
   verifies SHA-256 in-stream
5. hub → `ota.end` → device finalizes the other 3 MB slot → `ota.done{version}` → reboot
6. Probation: the new image is only marked valid after it reaches the hub (`welcome`);
   a bad image that never connects rolls back automatically.

Safety rails: 30 s in-progress silence watchdog (device), queue retries on disconnect
(hub), `ota.abort` on any protocol error, image size cap 3 MB minus partition headroom.
