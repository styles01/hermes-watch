// SPDX-License-Identifier: AGPL-3.0-or-later
//
// hermes-watch.ino — Hermes Watch for the M5Stack StopWatch (v0.5: circular UI)
//
// Layer 2 live avatar (KK engine, verbatim) + hg.v1 WebSocket client.
// The Mac runs the Hermes gateway with the SDK's gadget platform plugin —
// it transcribes voice turns, drives the agent, synthesizes TTS, and streams
// reply.delta back. The watch = face + buttons. Text turns first (typed from
// the serial line, like the sim demo). v0.3 adds voice: BtnB-hold = PTT\n// (mic 16 kHz → hub STT), TTS replies play on the speaker; serial 'T' too.
//
// Buttons: A (left) = next screen (eyes -> agents -> settings) · B (right) =
//   per-screen action: eyes = 400 ms PTT hold / short-click tap-to-talk ·
//   agents = click: next candidate, hold 800 ms: commit · settings =
//   click: volume +0.5, hold 800 ms: volume -1.0 · both held 1 s = diagnostics.

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>

#include "avatar_engine.h"

#include "ws_raw.h"  // hand-rolled RFC6455 client (ArduinoWebsockets dropped: partial-read frame desync on core 3.3)
#include "hermes_voice.h"  // hg.v1 voice: PTT mic upload + TTS playback
#include "hermes_ota.h"   // v0.5.4: OTA over the hg.v1 socket (ch 0x03)

#include <mbedtls/md.h>      // HMAC-SHA256 (core, no extra lib)
#include <mbedtls/sha256.h>  // key -> device_id
#include "esp_ota_ops.h"  // v0.5.4: rollback confirm on welcome

#include <cstring>
#include <cstdio>
#include <cstdlib>

// ------------------------------------------------- hg.v1 configuration ----
// Gateway host: the Mac that runs both the Hermes desktop app + the gadget
// plugin (hub listens on :8765/gadget). v0.1: read the meter's provisioned
// companion host from NVS (namespace "meter", key "host") — same machine —
// and override via serial 'H <host>[:port]'.
static char s_wsHost[48] = "";
static uint16_t s_wsPort = 8765;
static const char *kWsPath = "/gadget";

// Device identity: hg-<hex(sha256(key))[0:16]>, key 32B, NVS-persisted.
static uint8_t s_deviceKey[32];
static char s_deviceId[24];  // "hg-" + 16 hex + NUL
static char s_wifiSsid[33] = "";
static char s_wifiPass[65] = "";

// TOFU enrollment state: on first connect the server reports enrolled=false;
// we send the raw key once, then always HMAC afterwards.
static bool s_enrolled = false;         // last-known server truth

// OTA image identity tags (v0.5.4): read back by the gateway's ota.inspect_image — app_desc of an
// Arduino build reads 'arduino-lib-builder', so these rodata strings are the trusted markers.
// 'used' keeps the linker from dead-stripping them even though nothing here reads them.
__attribute__((used, section(".rodata")))
static const char HG_BOARD_TAG[] = "HGBOARD=m5stack-stopwatch";
__attribute__((used, section(".rodata")))
static const char HG_VER_TAG[] = "HGVER=0.5.16";
static bool s_connected = false;
static bool s_paired = false;
static bool s_turnActive = false;       // reply streaming in progress
static bool s_hasLiveStatus = false;
static bool s_deltaLive = false;   // delta strip currently painted

// Avatar state mapping (DESIGN-LOCKED Layer 2):
//   listening  = mic open / PTT held          -> ExpressionId::Listening
//   thinking   = turn.start / status          -> ExpressionId::Thinking
//   speaking   = TTS audio playing            -> (v0.1: reply streaming) Happy
//   idle       = nothing in flight            -> ExpressionId::Idle
static bool s_pttStartSerial = false;    // serial 'T' one-shot
static bool s_pttStopSerial = false;
static uint32_t s_lastServerPollMs = 0;
static uint32_t s_reconnectAtMs = 0;

WsRawClient s_ws;

namespace {

// Circular panel: 466x466, active circle center 233 / r 233 (corners dead).
// Declared early: uiText's default-x default argument needs it above UI glue.
static constexpr int16_t kCircleC = 233;  // circle center (x = y)
static constexpr int16_t kCircleR = 233;  // active radius

constexpr uint32_t kHeartbeatIntervalMs = 10000;
constexpr uint32_t kReconnectBaseMs = 2000;     // backoff base
constexpr uint32_t kReconnectMaxMs = 30000;     // backoff cap
constexpr uint32_t kTurnStallMs = 180000;       // v0.5.12: no hub frames for 3 min = stalled turn
// v0.5.16: orphaned-turn fast path — turn active but no audio ever began and the hub
// went silent (connection replaced mid-turn): clear after 20 s, not 3 min.
constexpr uint32_t kTurnStallNoAudioMs = 20000;
constexpr uint32_t kAvatarIdleReturnMs = 0;     // KK engine handles its own idle
constexpr size_t kSendBufferCap = 1400;         // stay under a WS frame

AvatarEngine avatar;
bool s_diagMode = false;
bool s_diagLatched = false;
bool s_diagPainted = false;  // H3: diag view currently on the panel

// ------------------------------------------------------------- helpers ----

void sendText(const String &line) {
  if (!s_connected) return;
  const size_t n = s_ws.sendText(line.c_str());
  if (n == 0) {
    s_connected = false;
    s_paired = false;
    s_reconnectAtMs = millis() + kReconnectBaseMs;
    Serial.println("[hg] send failed — socket dead");
    avatar.show(ExpressionId::Sleepy, millis(), false);
  }
}

void sendJson(const char *type, const char *extraJson = nullptr) {
  char buf[192];
  if (extraJson && extraJson[0]) {
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",%s}", type, extraJson);
  } else {
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\"}", type);
  }
  sendText(buf);
}

// ------------------------------------------------------- hg.v1 identity ----

void ensureDeviceKey() {
  // NVS 'hg-v1' blob: 32B device key. First boot = generate via esp_random.
  Preferences prefs;
  prefs.begin("hg-v1", false);
  size_t got = prefs.getBytesLength("seed");
  if (got != sizeof(s_deviceKey)) {
    for (auto &b : s_deviceKey) b = static_cast<uint8_t>(esp_random());
    prefs.putBytes("seed", s_deviceKey, sizeof(s_deviceKey));
  } else {
    prefs.getBytes("seed", s_deviceKey, sizeof(s_deviceKey));
  }
  prefs.end();

  // device_id = "hg-" + hex(sha256(key))[0:16]
  uint8_t digest[32];
  mbedtls_sha256(s_deviceKey, sizeof(s_deviceKey), digest, 0);
  static const char *kHex = "0123456789abcdef";
  s_deviceId[0] = 'h'; s_deviceId[1] = 'g'; s_deviceId[2] = '-';
  for (int i = 0; i < 8; ++i) {
    s_deviceId[3 + i * 2] = kHex[digest[i] >> 4];
    s_deviceId[4 + i * 2] = kHex[digest[i] & 0xF];
  }
  s_deviceId[19] = '\0';
  Serial.printf("[hg] device_id=%s\n", s_deviceId);
}

// meter's provisioner namespace ("meter"): ssid/pass/host — same flash, so
// the watch inherits the meter's WiFi + the Mac's LAN IP without any setup.
bool loadMeterProvisioning() {
  Preferences prefs;
  if (!prefs.begin("meter", true)) return false;
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  String h = prefs.getString("host", "");
  prefs.end();
  if (s.length() == 0) return false;
  strlcpy(s_wifiSsid, s.c_str(), sizeof(s_wifiSsid));
  strlcpy(s_wifiPass, p.c_str(), sizeof(s_wifiPass));
  // the meter's "auto" host token = mDNS auto-resolve keyword; not a host.
  if (h.length() && h != "auto") strlcpy(s_wsHost, h.c_str(), sizeof(s_wsHost));
  return true;
}

String b64(const uint8_t *data, size_t len) {
  unsigned char tmp[256];
  size_t olen = 0;
  if (mbedtls_base64_encode(tmp, sizeof(tmp), &olen, data, len) != 0) return "";
  tmp[olen] = 0;
  return String((const char *)tmp);
}

// base64 -> raw bytes (challenge nonce = 16B). mbedtls needs no padding math.
bool b64decode(const char *in, uint8_t *out, size_t outCap, size_t &outLen) {
  size_t olen = 0;
  if (mbedtls_base64_decode(out, outCap, &olen,
                            (const uint8_t *)in, strlen(in)) != 0) return false;
  outLen = olen;
  return true;
}

void sendHello() {
  char caps[384];
  snprintf(caps, sizeof(caps),
           "\"name\":\"Hermes Watch\",\"board\":\"m5stack-stopwatch\","
           "\"firmware\":\"%s\",\"caps\":{"
           "\"display\":{\"width\":466,\"height\":466,\"color\":true,"
           "\"charset\":\"ascii\",\"text_cols\":30,\"text_rows\":16},"
           "\"mic\":{\"rate\":16000,\"format\":\"pcm16\"},"
           "\"speaker\":{\"rate\":16000,\"format\":\"pcm16\"},"
           "\"inputs\":[\"talk\",\"cancel\",\"up\",\"down\"],"
           "\"talk_mode\":\"hold\",\"ota\":{\"max_size\":3145200}}",
           HG_VER_TAG + 6);
  // NOTE: caps payload is under kSendBufferCap — single frame is fine.
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"type\":\"hello\",\"proto\":1,\"device_id\":\"%s\",%s}",
           s_deviceId, caps);
  sendText(buf);
}

void sendAuthEnroll() {
  // {"type":"auth","key":"<b64 32B>"} — first contact only
  char buf[160];
  String kb = b64(s_deviceKey, sizeof(s_deviceKey));
  snprintf(buf, sizeof(buf), "{\"type\":\"auth\",\"key\":\"%s\"}", kb.c_str());
  sendText(buf);
}

void sendAuthHmac(const String &nonceB64) {
  // mac = HMAC-SHA256(key, "hermes-gadget/v1|<device_id>|<nonce>") —
  // the hub HMACs the nonce AS THE BASE64 STRING IT SENT (protocol.py
  // auth_mac: msg = AUTH_CONTEXT + device_id + "|" + nonce.encode()).
  // No b64 decoding here — v0.1.1 bug: raw-decoding it broke enrollment auth.
  const char *kCtx = "hermes-gadget/v1|";
  uint8_t mac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, s_deviceKey, sizeof(s_deviceKey));
  mbedtls_md_hmac_update(&ctx, (const uint8_t *)kCtx, strlen(kCtx));
  mbedtls_md_hmac_update(&ctx, (const uint8_t *)s_deviceId, strlen(s_deviceId));
  mbedtls_md_hmac_update(&ctx, (const uint8_t *)"|", 1);
  mbedtls_md_hmac_update(&ctx, (const uint8_t *)nonceB64.c_str(), nonceB64.length());
  mbedtls_md_hmac_finish(&ctx, mac);
  mbedtls_md_free(&ctx);

  String mb = b64(mac, sizeof(mac));
  char buf[240];
  snprintf(buf, sizeof(buf), "{\"type\":\"auth\",\"mac\":\"%s\"}", mb.c_str());
  sendText(buf);
}

