// hermes_ota.h — v0.5.4 device-side OTA over the hg.v1 WebSocket (channel 0x03).
// Implements the Hermes Gadget protocol firmware-update contract (plugin/ota.py):
//   ota.offer → ota.ready{nonce} →
//   ota.begin{mac = HMAC-SHA256(key, "hermes-gadget/v1|ota|<id>|<nonce>|<sha256hex>|<size>")} →
//   ota.ack{offset:0} → binary ch 0x03 frames (4 KB, seq from 0) → ota.ack every 16 KB →
//   ota.end → size + SHA-256 checks → ota.done{version} → restart into the new slot (ota_1).
// Partition layout (partitions_v054.csv): app0/ota_0 @0x10000 (3 MB, current), ota_1 @0x400000 (3 MB).
// Arduino Update.h selects the *other* app partition automatically via esp_ota_begin.
// Probation/rollback: images installed via OTA boot in ESP_OTA_IMG_NEW → hermes-watch.ino calls
// esp_ota_mark_app_valid_cancel_rollback() on the first `welcome` so a bad image that never
// reaches the hub rolls back to the previous slot. The running USB-flashed build stays VALID.
// Binding: the sketch calls Ota::begin(key, id, sendFn, b64Fn, version) after its own state exists
// (the sketch's key/state are `static` — internal linkage — so nothing is extern'd here).
#include <Arduino.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <Update.h>

namespace Ota {

// ---- bindings (set once by Ota::begin) --------------------------------------
static const uint8_t (*_key)[32] = nullptr;
static const char   *_id = nullptr;
static const char   *_fwVersion = "0.5.4";
static void (*_send)(const String &) = nullptr;
static String (*_b64)(const uint8_t *, size_t) = nullptr;

inline void begin(const uint8_t (*deviceKey)[32], const char *deviceId,
                  void (*sendTextFn)(const String &),
                  String (*b64Fn)(const uint8_t *, size_t),
                  const char *fwVersion) {
  _key = deviceKey;
  _id = deviceId;
  _send = sendTextFn;
  _b64 = b64Fn;
  _fwVersion = fwVersion;
}

enum Phase : uint8_t { IDLE, OFFERED, WRITING };

struct State {
  Phase    phase = IDLE;
  uint32_t size = 0;              // advertised image size
  uint32_t written = 0;           // bytes written+hashed
  uint16_t stream = 0;            // ota.offer stream id
  uint16_t seq = 0;               // expected next frame seq (gap → abandon)
  char     sha256hex[65] = {};    // lowercase hex digest from ota.offer
  char     version[40] = {};      // from ota.offer (echoed in ota.done)
  char     nonce[25] = {};        // fresh nonce, b64 of 16 B
  uint32_t lastDataMs = 0;        // 30 s silence watchdog
  mbedtls_sha256_context sha;     // streaming SHA-256 of written bytes
};
static State st;

static void stateReset(bool abortUpdate) {
  if (abortUpdate && !Update.isFinished()) Update.abort();
  mbedtls_sha256_free(&st.sha);
  st = State();
}

static void fail(const char *code, const char *msg) {
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"type\":\"ota.error\",\"code\":\"%s\",\"message\":\"%s\"}", code, msg);
  _send(String(buf));
  stateReset(st.phase == WRITING);
  Serial.printf("[ota] fail %s: %s\n", code, msg);
}

static void hexdigest(const uint8_t *d32, char out[65]) {
  static const char *hx = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = hx[d32[i] >> 4];
    out[i * 2 + 1] = hx[d32[i] & 0xF];
  }
  out[64] = 0;
}

