# Vendored code — hermes-watch firmware (PRIVATE, not distributed)
- avatar_engine.{cpp,h}: m5stack-stopwatch-avatar (KK), AGPL-3.0-or-later, verbatim.
  main.cpp.device-reference = their original device main (AGPL, reference only).
- tiny_websockets/ + ArduinoWebsockets.h: gilmaimon/ArduinoWebsockets v0.5.4
  (GPL-3.0), vendored via Voice-Assistant-5 patches (setInsecure on ESP32).
- hg.v1 client protocol: Adolanium/hermes-gateway-sdk (MIT) reimplemented here
  (hand-written, clean-room from docs/protocol.md).