// ------------------------------------------------------------ UI glue ----

// ---- type system (v0.5.1): big base fonts at scale 1 -----------------------
// v0.5 shipped ONE font: drawStatusLine set FreeSans9pt7b and every screen
// only changed setTextSize -- so agent names were a 9pt outline bitmap blown
// up 5x with nearest-neighbour scaling (chunky jaggies; width grew but glyph
// weight stayed hairline). The fix: each role in the hierarchy picks a TRUE
// base size instead of upscaling (scale stays 1 everywhere):
//   display  = FreeSansBold24pt7b  (agent names, volume value)
//   title    = FreeSansBold12pt7b  (screen titles, diag title)
//   role     = FreeSans18pt7b      (agent roles -- regular weight, bold 18
//                                   measures 470px for 'Product & Market
//                                   Research' and clips the 462px chord)
//   meta     = FreeSans9pt7b       (hints, stats, diag rows)
// Every y below was measured against the real glyph tables + the 466px panel
// circle (center 233, r=233; strings checked at r=232 conservative): width vs
// chord at each ink row, per-glyph xAdvance/yOffset summed from
// M5GFX Fonts/GFXFF/*.h -- method + full audit in
// the fontmeasure audit pass (local tooling).
// NOTE: GFXFF Free fonts cover 0x20..0x7E only -- no U+00B7 middle dot;
// separators stay ASCII '-'.
static void uiText(const char *txt, int16_t y, const lgfx::v1::IFont *font,
                   uint32_t fg, uint32_t bg,
                   lgfx::v1::textdatum_t datum = lgfx::v1::textdatum_t::top_center,
                   int16_t x = kCircleC) {
  auto &d = M5.Display;
  d.setTextColor(fg, bg);
  d.setTextSize(1);
  d.setTextDatum(datum);
  d.setTextFont(font);
  d.drawString(txt, x, y);
}

// v0.5.3: reply.delta live strip — last ~26 chars of the streaming reply on
// the y=368 band (chord-safe rows 369..384, half-chord ~177 px; 26 chars
// of FreeSans9pt7b ≈ 110 px). Cleared on
// reply/turn.end. Never drawn over the commit toast (gated below).
static char s_deltaTail[27];
static int  s_deltaLen;

static void deltaClear(void) { s_deltaLen = 0; s_deltaTail[0] = 0; }

static void deltaPushC(uint8_t c) {
  if (s_deltaLen < 26) s_deltaTail[s_deltaLen++] = (char)c;
  else {
    memmove(s_deltaTail, s_deltaTail + 1, 25);
    s_deltaTail[25] = (char)c;
    s_deltaLen = 26;
  }
  s_deltaTail[s_deltaLen] = 0;
}

static void deltaDraw(void) {
  if (M5.Display.getRotation() < 0) return;   // never before display init
  uiText(s_deltaLen ? s_deltaTail : "", 368, &fonts::FreeSans9pt7b,
         0xFFFFFFu, TFT_BLACK);
}


// ---- v0.5.9 idle-face memory: tap cycles the three persistent expressions ----
// The three persistent faces (Idle/LISTENING/THINKING) are true DIFFERENT eye
// shapes in the KK catalog (capsule / angled+tilted / browed). show() on a
// persistent face writes the engine's baseExpression_, so one-shot reactions
// (Happy on audio.start, Confused on notices) still return to the chosen face.
// A clean idle-screen tap cycles it (see handleProductTouch); NVS-persisted.
static ExpressionId s_idleFace = ExpressionId::Idle;  // tap-cycled base face (turn.end maps to it)
static void faceHold(ExpressionId expr);  // fwd decl (v0.5.11: defined with the face cycle)
static void voiceReturnToIdle();          // v0.5.14: idle restore for watchdog paths
static void eraseStatusLine();            // v0.5.14: clear the y=40 status band

ExpressionId nextIdleFace(ExpressionId cur) {
  return cur == ExpressionId::Idle       ? ExpressionId::Listening :
         cur == ExpressionId::Listening  ? ExpressionId::Thinking :
                                           ExpressionId::Idle;
}

void setStateFromServer(const char *kind) {
  // Map an hg.v1 event to a KK expression (Layer 2 mapping).
  if (!strcmp(kind, "turn.start"))       avatar.show(ExpressionId::Thinking, millis(), false);
  else if (!strcmp(kind, "reply.delta")) avatar.show(ExpressionId::Listening, millis(), false);
  else if (!strcmp(kind, "turn.end"))    faceHold(s_idleFace);  // v0.5.11: hold any face
}

void drawStatusLine(const char *txt) {
  // L2: circle-safe state banner — text only, no fillRect bleed into the
  // dead corners. v0.5.1: TRUE FreeSans9pt7b base font at scale 1 (was 9pt +
  // sizes 2/4 from callers); y=40 keeps the band inside the top chord (rows
  // 41..54 need half-width 79 <= chord 197 there; at y=6 the chord is only
  // ~48 and 'hermes-watch 0.5.0' (158px) would clip). Datum top_center so
  // short tokens (PAIR?) sit centered like every other row.
  uiText(txt, 40, &fonts::FreeSans9pt7b, TFT_ORANGE, TFT_BLACK);
}

// v0.5.14: erase the y=40 status band (FreeSans9pt7b, ~18 px at scale 1).
static void eraseStatusLine() {
  M5.Display.fillRect(20, 38, 440, 18, TFT_BLACK);
}

// -------------------------------------------------- serial control line ----
// Commands (same grammar as the sim):
//   <expression>        KK engine expression (idle/listening/thinking/…)
//   say <text>          start an hg.v1 text turn to the host
//   H <host>[:port]     set gateway host (RAM only, v0.1)
//   state               dump hg.v1 state

String s_serialCmd;
static const char *committedAgentTag();  // fwd decl (defined with the profile table below)

void handleSerialLine(const String &line) {
  if (line.length() >= 4 && line.startsWith("say ")) {
    String text = line.substring(4);
    char id[12];
    snprintf(id, sizeof(id), "t%lu", (unsigned long)millis());
    String payload = "{\"type\":\"text\",\"id\":\"" + String(id) +
                     "\",\"text\":\"" + text + "\",\"agent\":\"" +
                     committedAgentTag() + "\"}";
    sendText(payload);
    avatar.show(ExpressionId::Listening, millis(), false);
    return;
  }
  if (line.startsWith("H ")) {
    // H <host>[:port] — persists to NVS (namespace hermes-watch) so reboots reconnect without a serial command.
    String spec = line.substring(2);
    spec.trim();
    int colon = spec.indexOf(':');
    if (colon > 0) {
      strlcpy(s_wsHost, spec.c_str(), colon < (int)sizeof(s_wsHost) ? colon + 1 : sizeof(s_wsHost));
      s_wsPort = (uint16_t)spec.substring(colon + 1).toInt();
    } else {
      strlcpy(s_wsHost, spec.c_str(), sizeof(s_wsHost));
      s_wsPort = 8765;
    }
    s_ws.close();
    s_connected = false;
    s_reconnectAtMs = millis() + 500;
    // persist (only the host; the meter's SSID/pass live elsewhere)
    Preferences prefs;
    if (prefs.begin("hermes-watch", false)) {
      prefs.putString("host", String(s_wsHost) + ":" + String(s_wsPort));
      prefs.end();
      Serial.printf("[hg] host set + saved: %s:%u\n", s_wsHost, s_wsPort);
    } else {
      Serial.printf("[hg] host set (NVS save failed): %s:%u\n", s_wsHost, s_wsPort);
    }
    return;
  }
  if (line == "state") {
    Serial.printf("[hg] connected=%d paired=%d enrolled=%d turnActive=%d host=%s:%u id=%s\n",
                  (int)s_connected, (int)s_paired, (int)s_enrolled,
                  (int)s_turnActive, s_wsHost, s_wsPort, s_deviceId);
    Serial.printf("[voice] mic=%d playStream=%u playBytes=%u uttBytes=%u\n",
                  (int)V.micOpen, (unsigned)V.playStream,
                  (unsigned)V.playBytes, (unsigned)V.uttBytes);
    return;
  }
  if (line == "T") {  // programmatic PTT: toggle listening (test hook, same as BtnB-hold)
    if (!V.micOpen) { s_pttStartSerial = true; Serial.println("[voice] PTT ON (serial)"); }
    else            { s_pttStopSerial = true;  Serial.println("[voice] PTT OFF (serial)"); }
    return;
  }
  if (avatar.showFromCommand(line, millis())) {
    Serial.printf("Command accepted: %s\n", avatar.activeName());
    return;
  }
  Serial.printf("Unknown command: %s\n", line.c_str());
}

void handleSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (s_serialCmd.length()) handleSerialLine(s_serialCmd);
      s_serialCmd = "";
    } else if (s_serialCmd.length() < 160) {
      s_serialCmd += c;
    }
  }
}

// ------------------------------------------------------- avatar glue ----

// ---- v0.5: four-screen circular UI ----------------------------------------
// Left (BtnA) cycles eyes -> agents -> settings -> commands -> eyes; right
// (BtnB) acts per screen (PTT hold / tap-to-talk · agent commit · volume ·
// quick-command). The avatar screen redraws only its dirty rects at 60 fps;
// the other three screens are static and repaint only on entry/state change.

enum ScreenId : uint8_t { SCR_EYES = 0, SCR_AGENTS = 1, SCR_SETTINGS = 2, SCR_CMDS = 3 };
static constexpr uint8_t kScreenCount = SCR_CMDS + 1;
static ScreenId s_screen = SCR_EYES;
static ScreenId s_rebuildScreenId = SCR_EYES;  // pending static-screen paint
static uint32_t s_lastNavMs = 0;               // nav/action debounce (150 ms)

// Hold-latch state is FILE-scope (H2): gotoScreen must clear latches when a
// hold crosses a screen boundary (commit jumps to EYES mid-hold; the old
// screen-local release handler never runs, leaking the latch).
static bool pttFromHold = false;
static bool agentHoldLatched = false;
static bool setHoldLatched = false;
static bool cmdHoldLatched = false;
static uint32_t btnBHoldEndMs = 0;  // a release ending a hold never also
                                    // counts as a click (>100 ms guard)
static ScreenId s_btnBPressScreen = SCR_EYES;  // screen the press STARTED on
static uint32_t s_btnBSuppressMs = 0;          // block PTT re-arm after a
                                               // hold crossed a screen exit
