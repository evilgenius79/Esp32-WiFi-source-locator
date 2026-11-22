// WiFi Source Locator - minimal 802.11 header parser: who transmitted this
// frame, and what kind of device is it? Plain C++ so it is tested on a PC.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "wsl_proto.h"

struct WslFrameInfo {
  const uint8_t* ta = nullptr;   // transmitter address (6 bytes)
  uint8_t kind = WSL_KIND_UNKNOWN;
  const uint8_t* ssid = nullptr; // beacons / probe responses only
  uint8_t ssidLen = 0;
  uint8_t apChannel = 0;         // channel the AP says it is on, 0 if not stated
};

// p/len: the frame as delivered by the ESP32 promiscuous callback, where len
// (rx_ctrl.sig_len) includes the 4-byte FCS. Returns false for frames that do
// not identify their transmitter (ACK, CTS...) or that are our own ESP-NOW.
static inline bool wsl_parse_frame(const uint8_t* p, int len, WslFrameInfo& out) {
  out = WslFrameInfo();
  if (len < 16) return false;
  const uint8_t ftype = (p[0] >> 2) & 3;
  const uint8_t subtype = (p[0] >> 4) & 0xF;
  const bool fromDS = p[1] & 0x02;

  if (ftype == 0) {                         // management
    if (len < 24) return false;
    if (subtype == 13 && len >= 28 && p[24] == 127 && p[25] == 0x18 && p[26] == 0xFE && p[27] == 0x34)
      return false;                         // Espressif vendor action frame = ESP-NOW
    if (subtype == 8 || subtype == 5) {     // beacon / probe response
      out.kind = WSL_KIND_AP;
      const int end = len - 4;              // stop before the FCS
      int i = 36;                           // 24 header + 12 fixed fields
      while (i + 2 <= end) {
        const uint8_t id = p[i], l = p[i + 1];
        if (i + 2 + l > end) break;
        if (id == 0 && l <= 32) {
          out.ssid = p + i + 2;
          out.ssidLen = l;
        } else if (id == 3 && l == 1) {
          out.apChannel = p[i + 2];
        }
        i += 2 + l;
      }
      if (out.ssidLen && out.ssid[0] == 0) out.ssidLen = 0;   // hidden network: zero-filled name
    } else if (subtype == 4 || subtype == 0 || subtype == 2) {
      out.kind = WSL_KIND_STA;              // probe / (re)association request
    }
  } else if (ftype == 1) {                  // control: only some carry a transmitter address
    if (subtype < 8 || subtype > 11) return false;   // keep BlockAckReq, BlockAck, PS-Poll, RTS
  } else if (ftype == 2) {                  // data
    if (len < 24) return false;
    out.kind = fromDS ? WSL_KIND_AP : WSL_KIND_STA;
  } else {
    return false;
  }
  out.ta = p + 10;
  return !(out.ta[0] & 0x01);               // a group address is never a real transmitter
}
