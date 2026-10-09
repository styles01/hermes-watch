// SPDX-License-Identifier: AGPL-3.0-or-later
// hermes_voice.h — hg.v1 voice for hermes-watch: PTT mic upload + TTS playback
//
// Wire (from the gateway's plugins/gadget/{hub.py,protocol.py}): a binary WS
// frame = [channel=0x01][stream u8][seq u16 LE] + raw mono PCM16LE samples.
//   upload   : text audio.start{stream,rate:16000,format:"pcm16"} → frames → text audio.end{stream}
//   playback : server audio.start{stream,rate,...} → binary frames → text audio.end / audio.abort
// Hub rules: 60 s max utterance, <0.25 s discarded, frames ≤1 MiB, seq gaps tolerated.
//
// PTT surface: hold the RIGHT button / BtnB (or serial 'T' once to open, again to close).
#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdint>

static constexpr uint32_t kVoiceRate     = 16000;
static constexpr size_t   kUttMaxBytes   = 24 * kVoiceRate * 2;  // self-cap 24 s (hub allows 60)
static constexpr size_t   kUttFrameBytes = 640;                  // 20 ms = 320 samples
static constexpr size_t   kPlayRingBytes = 160 * 1024;           // 5 s @ 16k mono16

// ------------------------------------------------------------- state ----
struct VoiceState {
  // upload side (PTT)
  bool     micOpen = false;
  bool     micByButton = false;        // PTT owned by BtnB press (release-closes only then)
  uint16_t uttStream = 1;
  uint16_t uttSeq = 0;
  size_t   uttBytes = 0;
  bool     uttTrunc = false;
  uint32_t uttStartMs = 0;
  uint8_t *uttBuf = nullptr;                 // PSRAM linear capture buffer

  // playback side (TTS)
  uint8_t *ring = nullptr;                   // PSRAM ring: head/tail/used
  volatile size_t rHead = 0;                 // write index
  volatile size_t rTail = 0;                 // read index
  volatile size_t rUsed = 0;                 // valid bytes
  uint16_t playStream = 0;                   // 0 = no active stream
  uint16_t playSeqExpected = 0;
  uint16_t playSeqGaps = 0;
  bool     playFirst = true;
  size_t   playBytes = 0;
  bool     speakerStarted = false;
  bool     playEnded = false;                // audio.end arrived; drain then idle
  uint32_t lastAudioInMs = 0;                // v0.5.9 stall watchdog: last inbound TTS frame
};
static VoiceState V;

// Hooks implemented by the sketch as fn pointers (the sketch's helpers live in
// an anonymous namespace — extern symbols can't reach them; lambdas can).
struct VoiceHooks {
  bool   (*ready)();                              // connected && paired && wifi
  void   (*noRam)();                              // buffer-alloc failure UX
  void   (*cancel)();                             // {"type":"cancel"} barge-in
  bool   (*sendText)(const char *, size_t);
  size_t (*sendBin)(const uint8_t *, size_t);
};
static VoiceHooks VH = {};  // sketch fills these in setup()