static uint32_t s_toastUntilMs = 0;            // commit-toast window (H1)
static bool s_toastShown = false;

// ---- v0.5.11 idle-face memory: tap cycles ALL TWELVE expressions ----
// The three persistent faces (Idle/LISTENING/THINKING = capsule / angled+tilted /
// browed) loop natively; the nine REACTION expressions hold via forced Loop
// playback (a few hold on their last frame and don't blink — that's the look).
// v0.5.9 behavior stays for the first three indices, so the persisted NVS face
// value 0..2 means exactly what it always did. The engine's base face follows
// the held expression (setBaseFace), so one-shot reactions return to it.
static const char *kFaceLabel[12] = {
    "FACE CAPSULE", "FACE ANGLED", "FACE BROWED", "FACE HAPPY",
    "FACE EXCITED", "FACE CURIOUS", "FACE CONFUSED", "FACE ANGRY",
    "FACE SURPRISED", "FACE SAD", "FACE SLEEPY", "FACE DIZZY",
};
static uint8_t idleFaceIdx() {
  return static_cast<uint8_t>(static_cast<uint8_t>(s_idleFace) % 12u);
}
static ExpressionId faceFromIdx(uint8_t i) {
  return static_cast<ExpressionId>(i % 12u);
}
// Hold `expr` as the resting face: loop it; keep the engine base in sync so
// reactions (Happy on audio.start, Confused on notices) return to it.
static void faceHold(ExpressionId expr) {
  s_idleFace = expr;
  avatar.setBaseFace(expr);
  if (avatar.isPersistentFace(expr)) avatar.show(expr, millis(), false);
  else avatar.play(expr, millis(), AvatarEngine::PlaybackMode::Loop, false);
  drawStatusLine(kFaceLabel[idleFaceIdx()]);
}
enum RingState : uint8_t {
  RING_IDLE = 0,   // v0.5.9: NO ring — band erased to black once on entering idle
  RING_PULSE,      // LISTENING: bright pulsing ring
  RING_THINKING,   // dim ring + rotating 70 deg segment
  RING_SPEAKING,   // TTS playing: slow breathing ring
};
static constexpr uint8_t kRingRedrawAll = 0xFF;  // force a ring repaint
static uint8_t s_ringState = kRingRedrawAll;     // last ring drawn
static uint8_t s_ringBucket = 0xFF;              // brightness/rotation step
static uint32_t s_lastRingMs = 0;                // >=100 ms redraw guard
static uint32_t s_clearStatusAtMs = 0;           // v0.5.14: idle status-band erase timer

// v0.5.14: return the eyes screen to TRUE idle — black disc ring band, no stale
// arc, no lingering status text (user: idle = "fully black, without the ring or the
// 'playback stopped' text"). Called from every voice watchdog + playback-done path.
static void voiceReturnToIdle() {
  faceHold(s_idleFace);            // re-commit resting face (draws its label once)
  s_clearStatusAtMs = millis() + 1200;   // then erase the band → idle is black
  s_ringState = kRingRedrawAll;    // idle entry erases the annulus arc (fix below)
}
static void cycleIdleFace() {
  faceHold(faceFromIdx((uint8_t)((idleFaceIdx() + 1u) % 12u)));
  Preferences prefs;
  if (prefs.begin("hermes-watch", false)) {
    prefs.putUChar("face", idleFaceIdx());
    prefs.end();
  }
  Serial.printf("[ui] idle face: %s\n", avatar.activeName());
}

// v0.5.11: hub-driven faces — incoming {"type":"action","name":"avatar_face",
// "args":{"cmd":"loop sleepy"|"happy"|...}} → engine command parser. The gadget
// action tool already sends action frames; the device answers action.result.{id,ok}.
static void handleFaceAction(const char *buf) {
  // id (echo back), name (only avatar_face accepted), args.cmd (expression command)
  const char *idK = strstr(buf, "\"id\":\"");
  char idS[24] = {};
  if (idK) {
    idK += 6;
    const char *end = strchr(idK, '"');
    if (end && (end - idK) < (int)sizeof(idS)) strlcpy(idS, idK, end - idK + 1);
  }
  const char *cmdK = strstr(buf, "\"cmd\":\"");
  char cmd[48] = {};
  bool ok = false;
  if (cmdK) {
    cmdK += 7;
    const char *end = strchr(cmdK, '"');
    if (end && (end - cmdK) < (int)sizeof(cmd)) {
      strlcpy(cmd, cmdK, end - cmdK + 1);
      String c = cmd;
      c.trim();
      c.toLowerCase();
      String looped = String("loop ") + c;   // bare name = loop it as resting face
      ok = avatar.showFromCommand(looped.c_str(), millis());
      if (ok) {
        ExpressionId held = avatar.activeExpression();
        s_idleFace = held;
        avatar.setBaseFace(held);
        if (idleFaceIdx() < 12) {  // persist the hub-chosen resting face too
          Preferences prefs;
          if (prefs.begin("hermes-watch", false)) {
            prefs.putUChar("face", idleFaceIdx());
            prefs.end();
          }
        }
        drawStatusLine(idleFaceIdx() < 12 ? kFaceLabel[idleFaceIdx()] : "FACE SET");
      }
    }
  }
  char res[80];
  snprintf(res, sizeof(res), "{\"type\":\"action.result\",\"id\":\"%s\",\"ok\":%s}",
           idS, ok ? "true" : "false");
  sendText(String(res));
}

void handleProductTouch(uint32_t nowMs) {
  if (!M5.Touch.isEnabled()) return;
  const auto t = M5.Touch.getDetail(0);
  // v0.5.2 tap-to-stop: a tap while the watch is busy aborts the in-flight
  // speech immediately (local stop + hub 'cancel' → audio.abort + /stop turn).
  // Reasoning states: TTS playing, server turn active, or mic open. The avatar
  // poke (setTouchTarget) only fires on IDLE taps so play never masks petting.
  if (t.wasPressed()) {
    const bool busy =
        V.playStream != 0 || s_turnActive || V.micOpen;
    if (busy) {
      Serial.println("[ui] tap-to-stop speech");
      if (V.micOpen) voiceStopListening();       // close + send audio.end
      else            M5.Speaker.stop();         // instant local mute
      voicePlaybackReset();                      // drop buffered TTS frames
      avatar.show(ExpressionId::Idle, millis(), false);
      sendText(String("{\"type\":\"cancel\"}")); // hub: abort stream + /stop
      return;                                    // tap consumed
    }
    avatar.setTouchTarget(t.x, t.y);
  }
  if (t.isPressed() && avatar.ready() && V.playStream == 0 && !s_turnActive) {
    avatar.setTouchTarget(t.x, t.y);
  }
  if (t.wasReleased()) {
    avatar.releaseTouch();
    // v0.5.9: a clean tap while FULLY idle cycles the persistent base face
    // (capsule -> angled -> browed). Idle-only so tap-to-stop, PTT and cancel
    // are untouched; single-click keeps double-tap free for card activation;
    // suppressed during toasts / diagnostics like petting above.
    if (t.wasClicked() && t.getClickCount() == 1 && s_screen == SCR_EYES &&
        !s_diagMode && !s_toastShown && !s_turnActive && V.playStream == 0 &&
        !V.micOpen) {
      cycleIdleFace();
    }
  }
}

void handleImuInteraction(uint32_t nowMs) {
  constexpr uint32_t kImuInterval = 20;
  static uint32_t lastImuMs = 0;
  if (nowMs - lastImuMs < kImuInterval) return;
  lastImuMs = nowMs;
  if (!M5.Imu.update()) return;
  const auto imu = M5.Imu.getImuData();
  const float kNorm = 1.2f;
  float nx = imu.accel.y / kNorm;   // same axes as KK device main
  float ny = imu.accel.x / kNorm;
  nx = nx < -1.f ? -1.f : nx > 1.f ? 1.f : nx;
  ny = ny < -1.f ? -1.f : ny > 1.f ? 1.f : ny;
  avatar.setTiltTarget(nx, ny);
}

// (kCircleC/kCircleR declared at the top of this namespace — uiText's
// default-argument needs them above the UI glue.)

struct AgentProfile {
  const char *name;
  const char *role;
  uint32_t color;  // 0xRRGGBB; passed straight to the gfx (565-converted)
};
// Order locked to the sim's profile_ring.h kProfileNames. Names are deliberately
// generic here (AGENT 1..5); rename to your own agent identities at this one table
// (and mirror in sim/include/profile_ring.h).
static const AgentProfile kProfiles[] = {
  {"MAIS",   "",                          0xFFB020u},
  {"ORACLE", "Foresight Strategist",      0x9D6BFFu},
  {"LARA",   "Project Engineer",          0x2DD4BFu},
  {"LOCA",   "Local Model Agent",         0x4ADE80u},
  {"VENUS",  "Product Research",          0xFF5C8Au},
};
static const size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);
static size_t s_profileIdx = 4;  // committed agent (default AGENT 5)
static size_t s_agentSel = 4;    // agents-screen highlight cursor
static float s_volumePct = 6.0f;  // 0..10 (persisted in NVS 'hermes-watch'); 8.5 was too loud for voice replies (2026-10-08)
static bool s_volumeDirty = false;  // volume stepped; flush NVS on leaving SETTINGS (M2)

// Profile colors arrive as 24-bit hex — scale in 8-bit space (the gfx
// converts to rgb565 on the way out; dimming rgb565 values would be lossy).
static uint32_t scaleColor24(uint32_t col, float k) {
  if (k < 0.0f) k = 0.0f;
  if (k > 1.0f) k = 1.0f;
  const uint32_t r = (uint32_t)(((float)((col >> 16) & 0xFF)) * k + 0.5f);
  const uint32_t g = (uint32_t)(((float)((col >> 8) & 0xFF)) * k + 0.5f);
  const uint32_t b = (uint32_t)(((float)(col & 0xFF)) * k + 0.5f);
  return (r << 16) | (g << 8) | b;
}

static const AgentProfile &committedProfile() { return kProfiles[s_profileIdx]; }

// v0.5.10: the committed agent rides EVERY turn so the hub can route it to that
// profile (agent_profiles in the gateway config). Lowercase = the map key form.
static const char *committedAgentTag() {
  static char tag[12];
  const char *n = kProfiles[s_profileIdx].name;
  size_t i = 0;
  for (; n[i] && i < sizeof(tag) - 1; ++i)
    tag[i] = (n[i] >= 'A' && n[i] <= 'Z') ? char(n[i] - 'A' + 'a') : n[i];
  tag[i] = 0;
  return tag;
}

// ---- volume ----------------------------------------------------------------
static int volumeLevel255() {  // 10-step user volume -> M5Unified 0..255
  const int lvl = (int)(s_volumePct * 25.5f + 0.5f);
  return lvl < 0 ? 0 : (lvl > 255 ? 255 : lvl);
}