// ---- text surface (routed from dispatchJson on ota.offer/begin/end/abort) ----
void onText(const char *buf) {
  if (strstr(buf, "\"type\":\"ota.offer\"")) {
    if (st.phase != IDLE) { fail("busy", "update already in progress"); return; }
    const char *v = strstr(buf, "\"size\":");
    const uint32_t size = v ? (uint32_t)strtoul(v + 7, nullptr, 10) : 0;
    v = strstr(buf, "\"stream\":");
    const uint16_t stream = v ? (uint16_t)atoi(v + 9) : 0;
    v = strstr(buf, "\"sha256\":\"");
    char sha[65] = {};
    if (v) strlcpy(sha, v + 10, sizeof(sha));
    v = strstr(buf, "\"version\":\"");
    if (v) {
      strlcpy(st.version, v + 11, sizeof(st.version));
      char *e = strchr(st.version, '"');
      if (e) *e = 0;
    }
    if (!size || size < 4096 || !sha[0]) { fail("bad_offer", "offer missing size/sha256"); return; }
    if (size > 0x300000 - 0x100) { fail("too_large", "image exceeds ota_1"); return; }
    st.size = size;
    st.stream = stream;
    strlcpy(st.sha256hex, sha, sizeof(st.sha256hex));
    uint8_t n[16];
    for (auto &b : n) b = (uint8_t)esp_random();
    strlcpy(st.nonce, _b64(n, sizeof(n)).c_str(), sizeof(st.nonce));
    char r[96];
    snprintf(r, sizeof(r), "{\"type\":\"ota.ready\",\"nonce\":\"%s\"}", st.nonce);
    _send(String(r));
    st.phase = OFFERED;
    st.lastDataMs = millis();
    Serial.printf("[ota] offer %u B stream %u ver %.16s\n", (unsigned)size, stream, st.version);
    return;
  }
  if (strstr(buf, "\"type\":\"ota.begin\"")) {
    if (st.phase != OFFERED) { fail("no_offer", "no accepted offer"); return; }
    const char *m = strstr(buf, "\"mac\":\"");
    char mac[48] = {};
    if (m) {
      strlcpy(mac, m + 7, sizeof(mac));
      char *e = strchr(mac, '"');
      if (e) *e = 0;
    }
    char msg[160];
    const int n = snprintf(msg, sizeof(msg), "hermes-gadget/v1|ota|%s|%s|%s|%u",
                           _id, st.nonce, st.sha256hex, (unsigned)st.size);
    uint8_t macX[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    *_key, 32,
                    (const uint8_t *)msg, (size_t)n, macX);
    const String macB64 = _b64(macX, 32);
    if (!mac[0] || macB64.length() < 20 || memcmp(macB64.c_str(), mac, macB64.length()) != 0) {
      fail("unauthorized", "ota.begin MAC mismatch");
      return;
    }
    if (!Update.begin(st.size, U_FLASH)) { fail("flash", Update.errorString()); return; }
    mbedtls_sha256_init(&st.sha);
    mbedtls_sha256_starts(&st.sha, 0);
    st.phase = WRITING;
    st.written = 0;
    st.seq = 0;
    st.lastDataMs = millis();
    _send(String("{\"type\":\"ota.ack\",\"offset\":0}"));
    Serial.println("[ota] authorized — streaming");
    return;
  }
  if (strstr(buf, "\"type\":\"ota.end\"")) {
    if (st.phase != WRITING) { fail("no_update", "no update in progress"); return; }
    if (st.written != st.size) { fail("size", "byte count mismatch"); return; }
    uint8_t dg[32];
    char hex[65];
    mbedtls_sha256_finish(&st.sha, dg);
    hexdigest(dg, hex);
    if (strcmp(hex, st.sha256hex) != 0) { fail("checksum", "SHA-256 mismatch"); return; }
    if (!Update.end(true)) { fail("invalid", Update.errorString()); return; }
    char d[96];
    snprintf(d, sizeof(d), "{\"type\":\"ota.done\",\"version\":\"%s\"}",
             st.version[0] ? st.version : _fwVersion);
    _send(String(d));
    Serial.printf("[ota] installed %.16s — restart in 1 s\n", st.version);
    delay(1000);
    ESP.restart();
    return;  // unreachable
  }
  if (strstr(buf, "\"type\":\"ota.abort\"")) {
    stateReset(st.phase == WRITING);
    Serial.println("[ota] aborted by hub");
    return;
  }
}

// ---- binary surface (channel 0x03; routed from the ws binary dispatcher) ----
// p = full ws binary payload INCLUDING the 4-byte header: ch, stream, seq LE16.
void onBinary(const uint8_t *p, size_t len) {
  if (st.phase != WRITING) return;
  st.lastDataMs = millis();
  if (len < 4) { fail("protocol", "short frame"); return; }
  if (p[0] != 0x03 || p[1] != st.stream) { fail("protocol", "foreign frame"); return; }
  const uint16_t seq = (uint16_t)(p[2] | (p[3] << 8));
  if (seq != st.seq) { fail("sequence", "frame gap"); return; }
  const uint8_t *data = p + 4;
  const size_t dlen = len - 4;
  if (!dlen || st.written + (uint32_t)dlen > st.size) { fail("size", "overflow"); return; }
  if (Update.write(const_cast<uint8_t *>(data), dlen) != dlen) { fail("flash", "write failed"); return; }
  mbedtls_sha256_update(&st.sha, data, dlen);
  st.written += (uint32_t)dlen;
  ++st.seq;
  if ((st.written & 0x3FFF) == 0 || st.written == st.size) {  // every 16 KB + final
    char a[64];
    snprintf(a, sizeof(a), "{\"type\":\"ota.ack\",\"offset\":%u}", (unsigned)st.written);
    _send(String(a));
  }
}

// 30 s no-data watchdog; call from loop()
void tick() {
  if (st.phase == IDLE) return;
  if (millis() - st.lastDataMs > 30000) fail("timeout", "update starved");
}

}  // namespace Ota