static inline bool voiceAllocBuffers() {
  if (!V.uttBuf)
    V.uttBuf = static_cast<uint8_t *>(heap_caps_malloc(kUttMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!V.ring)
    V.ring = static_cast<uint8_t *>(heap_caps_malloc(kPlayRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return V.uttBuf && V.ring;
}

static inline void voicePlaybackReset() {
  V.rTail = V.rHead = V.rUsed = 0;
  V.playStream = 0;
  V.playSeqExpected = 0;
  V.playSeqGaps = 0;
  V.playFirst = true;
  V.playBytes = 0;
  V.playEnded = false;
  V.speakerStarted = false;
}

// ------------------------------------------------------ playback (rx) ----

static void voiceRingPush(const uint8_t *data, size_t len) {
  if (!V.ring || len == 0 || len > kPlayRingBytes / 2) return;  // absurd frame guard
  // Make room by dropping the OLDEST bytes (keeps latency bounded; the hub
  // paces frames ~0.5 s ahead of playback, so overflow = playback stalled).
  size_t room = kPlayRingBytes - V.rUsed;
  while (room < len) {
    if (V.rUsed == 0) break;
    const size_t drop = len - room;
    if (drop >= V.rUsed) {
      V.rUsed = V.rTail = V.rHead = 0;
      break;
    }
    V.rTail = (V.rTail + drop) % kPlayRingBytes;
    V.rUsed -= drop;
    room = kPlayRingBytes - V.rUsed;
  }
  for (size_t i = 0; i < len; ++i) {
    V.ring[V.rHead] = data[i];
    V.rHead = (V.rHead + 1) % kPlayRingBytes;
    ++V.rUsed;
  }
}

namespace Ota { void onBinary(const uint8_t *p, size_t len); }  // fwd: hermes_ota.h (included after)
// Binary frame from the server → voice path (TTS). Header: ch, stream, seq u16LE.
static void voiceOnBinary(const uint8_t *p, size_t len) {
  if (len < 4) return;
  const uint8_t ch = p[0];
  const uint16_t stream = p[1];
  const uint16_t seq = static_cast<uint16_t>(p[2] | (p[3] << 8));
  if (ch == 0x03) { Ota::onBinary(p, len); return; }   // v0.5.4: firmware frames
  if (ch != 0x01 || !V.playStream || stream != V.playStream) return;  // ignore foreign
  if (V.playFirst) {
    V.playFirst = false;
    V.playSeqExpected = seq;
  } else if (seq != V.playSeqExpected) {
    if (seq > V.playSeqExpected) ++V.playSeqGaps;   // gaps counted, tolerated
    V.playSeqExpected = seq;                        // resync
  }
  ++V.playSeqExpected;
  voiceRingPush(p + 4, len - 4);
  V.playBytes += len - 4;
  V.lastAudioInMs = millis();
}

// ------------------------------------------------------ speaker feed ----
// M5Unified: playRaw() with the SAME channel appends to that channel's queue;
// data must remain valid until played → static rotating blocks.
// Feed ≤ 2 × 20 ms per loop to bound the loop stall.
static void voiceSpeakerPump() {
  if (!V.rUsed) return;
    for (int iter = 0; iter < 2; ++iter) {
    if (V.rUsed < kUttFrameBytes) break;
    // 4-block rotation: playRaw queue keeps a reference to the last TWO
    // queued buffers (wavinfo flip pair) — a 2-pool overwrites a block the
    // speaker task is still reading → waveform tearing (the 'garbled' bug).
    static int16_t pool[4][320];
    static int poolSel = 0;
    int16_t *dst = pool[poolSel];
    poolSel = (poolSel + 1) & 3;
    uint8_t *bytes = reinterpret_cast<uint8_t *>(dst);
    for (size_t i = 0; i < kUttFrameBytes; ++i) {
      bytes[i] = V.ring[V.rTail];
      V.rTail = (V.rTail + 1) % kPlayRingBytes;
    }
    V.rUsed -= kUttFrameBytes;
    M5.Speaker.playRaw(dst, kUttFrameBytes / 2, kVoiceRate, false, 1, 0);
    if (!V.speakerStarted) {
      V.speakerStarted = true;
      Serial.println("[voice] speaker feed started");
    }
  }
  // stream over + ring drained → release the playback slot
  if (V.playEnded && V.rUsed == 0 && V.playStream) {
    Serial.printf("[voice] playback done: %u B seqgaps=%u\n",
                  (unsigned)V.playBytes, (unsigned)V.playSeqGaps);
    M5.Speaker.stop();        // v0.5.2: queued playRaw tail would otherwise keep
                              // hissing/beeping after audio.end (channel linger)
    voicePlaybackReset();
  }
}

// audio.start / audio.end / audio.cancel senders (JSON, via sketch hook)
static inline bool hgSendVoiceJsonType(const char *type, const char *extraJson) {
  char buf[160];
  if (extraJson && extraJson[0])
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",%s}", type, extraJson);
  else
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\"}", type);
  if (!VH.sendText) return false;
  return VH.sendText(buf, strlen(buf));
}

// ------------------------------------------------------------ upload ----

// PTT press: open the hub-side upload.
static void voiceStartListening(bool byButton) {
  if (!VH.ready || !VH.ready()) return;
  if (!voiceAllocBuffers()) { if (VH.noRam) VH.noRam(); return; }
  if (V.playStream) {  // talking over the avatar while it speaks = barge-in
    if (VH.cancel) VH.cancel();
  }
  V.uttStream = V.uttStream ? V.uttStream : 1;  // keep identity; hub tracks by id
  V.uttSeq = 0;
  V.uttBytes = 0;
  V.uttTrunc = false;
  V.uttStartMs = millis();

  char extra[96];
  snprintf(extra, sizeof(extra), "\"stream\":%u,\"rate\":%u,\"format\":\"pcm16\"",
           (unsigned)V.uttStream, (unsigned)kVoiceRate);
  if (!hgSendVoiceJsonType("audio.start", extra)) return;

  if (!M5.Mic.begin()) {
    Serial.println("[voice] mic.begin FAILED");
    hgSendVoiceJsonType("audio.cancel", nullptr);
    return;
  }
  V.micOpen = true;
  V.micByButton = byButton;
  Serial.printf("[voice] listening (stream %u, %s)\n",
                (unsigned)V.uttStream, byButton ? "BtnB hold" : "serial toggle");
}

// PTT release: close the hub-side upload → hub assembles + STT.
static void voiceStopListening() {
  if (!V.micOpen) return;
  V.micOpen = false;
  M5.Mic.end();
  char extra[32];
  snprintf(extra, sizeof(extra), "\"stream\":%u", (unsigned)V.uttStream);
  hgSendVoiceJsonType("audio.end", extra);
  Serial.printf("[voice] utterance sent: %u B (%.2f s)%s\n",
                (unsigned)V.uttBytes, (float)V.uttBytes / (2.0f * kVoiceRate),
                V.uttTrunc ? " TRUNCATED" : "");
  V.uttStream = (V.uttStream % 250) + 1;  // next stream id (hub cycles 1..250)
}

// Every loop while micOpen: pull one 20 ms block from the mic and ship it.
static void voiceMicPump() {
  if (!V.micOpen) return;
  static int16_t micBlock[320];
  if (!M5.Mic.record(micBlock, 320, kVoiceRate)) {
    static uint32_t lastMicErrMs = 0;
    if (millis() - lastMicErrMs > 1000) {
      lastMicErrMs = millis();
      Serial.println("[voice] mic.record failed");
    }
    return;
  }
  if (V.uttBytes + kUttFrameBytes > kUttMaxBytes) {
    if (!V.uttTrunc) {
      V.uttTrunc = true;
      voiceStopListening();  // self-cap hit: close as a complete utterance
    }
    return;
  }
  uint8_t frame[4 + kUttFrameBytes];
  frame[0] = 0x01;                       // CHANNEL_AUDIO
  frame[1] = static_cast<uint8_t>(V.uttStream);
  frame[2] = static_cast<uint8_t>(V.uttSeq & 0xFF);
  frame[3] = static_cast<uint8_t>((V.uttSeq >> 8) & 0xFF);
  memcpy(frame + 4, micBlock, kUttFrameBytes);
  if (!VH.sendBin || VH.sendBin(frame, sizeof(frame)) != sizeof(frame)) {
    Serial.println("[voice] frame send failed — closing upload");
    V.micOpen = false;
    M5.Mic.end();
    return;
  }
  memcpy(V.uttBuf + V.uttBytes, micBlock, kUttFrameBytes);  // keep for local replay/log
  V.uttBytes += kUttFrameBytes;
  ++V.uttSeq;
}