// M2: setVolume applies LIVE per step; the NVS write happens ONCE when the
// volume actually changed and we leave SETTINGS (saveVolumeIfDirty → from
// gotoScreen). A flash write per 0.5-step click was needless write amp.
static void applyVolume() { M5.Speaker.setVolume(volumeLevel255()); }

static void saveVolumeIfDirty() {
  if (!s_volumeDirty) return;
  s_volumeDirty = false;
  applyVolume();
  Preferences prefs;
  if (prefs.begin("hermes-watch", false)) {
    prefs.putFloat("vol", s_volumePct);
    prefs.end();
  }
}

// ---- eyes-screen circular status ring --------------------------------------
static const char *turnStateName() {
  if (V.micOpen) return "LISTENING";
  if (s_turnActive) return "THINKING";
  return "IDLE";
}


// Ring annulus near the display edge (spec's 436..456 exceeds the ~233 px
// active radius; 210..228 gives the same near-edge look, circle-safe).
static void drawRingFor(const char *stateName, uint32_t nowMs) {
  if (s_screen != SCR_EYES) {
    s_ringState = kRingRedrawAll;  // ring reappears when eyes return
    return;
  }
  uint8_t state;
  if (!strcmp(stateName, "LISTENING"))     state = RING_PULSE;
  else if (!strcmp(stateName, "THINKING")) state = RING_THINKING;
  else if (V.playStream)                   state = RING_SPEAKING;
  else                                     state = RING_IDLE;

  uint8_t bucket = 0;
  if (state == RING_PULSE)         bucket = (uint8_t)((nowMs / 100) % 10);
  else if (state == RING_THINKING) bucket = (uint8_t)((nowMs / 100) % 10);
  else if (state == RING_SPEAKING) bucket = (uint8_t)(nowMs / 100);

  const bool force = (s_ringState == kRingRedrawAll);
  if (!force) {
    if (state != s_ringState) {
      // state change: repaint immediately
    } else if (nowMs - s_lastRingMs < 100) {
      return;  // hard gate: ring never redraws more often than 100 ms
    } else if (bucket == s_ringBucket) {
      return;  // same bucket: nothing to do
    }
  }
  s_lastRingMs = nowMs;
  s_ringState = state;
  s_ringBucket = bucket;

  auto &d = M5.Display;
  // v0.5.3: state colors are STATE-coded, not profile-coded (a single
  // "red ring that flashes" told the user nothing). v0.5.9: IDLE draws NOTHING
  // (black band erase) — LISTENING green / THINKING amber / SPEAKING blue stay
  // constant so a state change is UNMISSABLE. Labels under the disc carry the
  // word, too.
  const uint32_t base =
      state == RING_PULSE     ? 0x00C853u :   // listening  = green
      state == RING_THINKING  ? 0xFFB300u :   // thinking   = amber
      state == RING_SPEAKING  ? 0x1E88E5u :   // speaking   = blue
      committedProfile().color;               // idle       = profile color
  // State word at y=340 (rows 341..354, min half-chord 199 px; worst label
  // 'LISTENING' ~90 px): paint once per state change, erase with a black
  // band when leaving a state. Never drawn while a toast owns the screen.
  static uint8_t s_labelShown = 0xFF;  // 0xFF = unknown (no label up yet)
  auto drawLabel = [&](const char *txt, uint32_t col) {
    uiText(txt, 340, &fonts::FreeSans9pt7b, col, TFT_BLACK);
    s_labelShown = state;
  };
  if (state == RING_IDLE) {
    if (s_labelShown != RING_IDLE) {
      d.fillRect(93, 339, 280, 17, TFT_BLACK);  // chord at y~356: half 197 px
      // v0.5.14: erase the arc too — SPEAKING/THINKING drew 205..230; leaving it
      // strayed a stale blue/amber ring on screen ("blue ring hang", James).
      d.drawArc(kCircleC, kCircleC, 203, 232, 0, 360, TFT_BLACK);
      d.fillRect(20, 38, 440, 18, TFT_BLACK);   // status band (y=40 text) clear
      s_labelShown = RING_IDLE;
    }
  } else if (!s_toastShown && (s_labelShown != state || force)) {
    // toast branch: skip the label — the band (dist 106..123 from center)
    // sits inside the toast disc (r140) and would collide with its art.
    d.fillRect(93, 339, 280, 17, TFT_BLACK);
    drawLabel(state == RING_PULSE     ? "PUSH-TO-TALK" :
              state == RING_THINKING  ? "THINKING" :
                                        "PLAYING", base);
  }

  switch (state) {
    case RING_PULSE: {
      // 10 buckets over 1 s; brightness 0.30..1.00, quantized per bucket
      const float k = 0.30f + 0.35f * (1.0f + sinf((float)s_ringBucket * 0.6283f));
      d.drawArc(kCircleC, kCircleC, 210, 228, 0, 360, scaleColor24(base, k));
      break;
    }
    case RING_THINKING: {
      // repaint the band each bucket: dim ring + rotating 70 deg segment
      d.drawArc(kCircleC, kCircleC, 205, 230, 0, 360, TFT_BLACK);
      d.drawArc(kCircleC, kCircleC, 210, 228, 0, 360, scaleColor24(base, 0.35f));
      const float rot = (float)s_ringBucket * 7.2f;
      d.drawArc(kCircleC, kCircleC, 210, 228, rot, rot + 70.0f, base);
      break;
    }
    case RING_SPEAKING: {
      // ~1 Hz breathing (bucket-quantized at 100 ms)
      const float k = 0.45f + 0.275f *
          (1.0f + sinf((float)(nowMs % 1000) * 0.0062832f));
      d.drawArc(kCircleC, kCircleC, 210, 228, 0, 360, scaleColor24(base, k));
      break;
    }
    default: {  // RING_IDLE
      // v0.5.9: fully idle = NO ring; the disc area returns to plain black.
      // One-time erase of the 205..230 band (same rect THINKING clears at
      // :610). Bucket stays 0 for idle, so after this paint the early-return
      // above skips redraws entirely; the avatar engine's dirty-rects never
      // touch this band, so it stays black until an active state repaints it.
      d.drawArc(kCircleC, kCircleC, 205, 230, 0, 360, TFT_BLACK);
      break;
    }
  }
}

// ---- commit toast (1.2 s on the eyes screen, circle-safe) ------------------
static void drawCommitToast() {
  auto &d = M5.Display;
  const AgentProfile &p = committedProfile();
  d.fillCircle(kCircleC, kCircleC, 140, 0x0A0A12u);
  d.drawCircle(kCircleC, kCircleC, 160, p.color);
  // v0.5.1: display role, TRUE FreeSansBold24pt7b scale 1 (was 9pt scale5):
  // crisp glyph outlines instead of 5x nearest-neighbour upscale inside the
  // r140 disc. 'AGENT 2' (worst) = 198px wide, ink rows 177..213, needs
  // half-width 99 vs chord 225..231 there (r140-disc chord 128..138 too):
  // fits with >126px/side.
  uiText(p.name, 176, &fonts::FreeSansBold24pt7b, p.color, 0x0A0A12u);
  uiText("COMMITTED", 262, &fonts::FreeSans9pt7b, 0x9AA3ADu, 0x0A0A12u);
}

// ---- SCR_AGENTS: static screen (painted on entry / change only) ------------
static void drawAgentsScreen() {
  auto &d = M5.Display;
  d.fillCircle(kCircleC, kCircleC, kCircleR, TFT_BLACK);

  // v0.5.1 title: FreeSansBold12pt7b scale 1 (was 9pt scale2). 'AGENTS'
  // = 100px, ink rows 88..106, chord >= 231 there: fits with 131px/side.
  uiText("AGENTS", 88, &fonts::FreeSansBold12pt7b, TFT_WHITE, TFT_BLACK);

  // 5 dots, -90 deg at top then 72 deg apart; selected = full color + bigger
  for (size_t i = 0; i < kProfileCount; ++i) {
    const float a = (-90.0f + 72.0f * (float)i) * 0.0174533f;
    const int16_t x = kCircleC + (int16_t)lroundf(175.0f * cosf(a));
    const int16_t y = kCircleC + (int16_t)lroundf(175.0f * sinf(a));
    const bool sel = (i == s_agentSel);
    d.fillCircle(x, y, sel ? 16 : 11,
                 sel ? kProfiles[i].color
                     : scaleColor24(kProfiles[i].color, 0.4f));
  }

  const AgentProfile &p = kProfiles[s_agentSel];
  // v0.5.1 name: display role, TRUE FreeSansBold24pt7b scale 1 (was 9pt
  // scale5 jaggies). Worst name 'AGENT 2' = 198px, ink rows 186..222, chord
  // >= 216 at those rows (462px available at the widest): fits 130px/side.
  uiText(p.name, 185, &fonts::FreeSansBold24pt7b, p.color, TFT_BLACK);
  if (p.role[0]) {
    // v0.5.1 role: FreeSans18pt7b scale 1 (was 9pt scale2 = 442px, ~2px from
    // the chord). 18pt regular = ~427px worst ('Product & Market Research'),
    // ink rows 259..285, chord >= 223: fits 12px/side (bold 18 = ~470px
    // would clip -- hence regular weight).
    uiText(p.role, 258, &fonts::FreeSans18pt7b, 0x9AA3ADu, TFT_BLACK);
  }

  // v0.5.1 hint: meta role 9pt scale 1 at y=402 (was y=405, M1 showed the
  // 248px row grazing the bottom chord at y=430; audit: ink 403..416 needs
  // 124px/side vs chord 147 there -- 20px margin).
  uiText("CLICK NEXT - HOLD SELECT", 402, &fonts::FreeSans9pt7b, 0x5A6470u, TFT_BLACK);

}

