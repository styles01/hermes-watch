// ws_raw.h — hand-rolled RFC6455 WebSocket client for hermes-watch.
// Replaces the vendored ArduinoWebsockets (tiny_websockets): its frame
// reader assumes read(buf,len) fills exactly len, but ESP32 core 3.3.x
// NetworkClient::read returns partial counts — frames desync silently
// (challenge frames never dispatched, "errno 9" fd churn on reconnect).
// This client: full read-loops everywhere, incremental non-blocking RX
// state machine, per-frame random masking, ping→pong, close handling.
// Part of hermes-watch (GPL-3.0), same as the sketch.
#pragma once

#include <Arduino.h>
#include <NetworkClient.h>
#include <esp_random.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>

class WsRawClient {
public:
  typedef void (*DispatchFn)(const char *payload, size_t len);
  typedef void (*BinDispatchFn)(const uint8_t *payload, size_t len);

  bool connected() const { return _open; }
  const char *lastDrop() const { return _lastDrop; }

  // Blocking handshake (~<2.5 s budget, runs once per reconnect attempt).
  bool connect(const char *host, uint16_t port, const char *path) {
    close();
    ensurePayloadBuf();
    if (!_payload) { setDrop("noRam"); return false; }
    if (!_tcp.connect(host, port, 1200)) { setDrop("tcp"); return false; }  // bounded: no 5.5s wedge when hub is down
    _tcp.setNoDelay(true);

    uint8_t keyRaw[16];
    for (auto &b : keyRaw) b = (uint8_t)(esp_random() & 0xFF);
    char keyB64[29] = {};
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)keyB64, sizeof(keyB64) - 1, &olen, keyRaw, 16);

    char req[320];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: hermes-gadget.v1\r\n"
        "\r\n",
        path, host, (unsigned)port, keyB64);
    if (n <= 0 || (size_t)n >= sizeof(req) ||
        writeAll((const uint8_t *)req, (size_t)n) != (size_t)n) {
      close(); setDrop("req-write"); return false;
    }

    const uint32_t deadline = millis() + 1200;
    char line[300];
    if (!readLine(line, sizeof(line), deadline)) { close(); setDrop("status-timeout"); return false; }
    if (!strstr(line, "101")) { close(); setDrop(line); return false; }

    char accept[40] = {};
    for (;;) {
      if (!readLine(line, sizeof(line), deadline)) { close(); setDrop("hdr-timeout"); return false; }
      if (line[0] == '\r' || line[0] == '\n' || line[0] == 0) break;
      const char *a = strstr(line, "Sec-WebSocket-Accept");
      if (a) {
        a = strchr(a, ':');
        if (!a) continue;
        ++a; while (*a == ' ') ++a;
        strlcpy(accept, a, sizeof(accept));
        for (char *e = accept + strlen(accept) - 1;
             e >= accept && (*e == '\r' || *e == '\n' || *e == ' '); --e) *e = 0;
      }
    }
    if (!accept[0]) { close(); setDrop("no-accept"); return false; }

    char cat[72];
    snprintf(cat, sizeof(cat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", keyB64);
    uint8_t sha[20];
    int sh = mbedtls_sha1((const unsigned char *)cat, strlen(cat), sha);
    char expect[40] = {};
    int be = mbedtls_base64_encode((unsigned char *)expect, sizeof(expect) - 1, &olen, sha, 20);
    if (sh != 0 || be != 0) {
      Serial.printf("[ws] sha1=%d b64=%d olen=%u\n", sh, be, (unsigned)olen);
      close(); setDrop("crypto-fail"); return false;
    }
    expect[olen] = 0;
    if (strcmp(expect, accept) != 0) {
      Serial.printf("[ws] key=%s accept=%s expect=%s\n", keyB64, accept, expect);
      close(); setDrop("accept-mismatch"); return false;
    }

    _open = true;
    resetRx();
    return true;
  }

  // Masked text frame. Returns bytes of payload accepted (0 = dead socket).
  size_t sendText(const char *payload) {
    if (!_open) return 0;
    const size_t len = strlen(payload);
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x81;  // FIN | text
    if (len <= 125) {
      hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xFFFF) {
      hdr[h++] = 0x80 | 126;
      hdr[h++] = (uint8_t)(len >> 8); hdr[h++] = (uint8_t)(len & 0xFF);
    } else {
      hdr[h++] = 0x80 | 127;
      for (int i = 7; i >= 0; --i) hdr[h++] = (uint8_t)((len >> (8 * i)) & 0xFF);
    }
    uint8_t mk[4];
    for (auto &m : mk) m = (uint8_t)(esp_random() & 0xFF);
    memcpy(hdr + h, mk, 4); h += 4;
    if (writeAll(hdr, h) != h) { drop("send-hdr"); return 0; }

    static uint8_t scratch[512];
    size_t sent = 0;
    while (sent < len) {
      const size_t chunk = (len - sent) > sizeof(scratch) ? sizeof(scratch) : (len - sent);
      for (size_t i = 0; i < chunk; ++i)
        scratch[i] = (uint8_t)payload[sent + i] ^ mk[(sent + i) & 3];
      if (writeAll(scratch, chunk) != chunk) { drop("send-body"); return sent; }
      sent += chunk;
    }
    return sent;
  }

  // Incremental RX: consumes only what the socket has buffered, never blocks
  // the render loop. Completed text frames call dispatch(payload, len).
  void poll(DispatchFn dispatch, BinDispatchFn binDispatch = nullptr) {
    if (!_open) return;
    for (int spin = 0; spin < 8; ++spin) {
      if (_tcp.available() <= 0) break;
      switch (_rx) {
        case RxHdr: {
          _hdrBytes += grab(_hdr + _hdrBytes, 2 - _hdrBytes);
          if (_hdrBytes < 2) return;
          _op = _hdr[0] & 0x0F;
          _fin = (_hdr[0] & 0x80) != 0;
          _masked = (_hdr[1] & 0x80) != 0;
          const uint64_t l = _hdr[1] & 0x7F;
          _hdrBytes = 0;
          if (l < 126) {
            _payloadLen = l; _payloadFill = 0;
            if (l == 0) { frameDone(dispatch, binDispatch); break; }
            _rx = _masked ? RxMask : RxPayload;
          } else {
            _extTotal = (l == 126) ? 2 : 8; _extFill = 0; _rx = RxExt;
          }
          break;
        }
        case RxExt: {
          _extFill += grab(_ext + _extFill, _extTotal - _extFill);
          if (_extFill < _extTotal) return;
          uint64_t l = 0;
          for (int i = 0; i < _extTotal; ++i) l = (l << 8) | _ext[i];
          _payloadLen = l; _payloadFill = 0;
          _rx = _masked ? RxMask : RxPayload;
          if (_payloadLen == 0) frameDone(dispatch, binDispatch);
          break;
        }
        case RxMask: {  // server frames must not be masked; consume key if one appears
          static uint8_t mk[4];
          _maskFill += grab(mk + _maskFill, 4 - _maskFill);
          if (_maskFill < 4) return;
          _maskFill = 0;
          _rx = _payloadLen ? RxPayload : RxPay;
          if (!_payloadLen) { _rx = RxHdr; break; }
          _rx = RxPayload;
          break;
        }
        case RxPayload: {
          const uint32_t room = kPayloadCap - _payloadFill;
          const uint32_t want = (uint32_t)(_payloadLen - _payloadFill);
          const uint32_t take = want < room ? want : room;
          if (take) {
            const int gr = grab(_payload + _payloadFill, take);
            if (gr <= 0) { Serial.printf("[ws-dbg] payload grab=%d want=%u\n", gr, (unsigned)take); return; }
            _payloadFill += (uint32_t)gr;
          }
          // Oversized frames: consume-and-discard to stay in sync.
          if (_payloadFill == kPayloadCap && _payloadLen > _payloadFill) {
            static uint8_t sink[512];
            const uint64_t rest = _payloadLen - _payloadFill;
            uint64_t eaten = 0;
            while (eaten < rest) {
              const uint32_t c = (uint32_t)(rest - eaten) > sizeof(sink) ? sizeof(sink) : (uint32_t)(rest - eaten);
              const int got = grab(sink, c);
              if (got <= 0) { _rxOverCap = true; return; }
              eaten += got;
            }
            _payloadFill = (uint32_t)_payloadLen;  // mark complete-but-dropped
            _rxOverCap = true;
            frameDone(dispatch, binDispatch);
            break;
          }
          if (_payloadFill < (uint32_t)_payloadLen) return;
          frameDone(dispatch, binDispatch);
          break;
        }
        default: return;
      }
      if (!_open) break;
    }
  }

  // Masked binary frame (PCM/audio payloads). Same chunked path as sendText.
  size_t sendBinary(const uint8_t *payload, size_t len) {
    if (!_open || !len) return 0;
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x82;  // FIN | binary
    if (len <= 125) {
      hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xFFFF) {
      hdr[h++] = 0x80 | 126;
      hdr[h++] = (uint8_t)(len >> 8); hdr[h++] = (uint8_t)(len & 0xFF);
    } else {
      hdr[h++] = 0x80 | 127;
      for (int i = 7; i >= 0; --i) hdr[h++] = (uint8_t)((len >> (8 * i)) & 0xFF);
    }
    uint8_t mk[4];
    for (auto &m : mk) m = (uint8_t)(esp_random() & 0xFF);
    memcpy(hdr + h, mk, 4); h += 4;
    if (writeAll(hdr, h) != h) { drop("bin-hdr"); return 0; }
    static uint8_t scratch[512];
    size_t sent = 0;
    while (sent < len) {
      const size_t chunk = (len - sent) > sizeof(scratch) ? sizeof(scratch) : (len - sent);
      for (size_t i = 0; i < chunk; ++i)
        scratch[i] = payload[sent + i] ^ mk[(sent + i) & 3];
      if (writeAll(scratch, chunk) != chunk) { drop("bin-body"); return sent; }
      sent += chunk;
    }
    return sent;
  }

  void close() {
    if (_open) {
      _open = false;
      static const uint8_t f[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
      _tcp.write(f, 6);  // polite close frame; ignore errors
    }
    _open = false;
    _tcp.stop();
  }

private:
  enum RxState { RxHdr, RxExt, RxMask, RxPayload, RxPay };
  static constexpr uint32_t kPayloadCap = 64 * 1024;

  bool readLine(char *dst, size_t cap, uint32_t deadline) {
    size_t i = 0;
    while (millis() < deadline) {
      int c = _tcp.read();
      if (c < 0) { delayMicroseconds(250); continue; }
      if (i < cap - 1) dst[i++] = (char)c;
      if (c == '\n') { dst[i] = 0; return true; }
    }
    return false;
  }

  int grab(uint8_t *dst, uint32_t n) {
    if (!n) return 0;
    return (int)_tcp.read(dst, n);  // returns buffered count ≤ n; loop caller resumes
  }

  size_t writeAll(const uint8_t *src, size_t len) {
    size_t sent = 0;
    while (sent < len) {
      const int w = _tcp.write(src + sent, len - sent);
      if (w <= 0) return sent;
      sent += (size_t)w;
    }
    return sent;
  }

  void frameDone(DispatchFn dispatch, BinDispatchFn binDispatch) {
    const bool wasOverCap = _rxOverCap;
    _rxOverCap = false;
    _rx = RxHdr;
    const uint64_t plen = _payloadLen;
    _payloadLen = 0; _payloadFill = 0;
    switch (_op) {
      case 0x1:  // text
      case 0x2:  // binary
        if (!wasOverCap) {
          if (_op == 0x1 && dispatch) dispatch((const char *)_payload, (size_t)plen);
          if (_op == 0x2 && binDispatch) binDispatch(_payload, (size_t)plen);
        } else {
          Serial.printf("[ws] dropped %lluB frame (cap %u)\n",
                        (unsigned long long)plen, (unsigned)kPayloadCap);
        }
        break;
      case 0x8:  // close
        _open = false; _tcp.stop();
        Serial.println("[ws] server close");
        break;
      case 0x9: {  // ping → pong (masked, same payload)
        sendPong(_payload, (size_t)plen);
        break;
      }
      case 0xA:  // pong
      default:   // continuation/other frames dropped in v0.1
        break;
    }
  }

  void sendPong(const uint8_t *payload, size_t len) {
    if (!_open || len > 125) return;
    uint8_t f[132];
    f[0] = 0x8A; f[1] = 0x80 | (uint8_t)len;
    uint8_t mk[4];
    for (auto &m : mk) m = (uint8_t)(esp_random() & 0xFF);
    memcpy(f + 2, mk, 4);
    for (size_t i = 0; i < len; ++i) f[6 + i] = payload[i] ^ mk[i & 3];
    writeAll(f, 6 + len);
  }

  void drop(const char *why) { setDrop(why); _open = false; _tcp.stop(); }
  void setDrop(const char *why) { strlcpy(_lastDrop, why, sizeof(_lastDrop)); }

  void resetRx() {
    _rx = RxHdr; _hdrBytes = 0; _extFill = 0; _payloadLen = 0; _payloadFill = 0;
    _maskFill = 0; _rxOverCap = false;
  }

  NetworkClient _tcp;
  bool _open = false;
  char _lastDrop[24] = "none";

  RxState _rx = RxHdr;
  uint8_t _hdr[2] = {};     uint8_t _hdrBytes = 0;
  uint8_t _ext[8] = {};     uint8_t _extFill = 0, _extTotal = 0;
  uint8_t _maskFill = 0;
  uint64_t _payloadLen = 0; uint32_t _payloadFill = 0;
  bool _fin = false, _masked = false, _rxOverCap = false;
  uint8_t _op = 0;
  uint8_t *_payload = nullptr;  // allocated (heap_caps PSRAM) on first connect

  void ensurePayloadBuf() {
    if (_payload) return;
    _payload = static_cast<uint8_t *>(heap_caps_malloc(kPayloadCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_payload) _payload = static_cast<uint8_t *>(malloc(kPayloadCap));  // fall back to DRAM
  }
};