// ---- SCR_SETTINGS: static screen --------------------------------------------
static void drawSettingsScreen() {
  auto &d = M5.Display;
  d.fillCircle(kCircleC, kCircleC, kCircleR, TFT_BLACK);
  const uint32_t accent = committedProfile().color;

  // v0.5.1 title: FreeSansBold12pt7b scale 1 (was 9pt scale2).
  // 'SETTINGS' = 121px, ink rows 88..106, chord >= 209: fits 120px/side.
  uiText("SETTINGS", 88, &fonts::FreeSansBold12pt7b, TFT_WHITE, TFT_BLACK);

  // volume: 230 px bar at y~230, filled in the committed agent's color
  // (geometry UNCHANGED: bx=118..348, y=224..242 -- all inside the chord).
  constexpr int16_t kBarW = 230;
  constexpr int16_t kBarH = 18;
  const int16_t bx = kCircleC - kBarW / 2;
  const int16_t y0 = kCircleC - kBarH / 2;
  d.fillRoundRect(bx, y0, kBarW, kBarH, 6, 0x232B33u);
  const int16_t fillW = (int16_t)(kBarW * (s_volumePct / 10.0f) + 0.5f);
  if (fillW > 0) d.fillRoundRect(bx, y0, fillW, kBarH, 6, accent);

  // v0.5.1 value row: display role, TRUE FreeSansBold24pt7b scale 1 (was 9pt
  // scale4). '10.0 / 10' worst = 181px, ink rows 158..193, chord >= 221:
  // fits 129px/side. Label above in dim meta; bar+value y untouched.
  uiText("VOLUME", 130, &fonts::FreeSans9pt7b, 0x8A929Cu, TFT_BLACK);
  char line[48];
  snprintf(line, sizeof(line), "%.1f / 10", s_volumePct);
  uiText(line, 156, &fonts::FreeSansBold24pt7b, TFT_WHITE, TFT_BLACK);

  // v0.5.1 host row: meta 9pt scale 1 (was 9pt scale2 = 320px w/ 'host:' —
  // M3 showed scale2's 19-char guard still allowed 498px). Worst legal
  // 'HOST 192.168.100.100:8765' = 235px, ink rows 291..304, chord >= 219:
  // fits 103px/side, no truncation needed at scale 1.
  if (strlen(s_wsHost)) {
    snprintf(line, sizeof(line), "HOST %s:%u", s_wsHost, s_wsPort);
  } else {
    snprintf(line, sizeof(line), "HOST unset");
  }
  uiText(line, 290, &fonts::FreeSans9pt7b, 0x8A929Cu, TFT_BLACK);

  // v0.5.1 status rows: meta 9pt scale 1, single-line groups at 28px pitch.
  // Widest measured 'FW v0.5.0  RSSI -99 dBm' = 207px @y318 (marg 106);
  // 'UP 65535 min' = 114px @y346 (marg 137).
  char status[64];
  snprintf(status, sizeof(status), "FW v%s  RSSI %d dBm", HG_VER_TAG + 6, (int)WiFi.RSSI());
  uiText(status, 318, &fonts::FreeSans9pt7b, 0x8A929Cu, TFT_BLACK);
  snprintf(line, sizeof(line), "UP %lu min", (unsigned long)(millis() / 60000u));
  uiText(line, 346, &fonts::FreeSans9pt7b, 0x8A929Cu, TFT_BLACK);

  // v0.5.1 hint: meta 9pt scale 1 @402 (ink 403..416, need 98/side vs 147).
  uiText("CLICK +0.5 - HOLD -1.0", 402, &fonts::FreeSans9pt7b, 0x5A6470u, TFT_BLACK);

}

// ---- SCR_CMDS: quick-action launcher (v0.5.9, James 2026-10-09 "4th screen" --
// frequently used commands). BtnB click = next item, hold >=800 ms = send.
// STATUS/RECAP ride the plain {"type":"text"} turn (hub '_h_text' -> agent
// turn, works TODAY on both ends); STOP sends cancel; NEW SESSION opens a
// fresh session — all no-hub-change routes. Selection cursor s_cmdSel.
static constexpr const char *kQuickCmdText[] = {
    "status check: what are you working on and what is pending?",
    "summarize our last conversation in a few bullets",
};
static constexpr uint8_t kQuickCmdCount = 4;  // 0 STATUS, 1 RECAP, 2 STOP, 3 NEW SESSION
static uint8_t s_cmdSel = 0;
static const char *kQuickCmdLabel(uint8_t i) {
  return i == 0 ? "STATUS" : i == 1 ? "RECAP" : i == 2 ? "STOP" : "NEW SESSION";
}
static void drawCommandsScreen() {
  auto &d = M5.Display;
  d.fillCircle(kCircleC, kCircleC, kCircleR, TFT_BLACK);
  const uint32_t accent = committedProfile().color;
  uiText("COMMANDS", 88, &fonts::FreeSansBold12pt7b, TFT_WHITE, TFT_BLACK);
  for (uint8_t i = 0; i < kQuickCmdCount; ++i) {
    const int16_t y = 176 + 62 * (int16_t)i;
    const bool sel = (i == s_cmdSel);
    char row[24];
    snprintf(row, sizeof(row), "%s %s", sel ? ">" : " ", kQuickCmdLabel(i));
    uiText(row, y, &fonts::FreeSans18pt7b,
           sel ? accent : 0x9AA3ADu, TFT_BLACK);
  }
  uiText("CLICK NEXT - HOLD SEND", 402, &fonts::FreeSans9pt7b, 0x5A6470u, TFT_BLACK);
}

// ---- screen switching -------------------------------------------------------
// Static screens are painted on the NEXT loop pass (not from inside the
// button handler): there the avatar engine may be mid-render; a frame later
// update() is skipped for these screens, so the paint is unopposed.
static void gotoScreen(ScreenId scr) {
  if (scr == s_screen) return;
  if (s_screen == SCR_SETTINGS && scr != SCR_SETTINGS) {
    saveVolumeIfDirty();  // M2: flush once when a tuned volume leaves settings
  }
  // H2: a hold that ENDS on another screen never reaches its screen-local
  // release handler (commit jumps to EYES mid-hold), leaking hold latches.
  // Clear them here; stop capture if PTT crosses a boundary; suppress a
  // mid-flight hold from re-arming PTT on the eyes screen.
  if (pttFromHold) {
    if (V.micOpen) voiceStopListening();
    pttFromHold = false;
  }
  agentHoldLatched = false;
  setHoldLatched = false;
  cmdHoldLatched = false;
  s_btnBSuppressMs = millis();
  s_screen = scr;
  if (s_screen == SCR_AGENTS) {
    s_agentSel = s_profileIdx;  // highlight starts at the committed agent
    s_rebuildScreenId = SCR_AGENTS;
  } else if (s_screen == SCR_SETTINGS) {
    s_rebuildScreenId = SCR_SETTINGS;
  } else if (s_screen == SCR_CMDS) {
    s_cmdSel = 0;
    s_rebuildScreenId = SCR_CMDS;
  } else {  // SCR_EYES
    M5.Display.fillCircle(kCircleC, kCircleC, kCircleR, TFT_BLACK);
    avatar.invalidate();  // full engine repaint over the static screen
    s_toastUntilMs = 0;
    s_toastShown = false;
    s_ringState = kRingRedrawAll;
  }
}

// ---- buttons ----------------------------------------------------------------
// LEFT (BtnA) = screen cycle (global, 150 ms debounce) · RIGHT (BtnB) =
// per-screen action: eyes: hold >=400 ms PTT / click = tap-to-talk · agents:
// click = next candidate, hold >=800 ms = commit · settings: click = volume
// +0.5, hold >=800 ms = volume -1.0 (one step, no repeat). Unchanged from
// v0.4: both-held 1 s diagnostics toggle, touch-hold = cancel.
void drawDiagnosticFrame();  // fwd decl: defined below (H3 call sites here)
static void handleButtons(uint32_t nowMs) {
  static uint32_t btnBDownMs = 0;
  if (M5.BtnB.wasPressed()) {
    btnBDownMs = nowMs;
    s_btnBPressScreen = s_screen;  // press origin tags the whole gesture (H2)
  }

  // LEFT: global screen cycle
  if (M5.BtnA.wasClicked() && nowMs - s_lastNavMs > 150) {
    s_lastNavMs = nowMs;
    gotoScreen((ScreenId)((s_screen + 1) % kScreenCount));
  }

  if (s_btnBPressScreen == SCR_EYES) {
    // H2: a committed agent-hold releases here after gotoScreen(EYES) mid-
    // press — suppress its tail from phantom-opening the mic for a moment.
    const bool pttSuppressed = (nowMs - s_btnBSuppressMs < 600);
    if (M5.BtnB.isPressed() && !pttFromHold && !V.micOpen && !pttSuppressed &&
        M5.BtnB.pressedFor(400)) {
      pttFromHold = true;
      voiceStartListening(true);
    }
    if (!M5.BtnB.isPressed() && pttFromHold) {
      pttFromHold = false;
      btnBHoldEndMs = nowMs;
      if (V.micOpen) voiceStopListening();
    }
    if (M5.BtnB.wasClicked() && !pttFromHold &&
        nowMs - s_lastNavMs > 150 && nowMs - btnBHoldEndMs > 100) {
      s_lastNavMs = nowMs;
      if (!V.micOpen) {
        Serial.println("[ui] tap-to-talk start");
        voiceStartListening(true);
      } else {
        Serial.println("[ui] tap-to-talk stop");
        voiceStopListening();
      }
    }
  } else if (s_btnBPressScreen == SCR_AGENTS) {
    if (M5.BtnB.wasClicked() && !agentHoldLatched &&
        nowMs - s_lastNavMs > 150 && nowMs - btnBHoldEndMs > 100) {
      s_lastNavMs = nowMs;
      s_agentSel = (s_agentSel + 1) % kProfileCount;
      Serial.printf("[ui] agent highlight: %s\n", kProfiles[s_agentSel].name);
      drawAgentsScreen();
    }
    if (M5.BtnB.isPressed() && !agentHoldLatched && M5.BtnB.pressedFor(800)) {
      agentHoldLatched = true;
      s_profileIdx = s_agentSel;  // COMMIT
      Serial.printf("[ui] agent committed: %s\n",
                    kProfiles[s_profileIdx].name);
      gotoScreen(SCR_EYES);
      s_toastUntilMs = nowMs + 1200;  // toast renders after returning to eyes
      s_toastShown = false;  // H1: toast must paint fresh in the eyes branch
    }
    if (!M5.BtnB.isPressed() && agentHoldLatched) {
      agentHoldLatched = false;
      btnBHoldEndMs = nowMs;
    }
  } else if (s_btnBPressScreen == SCR_CMDS) {
    if (M5.BtnB.wasClicked() && !cmdHoldLatched &&
        nowMs - s_lastNavMs > 150 && nowMs - btnBHoldEndMs > 100) {
      s_lastNavMs = nowMs;
      s_cmdSel = (uint8_t)((s_cmdSel + 1) % kQuickCmdCount);
      Serial.printf("[ui] cmd highlight: %s\n", kQuickCmdLabel(s_cmdSel));
      drawCommandsScreen();
    }
    if (M5.BtnB.isPressed() && !cmdHoldLatched && M5.BtnB.pressedFor(800)) {
      cmdHoldLatched = true;
      Serial.printf("[ui] cmd send: %s\n", kQuickCmdLabel(s_cmdSel));
      if (s_cmdSel == 2)       sendJson("cancel");       // STOP
      else if (s_cmdSel == 3)  sendJson("session.new");  // fresh session
      else {
        char id[12];
        snprintf(id, sizeof(id), "q%lu", (unsigned long)millis());
        String payload = String("{\"type\":\"text\",\"id\":\"") + id +
                         "\",\"text\":\"" + kQuickCmdText[s_cmdSel] +
                         "\",\"agent\":\"" + committedAgentTag() + "\"}";
        sendText(payload);
      }
      gotoScreen(SCR_EYES);
    }
    if (!M5.BtnB.isPressed() && cmdHoldLatched) {
      cmdHoldLatched = false;
      btnBHoldEndMs = nowMs;
    }
  } else if (s_btnBPressScreen == SCR_SETTINGS) {
    if (M5.BtnB.wasClicked() && !setHoldLatched &&
        nowMs - s_lastNavMs > 150 && nowMs - btnBHoldEndMs > 100) {
      s_lastNavMs = nowMs;
      s_volumePct += 0.5f;
      if (s_volumePct > 10.0f) s_volumePct = 0.0f;  // wraps 10.0 -> 0.0
      s_volumeDirty = true;  // NVS flush deferred to settings exit (M2)
      applyVolume();
      Serial.printf("[ui] volume %.1f\n", s_volumePct);
      drawSettingsScreen();
    }
    if (M5.BtnB.isPressed() && !setHoldLatched && M5.BtnB.pressedFor(800)) {
      setHoldLatched = true;  // one-step down, no auto-repeat (MVP)
      s_volumePct -= 1.0f;
      if (s_volumePct < 0.0f) s_volumePct = 0.0f;
      s_volumeDirty = true;  // NVS flush deferred to settings exit (M2)
      applyVolume();
      Serial.printf("[ui] volume %.1f\n", s_volumePct);
      drawSettingsScreen();
    }
    if (!M5.BtnB.isPressed() && setHoldLatched) {
      setHoldLatched = false;
      btnBHoldEndMs = nowMs;
    }
  }

  // H3: both-held 1 s = a real diagnostics VIEW, not a mute serial toggle.
  const bool bothHeld = M5.BtnA.pressedFor(1000) && M5.BtnB.pressedFor(1000);
  if (bothHeld && !s_diagLatched) {
    s_diagLatched = true;
    s_diagMode = !s_diagMode;
    Serial.printf("[ui] diagnostics %s\n", s_diagMode ? "on" : "off");
    if (s_diagMode) {
      // freeze the avatar for the full-screen diag view (rings/screens
      // resume on toggle-off); diag paints only on EYES-like idle screens
      if (s_screen == SCR_EYES && !s_toastShown) {
        drawDiagnosticFrame();
        s_diagPainted = true;
      }
    } else {
      // restore the actual screen: EYES = engine full repaint; static
      // screens = schedule their own rebuild (a bare circle-clear without
      // a repend would blank AGENTS/SETTINGS).
      s_diagPainted = false;
      if (s_screen == SCR_EYES) {
        M5.Display.fillCircle(kCircleC, kCircleC, kCircleR, TFT_BLACK);
        avatar.invalidate();
        s_ringState = kRingRedrawAll;
      } else {
        s_rebuildScreenId = s_screen;  // repaint the static screen as-is
      }
    }
  } else if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
    s_diagLatched = false;
  }
  // H3: live refresh of the diag view (1 Hz) while it is on.
  if (s_diagMode && s_diagPainted) {
    static uint32_t s_diagRefreshMs = 0;
    if (nowMs - s_diagRefreshMs >= 1000) {
      s_diagRefreshMs = nowMs;
      drawDiagnosticFrame();
    }
  }
  // v0.3: touch-hold = cancel (unchanged)
  const auto t = M5.Touch.getDetail(0);
  if (t.wasHold()) {
    sendJson("cancel");
    Serial.println("[hg] cancel sent");
  }
}

// ----------------------------------------------------- hg.v1 events ----

void dispatchJson(const char *rx, size_t len) {
  V.lastHubMs = millis();  // v0.5.12 hub-liveness stamp (turn-stall watchdog)
  // Text frames = JSON control surface. We parse with strstr (no dynamic alloc).
  static char buf[1024];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  // python json.dumps sends '{"type": "challenge"}' WITH spaces after colons;
  // our sniff patterns have none — compact the payload before matching.
  size_t w = 0;
  for (size_t r = 0; r < len; ++r) {
    if (rx[r] != ' ') buf[w++] = rx[r];
  }
  buf[w] = 0;

  if (strstr(buf, "\"type\":\"notice\"")) {
    const char *t = strstr(rx, "\"text\":\"");
    if (t) {
      t += 8;
      const char *end = strchr(t, '"');
      size_t n = end && (size_t)(end - t) < 100 ? (size_t)(end - t) : 100;
      char msg[101]; memcpy(msg, t, n); msg[n] = 0;
      Serial.printf("[hg] notice: %s\n", msg);
      drawStatusLine(msg);   // v0.5.2: failure notices must be VISIBLE, not serial-only
    }
    avatar.show(ExpressionId::Confused, millis(), false, 400);
    return;
  }
  if (strstr(buf, "\"type\":\"action\"")) {  // v0.5.11: hub action → avatar_face
    handleFaceAction(buf);
    return;
  }
  // ---- pairing flow
  if (strstr(buf, "\"type\":\"challenge\"")) {
    // {"type":"challenge","nonce":"<b64>","enrolled":bool}
    Serial.printf("[hg] challenge rx (enrolled=%d)\n", (int)s_enrolled);
    const char *nonce = strstr(buf, "\"nonce\":\"");
    const char *enr = strstr(buf, "\"enrolled\":");
    if (nonce) {
      nonce += 9;
      const char *end = strchr(nonce, '"');
      char nonceB64[80] = {};
      if (end && (end - nonce) < (int)sizeof(nonceB64)) {
        strlcpy(nonceB64, nonce, end - nonce + 1);
      }
      if (enr) s_enrolled = strstr(enr, "true") != nullptr;
      if (s_enrolled) sendAuthHmac(nonceB64);
      else            sendAuthEnroll();
    }
    return;
  }
  if (strstr(buf, "\"type\":\"welcome\"")) {
    s_connected = true;
    const char *p = strstr(buf, "\"paired\":");
    if (p) s_paired = strstr(p, "true") != nullptr;
    Serial.printf("[hg] welcome: paired=%d\n", (int)s_paired);
    // v0.5.4 probation: we reached the hub on this boot → confirm this image
    // so a future OTA that never connects rolls back automatically.
    esp_ota_mark_app_valid_cancel_rollback();
    avatar.show(ExpressionId::Excited, millis(), false, 300);
    return;
  }
  if (strstr(buf, "\"type\":\"pairing\"")) {
    const char *code = strstr(rx, "\"code\":\"");
    char codeS[16] = {};
    if (code) {
      code += 8;
      const char *end = strchr(code, '"');
      if (end && (end - code) < (int)sizeof(codeS)) strlcpy(codeS, code, end - code + 1);
    }
    Serial.printf("[hg] PAIRING CODE: %s — approve on the Mac:\n", codeS);
    Serial.printf("[hg]   hermes pairing approve gadget %s\n", codeS);
    drawStatusLine("PAIR?");
    avatar.show(ExpressionId::Confused, millis(), false, 400);
    return;
  }
  if (strstr(buf, "\"type\":\"paired\"")) {
    s_paired = true;
    drawStatusLine("PAIRED");
    avatar.show(ExpressionId::Excited, millis(), false, 400);
    return;
  }
  // ---- OTA (v0.5.4): offers/acks over the control surface
  if (strstr(buf, "\"type\":\"ota.")) {
    Ota::onText(buf);
    return;
  }
  if (strstr(buf, "\"type\":\"unpaired\"")) {
    s_paired = false;
    drawStatusLine("UNPAIRED");
    return;
  }
  if (strstr(buf, "\"type\":\"ping\"")) {
    // App-level heartbeat: hub closes us after >3 x 20 s of inbound silence.
    char pong[64];
    const char *ts = strstr(buf, "\"ts\":");
    if (ts) snprintf(pong, sizeof(pong), "{\"type\":\"pong\",\"ts\":%s}", ts + 5);
    else    snprintf(pong, sizeof(pong), "{\"type\":\"pong\",\"ts\":0}");
    sendText(String(pong));
    return;
  }

  // ---- turn lifecycle
  if (strstr(buf, "\"type\":\"turn.start\"")) { s_turnActive = true; setStateFromServer("turn.start"); return; }
  if (strstr(buf, "\"type\":\"status\""))      { setStateFromServer("status"); s_hasLiveStatus = true; return; }
  if (strstr(buf, "\"type\":\"transcript\"")) {
    const char *t = strstr(rx, "\"text\":\"");
    if (t) Serial.printf("[hg] transcript: %.80s\n", t + 8);
    return;
  }
  if (strstr(buf, "\"type\":\"reply.delta\"")) {
    setStateFromServer("reply.delta");
    // cumulative text: tail live-strip on the y=24 band (v0.5.3)
    const char *t = strstr(rx, "\"text\":\"");
    if (t) {
      t += 8;
      deltaClear();
      uint8_t q = 0;
      for (int i = 0; t[i] && t[i] != '"' && q < 6; ++i) {
        char c = t[i];
        if (c == '\\' && t[i + 1]) {           // minimal JSON unescape
          ++i;
          char e = t[i];
          if (e == 'n') c = ' ';
          else if (e == '"') c = '"';
          else if (e == '\\') c = '\\';
          else if (e == 'u') {                // UTF-16 escape: swallow 4 hex
            if (isxdigit((unsigned char)t[i+1]) && isxdigit((unsigned char)t[i+2]) &&
                isxdigit((unsigned char)t[i+3]) && isxdigit((unsigned char)t[i+4])) i += 4;
            c = 0; q = 6;                     // non-ASCII → stop (fonts are ASCII-only)
          }
          else c = e;
        } else if ((unsigned char)c > 0x7E) {  // UTF-8 multibyte / emoji → stop
          break;
        }
        if (!c) break;
        deltaPushC((uint8_t)c);
      }
      if (!s_toastShown) {                    // never collide with the toast
        M5.Display.fillRect(100, 367, 266, 17, TFT_BLACK);  // y=368 strip, half-chord ~178, clear chord-safe box
        deltaDraw();
        s_deltaLive = true;
      }
    }
    return;
  }
  if (strstr(buf, "\"type\":\"reply\"")) {
    if (s_deltaLive) { M5.Display.fillRect(100, 367, 266, 17, TFT_BLACK); deltaClear(); s_deltaLive = false; }
    const char *t = strstr(rx, "\"text\":\"");   // RAW rx: reply text keeps real spaces
    if (t) {
      t += 8;
      const char *end = strchr(t, '"');   // stop at the JSON string terminator
      const size_t n = end && (size_t)(end - t) < 120 ? (size_t)(end - t) : 120;
      char msg[121]; memcpy(msg, t, n); msg[n] = 0;
      Serial.printf("[hg] reply: %s\n", msg);
    }
    return;
  }
  if (strstr(buf, "\"type\":\"turn.end\"")) {
    s_turnActive = false;
    s_hasLiveStatus = false;
    if (s_deltaLive) { M5.Display.fillRect(100, 367, 266, 17, TFT_BLACK); deltaClear(); s_deltaLive = false; }
    // v0.5.16: a turn that NEVER had an audio phase (hub replaced the turn's
    // connection mid-turn — reply+turn.end landed on the new socket with no
    // audio.start/pcm/audio.end) wedges in THINKING/SPEAKING until the 3-min stall.
    // Snap straight back to idle when no audio ever existed in this turn.
    if (!V.micOpen && !V.playStream && !V.rUsed) {
      Serial.println("[voice] turn.end without audio phase — returning to idle");
      voiceReturnToIdle();
    }
    setStateFromServer("turn.end");
    return;
  }
  if (strstr(buf, "\"type\":\"audio.start\"") && strstr(buf, "\"format\":\"pcm16\"")) {
    // server→device TTS: {"type":"audio.start","stream":S,"rate":R,"format":"pcm16"}
    // (no direction field — hub never echoes OUR audio.start, so any rx = TTS)
    const char *st = strstr(buf, "\"stream\":");
    const char *rt = strstr(buf, "\"rate\":");
    uint16_t stream = 0;
    if (st) stream = (uint16_t)atoi(st + 9);
    uint32_t rate = kVoiceRate;
    if (rt) rate = (uint32_t)atoi(rt + 7);
    voicePlaybackReset();
    V.playStream = stream;
    V.lastAudioInMs = millis();
    voiceAllocBuffers();
    Serial.printf("[voice] tts stream %u @%lu Hz\n", (unsigned)stream, (unsigned long)rate);
    avatar.show(ExpressionId::Happy, millis(), false);
    return;
  }
  if (strstr(buf, "\"type\":\"audio.end\"")) {
    // device playback end OR our utterance ack (server's own audio.end for our upload comes as text too)
    const char *st = strstr(buf, "\"stream\":");
    if (st) {
      const uint16_t stream = (uint16_t)atoi(st + 9);
      if (stream == V.playStream && V.playStream) {
        V.playEnded = true;   // drain remaining ring, then reset
      } else if (stream == V.uttStream && V.micOpen == false) {
        Serial.println("[voice] upload closed by hub");
      }
    }
    return;
  }
  if (strstr(buf, "\"type\":\"audio.abort\"") || strstr(buf, "\"type\":\"audio.cancel\"")) {
    const char *st = strstr(buf, "\"stream\":");
    const uint16_t stream = st ? (uint16_t)atoi(st + 9) : V.playStream;
    if (stream == V.playStream && V.playStream) {
      Serial.println("[voice] tts aborted");
      voicePlaybackReset();
      M5.Speaker.stop();
    } else if (V.micOpen && stream == V.uttStream) {
      V.micOpen = false;
      M5.Mic.end();
      Serial.println("[voice] upload cancelled by hub");
    }
    return;
  }
  if (strstr(buf, "\"type\":\"error\"")) {
    Serial.printf("[hg] ERROR: %.140s\n", buf);
    if (strstr(buf, "auth_failed")) {
      s_enrolled = false;  // force re-enroll on next challenge
    }
    return;
  }
}

bool hgConnect() {
  if (!s_wifiSsid[0] || !s_wsHost[0]) return false;
  Serial.printf("[hg] connect → ws://%s:%u%s\n", s_wsHost, s_wsPort, kWsPath);
  if (!s_ws.connect(s_wsHost, s_wsPort, kWsPath)) {
    Serial.printf("[hg] connect FAILED (%s)\n", s_ws.lastDrop());
    return false;
  }
  s_connected = true;   // 101 + accept-key validated inside connect()
  sendHello();          // challenge follows — must go out SAME cycle
  return true;
}

// on socket death (send failure / server close / poll close frame)
void hgSocketDead() {
  if (!s_connected) return;
  s_connected = false;
  s_paired = false;
  // v0.5.12: a dead socket can never deliver turn.end — clear the orphaned turn
  // state or VH.ready() stays false and every later PTT reports "resource busy"
  // (the 2026-10-09 hang: gateway restart mid-turn wedged the watch until reboot).
  if (s_turnActive) { s_turnActive = false; s_hasLiveStatus = false; deltaClear(); s_deltaLive = false; }
  if (V.micOpen) { V.micOpen = false; M5.Mic.end(); Serial.println("[voice] mic closed (socket down)"); }
  voicePlaybackReset();
  M5.Speaker.stop();
  s_reconnectAtMs = millis() + kReconnectBaseMs;
  avatar.show(ExpressionId::Sleepy, millis(), false);
  Serial.println("[hg] ws closed");
}

// ------------------------------------------------------ diagnostics ----

void drawDiagnosticFrame() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  // v0.5.1: same 4-role type system (title + meta rows, top_center title);
  // rows at a shared x=56 left edge, measured against the chord: id row
  // (199px, ink 89..106) needs 176 from x=57 vs chord 182 there; host row
  // budget ~374px max incl '...' ; ws/ip rows sit deeper with 180px+ slack.
  uiText("DIAGNOSTICS", 24, &fonts::FreeSansBold12pt7b, TFT_WHITE, TFT_BLACK);
  // version label derived from HG_VER_TAG ("HGVER=x.y.z") — never drift again
  // (a 0.5.3 hardcoded literal survived three OTAs and fooled the user).
  {
    char ver[32];
    snprintf(ver, sizeof(ver), "hermes-watch v%s", HG_VER_TAG + 6);
    uiText(ver, 56, &fonts::FreeSans9pt7b, 0x9AA3ADu, TFT_BLACK);
  }
  char line[96];
  snprintf(line, sizeof(line), "id %s", s_deviceId);
  uiText(line, 88, &fonts::FreeSans9pt7b, TFT_WHITE, TFT_BLACK,
         lgfx::v1::textdatum_t::top_left, 56);
  if (strlen(s_wsHost)) {
    // 'host ' + host:port at 9pt ~ 7.4px/char; at y=120 (x=56) the right ink
    // tail must stay inside the chord: budget ~420px total -> host field
    // <= 28 chars incl port; 28-char worst measures 374px (margin 6/side).
    char hb[56];
    snprintf(hb, sizeof(hb), "%s:%u", s_wsHost, s_wsPort);
    if (strlen(hb) > 28) {
      hb[25] = 0;
      strcat(hb, "...");
    }
    snprintf(line, sizeof(line), "host %s", hb);
  } else {
    snprintf(line, sizeof(line), "host (unset)");
  }
  uiText(line, 120, &fonts::FreeSans9pt7b, TFT_WHITE, TFT_BLACK,
         lgfx::v1::textdatum_t::top_left, 56);
  snprintf(line, sizeof(line), "ws %s  paired %d  enrolled %d",
           s_connected ? "on" : "off", (int)s_paired, (int)s_enrolled);
  uiText(line, 152, &fonts::FreeSans9pt7b, TFT_WHITE, TFT_BLACK,
         lgfx::v1::textdatum_t::top_left, 56);
  snprintf(line, sizeof(line), "ip %s", WiFi.localIP().toString().c_str());
  uiText(line, 184, &fonts::FreeSans9pt7b, TFT_WHITE, TFT_BLACK,
         lgfx::v1::textdatum_t::top_left, 56);
  uiText("HOLD A+B 1s TO EXIT", 396, &fonts::FreeSans9pt7b, 0x5A6470u,
         TFT_BLACK);
}

}  // namespace

// ---------------------------------------------------------------- setup ----

void setup() {
  auto config = M5.config();
  config.serial_baudrate = 115200;
  M5.begin(config);
  Serial.begin(115200);

  M5.Display.setRotation(0);
  M5.Display.setBrightness(150);
  M5.Display.fillScreen(TFT_BLACK);

  M5.Touch.setHoldThresh(650);
  M5.Touch.setFlickThresh(10);

  // Speaker loudness: M5Unified default master_volume=64 → amplitude scaled
  // to ~6% ((64²)/(255²)) — the 'extremely quiet' bug. UserDemo runs 80-100
  // into the same ES8311 DAC register (0x32=0xEF / −6 dB ceiling).
  // v0.5: user default = 8.5/10 → 217 (was 240); changed live on the
  // settings screen and persisted below (NVS 'hermes-watch'/'vol').
  M5.Speaker.setVolume(217);

  // Voice hook wiring (anonymous namespace → VH fn pointers)
  VH.ready   = [] { return s_connected && s_paired && WiFi.status() == WL_CONNECTED && !s_turnActive; };
  VH.playbackIdle = [] { voiceReturnToIdle(); };
  VH.noRam   = [] { Serial.println("[voice] PSRAM alloc failed — voice disabled");
                    avatar.show(ExpressionId::Confused, millis(), false, 400); };
  VH.cancel  = [] { sendJson("cancel"); Serial.println("[hg] cancel sent (barge-in)"); };
  VH.sendText = [](const char *j, size_t) { return s_connected && s_ws.sendText(j) != 0; };
  VH.sendBin  = [](const uint8_t *d, size_t n) { return s_connected ? s_ws.sendBinary(d, n) : 0; };
  VH.agentTag = [] { return committedAgentTag(); };  // v0.5.10 turn routing

  ensureDeviceKey();
  // v0.5.4: bind OTA (here, not in ensureDeviceKey — b64/sendText are anon-ns and
  // the Arduino auto-prototyper can't forward them safely)
  Ota::begin(&s_deviceKey, s_deviceId, sendText, b64, "0.5.9");
  const bool provisioned = loadMeterProvisioning();
  Serial.printf("[setup] meter provisioning: %d (ssid=%s host=%s)\n",
                (int)provisioned, s_wifiSsid, s_wsHost);

  if (!avatar.begin()) {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("Avatar buffer failed", M5.Display.width() / 2,
                          M5.Display.height() / 2);
    Serial.println("Avatar sprite allocation failed");
  }

  { char b[40]; snprintf(b, sizeof(b), "hermes-watch %s", HG_VER_TAG + 6); drawStatusLine(b); }

  // Saved host from a previous 'H' command wins over the meter's NVS guess.
  {
    Preferences prefs;
    if (prefs.begin("hermes-watch", true)) {
      String h = prefs.getString("host", "");
      prefs.end();
      if (h.length()) {
        int colon = h.indexOf(':');
        if (colon > 0) {
          strlcpy(s_wsHost, h.c_str(), colon < (int)sizeof(s_wsHost) ? colon + 1 : sizeof(s_wsHost));
          s_wsPort = (uint16_t)h.substring(colon + 1).toInt();
        } else {
          strlcpy(s_wsHost, h.c_str(), sizeof(s_wsHost));
        }
        Serial.printf("[hg] saved host: %s:%u\n", s_wsHost, s_wsPort);
      }
    }
  }
  // Settings-screen volume persisted across reboots (10-step user scale).
  {
    Preferences prefs;
    if (prefs.begin("hermes-watch", true)) {
      const float vol = prefs.getFloat("vol", 8.5f);
      prefs.end();
      if (vol >= 0.0f && vol <= 10.0f && vol != s_volumePct) {
        s_volumePct = vol;
        M5.Speaker.setVolume(volumeLevel255());
      }
    }
  }
  // v0.5.9: restore the tap-cycled idle face.
  {
    Preferences prefs;
    if (prefs.begin("hermes-watch", true)) {
      const uint8_t f = prefs.getUChar("face", 0);
      prefs.end();
      if (f < 12) s_idleFace = faceFromIdx(f);  // v0.5.11: all 12 faces persist
    }
  }
  Serial.printf("[setup] effective host: %s:%u\n", s_wsHost, s_wsPort);
  { char sb[96]; snprintf(sb, sizeof(sb), "hermes-watch v%s — idle=NO ring, tap cycles face, CMD screen, audio-stall watchdog", HG_VER_TAG + 6); Serial.println(sb); }
  Serial.println("Commands: <expression> | say <text> | T | H <host[:port]> | state");

  // WiFi: the meter's own proven NVS creds (same flash, no re-setup).
  if (provisioned) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setHostname("hermes-watch");
    WiFi.begin(s_wifiSsid, s_wifiPass);
  } else {
    Serial.println("[setup] NO provisioning — flash the meter first or use 'H' command");
  }
}


void loop() {
  M5.update();
  const uint32_t nowMs = millis();

  // WiFi maintain
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t wifiWarnMs = 0;
    if (nowMs - wifiWarnMs > 5000) {
      wifiWarnMs = nowMs;
      WiFi.reconnect();
      Serial.println("[hg] wifi reconnecting...");
    }
    // H1: during the commit-toast window the engine is frozen (update()
    // would erase the toast); skip both update and ring here — the main
    // screen-sequencing block below owns them.
    if (s_screen == SCR_EYES && !s_toastShown && nowMs >= s_toastUntilMs) {
      avatar.update(nowMs);
      drawRingFor(turnStateName(), nowMs);
    }
    delay(20);
    // fall through: buttons/touch/IMU/serial stay live even without WiFi
  }

  // v0.5.12: turn-stall watchdog — if the server went silent mid-turn (turn.start
  // orphaned with a live socket: hub crash before turn.end), self-clear after 3 min
  // so the watch never hangs in THINKING with a live socket.
  // v0.5.16: two faster cases observed when the hub replaced the turn's connection
  // mid-turn (2026-10-09 20:48 fake-watch probe turn vs. real-watch reconnect — reply
  // + turn.end went to the OLD socket, no audio phase existed, and the ring wedged):
  //   (a) turn active, no audio phase started, and 20 s of hub silence — the turn is
  //       orphaned; self-clear instead of waiting out the 3-minute stall.
  //   (b) a 'reply'/'turn.end' landing mid-turn with ZERO audio phase — snap back to
  //       idle immediately (handled inline in dispatchJson where reply/turn.end arrive).
  if (s_turnActive && !V.micOpen) {
    const bool noAudioYet = !V.playStream && !V.rUsed;
    const uint32_t stall = noAudioYet ? kTurnStallNoAudioMs : kTurnStallMs;
    if ((int32_t)(nowMs - V.lastHubMs) > stall) {
      Serial.println("[hg] turn stall watchdog: no hub activity — clearing turn state");
      s_turnActive = false;
      s_hasLiveStatus = false;
      deltaClear(); s_deltaLive = false;
      voiceReturnToIdle();
      setStateFromServer("turn.stall-clear");
    }
  }

  // WS maintain
  if (!s_connected && nowMs >= s_reconnectAtMs) {
    static uint32_t s_reconnFail = 0;
    const uint32_t shift = s_reconnFail > 4 ? 4 : s_reconnFail;
    s_reconnectAtMs = nowMs + (kReconnectBaseMs << shift);  // true exponential backoff
    if (hgConnect()) { s_reconnectAtMs = nowMs + kHeartbeatIntervalMs; s_reconnFail = 0; }
    else { ++s_reconnFail; }
  }
  if (s_connected) {
    s_ws.poll(dispatchJson, voiceOnBinary);
    if (!s_ws.connected()) hgSocketDead();
  }

  if (s_pttStartSerial) { s_pttStartSerial = false; if (!V.micOpen) voiceStartListening(false); }
  if (s_pttStopSerial)  { s_pttStopSerial  = false; if (V.micOpen)  voiceStopListening();  }

  handleButtons(nowMs);

  // Input triage: 1 Hz raw-vs-M5 readout (settles mapping-vs-hardware live)
  {
    static uint32_t s_triageMs = 0;
    if (nowMs - s_triageMs >= 1000) {
      s_triageMs = nowMs;
      // Brute pin scan: prints level CHANGES only — find the real button pins.
      static uint32_t prevLevels = 0;
      uint32_t lv = 0;
      const int pins[] = {1, 2, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                          21, 38, 39, 40, 41, 42, 47, 48};
      for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        lv |= (m5gfx::gpio_in(static_cast<gpio_num_t>(pins[i])) ? 1u : 0u) << i;
      }
      const uint32_t changed = lv ^ prevLevels;
      if (changed) {
        char moved[112] = {};
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
          if (changed & (1u << i)) {
            snprintf(moved + strlen(moved), sizeof(moved) - strlen(moved),
                     "g%d:%u->%u ", pins[i], (prevLevels >> i) & 1, (lv >> i) & 1);
          }
        }
        Serial.printf("[pin-move] %s\n", moved);
        prevLevels = lv;
      }
      Serial.printf("[triage] scan=0x%08lx m5A=%d m5B=%d touch=%d touchEn=%d imuEn=%d\n",
                    (unsigned long)lv, (int)M5.BtnA.isPressed(), (int)M5.BtnB.isPressed(),
                    (int)M5.Touch.getCount(), (int)M5.Touch.isEnabled(), (int)M5.Imu.isEnabled());
    }
  }
  voiceMicPump();        // PTT capture → binary frames (20 ms per loop)
  voiceSpeakerPump();    // TTS ring → speaker (≤ 40 ms per loop)
  // v0.5.3 playback watchdog: a stuck/oversized stream must not talk for
  // minutes (user: "stop the output speech… run-on for no reason"). After
  // 90 s of one stream with no audio.end, hard-stop locally and surface it.
  static uint32_t playWatchMs = 0;   // run-on cap (90 s overall), unchanged
  static uint32_t playLastFrame = 0; // v0.5.9 stall: last inbound audio frame
  if (V.playStream) {
    if (playWatchMs == 0) { playWatchMs = nowMs; playLastFrame = V.lastAudioInMs; }
    else if (V.lastAudioInMs != playLastFrame) {
      playLastFrame = V.lastAudioInMs;              // frames still flowing
    } else if (!V.playEnded && nowMs - playLastFrame > 5000) {
      // v0.5.9: a hub pump that dies
      // mid-stream used to strand the device in SPEAKING forever — no
      // audio.end ever arrived. 5 s of zero frames while playing = dead
      // stream: un-stick locally. (Hub-side abort-on-pump-death is queued.)
      Serial.println("[voice] audio stall watchdog: 5 s with no frames — reset");
      playWatchMs = 0;
      M5.Speaker.stop();
      voicePlaybackReset();  // clears V.playStream → ring/idle, mic usable
      drawStatusLine("AUDIO STALLED");
      avatar.show(ExpressionId::Confused, millis(), false, 400);
    } else if (nowMs - playWatchMs > 90000) {
      Serial.println("[voice] playback watchdog: stream exceeded 90 s — stopped");
      playWatchMs = 0;
      M5.Speaker.stop();
      voicePlaybackReset();   // clears V.playStream → ring falls back to IDLE
      drawStatusLine("PLAYBACK STOPPED");
      avatar.show(ExpressionId::Confused, millis(), false, 400);
    }
  } else {
    playWatchMs = 0;          // stream ended/aborted → rearm the watchdog
  }
  // v0.5.14: erase the status band once "idle for real" so the screen is fully
  // black between turns (no lingering PLAYBACK STOPPED / face label).
  if (s_clearStatusAtMs && nowMs >= s_clearStatusAtMs) {
    if (!s_turnActive && !V.micOpen && !V.playStream) {
      eraseStatusLine();
    }
    s_clearStatusAtMs = 0;
  }
  Ota::tick();
  handleProductTouch(nowMs);
  handleImuInteraction(nowMs);
  handleSerialCommands();

  // ---- v0.5 screen/render sequencing ----------------------------------------
  // One static-screen paint per pass, never while the engine's render may
  // still be in flight from a pre-nav show(...)/invalidate() — that queued
  // render would land on top at the next update(). Paint first, then run the
  // avatar/ring for eyes only; WS/serial/voice pumps above stay always-on.
  if (s_rebuildScreenId != SCR_EYES) {
    if (s_rebuildScreenId == SCR_AGENTS) {
      drawAgentsScreen();
    } else if (s_rebuildScreenId == SCR_CMDS) {
      drawCommandsScreen();
    } else {  // SCR_SETTINGS
      drawSettingsScreen();
    }
    s_rebuildScreenId = SCR_EYES;
  } else {
    if (s_screen == SCR_EYES) {
      // H3: diagnostics view owns the panel while active — engine frozen.
      if (s_diagMode && s_diagPainted) {
        // nothing per frame; 1 Hz refresh happens in handleButtons
      } else if (nowMs < s_toastUntilMs) {
        // H1: toast window — the engine is FROZEN here. avatar.update()
        // renders its queued full-clear (fillScreen) on the same pass the
        // toast was drawn, erasing it within one frame. Draw the toast over
        // a fresh circle-clear instead; the ring (r210-228) sits outside
        // the toast disc (r160 max) so it keeps animating.
        if (!s_toastShown) {
          M5.Display.fillCircle(kCircleC, kCircleC, kCircleR, TFT_BLACK);
          drawCommitToast();
          s_toastShown = true;
        }
        drawRingFor(turnStateName(), nowMs);
      } else if (s_toastShown) {
        // expired: clear the circle, resume the engine with a full repaint
        s_toastShown = false;
        M5.Display.fillCircle(kCircleC, kCircleC, kCircleR, TFT_BLACK);
        avatar.invalidate();
        s_ringState = kRingRedrawAll;
      } else {
        avatar.update(nowMs);
        drawRingFor(turnStateName(), nowMs);
      }
    }
    // SCR_AGENTS / SCR_SETTINGS / SCR_CMDS: fully static — nothing per frame.
  }

  delay(1);
}