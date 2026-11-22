// WiFi Source Locator - wire protocol shared by the XIAO nodes and the Tab5.
//
// Wired nodes are I2C slaves at WSL_I2C_BASE + node id (0..7). The Tab5 is the
// master. Every exchange is two separate transactions:
//
//   1. master writes   [cmd][args...][crc8]
//   2. master reads    a fixed-size response for that cmd, ending in crc8
//
// Remote (wireless) nodes talk ESP-NOW broadcast to whichever wired node the
// Tab5 has flagged as relay; they show up as node ids 8..15.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define WSL_PROTO_VERSION 1
#define WSL_I2C_BASE      0x30
#define WSL_MAX_WIRED     8
#define WSL_MAX_NODES     16  // 0..7 wired, 8..15 remote
#define WSL_MAGIC         0xA5
#define WSL_DEV_PER_PAGE  16
#define WSL_REMOTE_PER_RESP 8
#define WSL_AGE_NEVER     0xFFFF

enum : uint8_t {
  WSL_CMD_SET_MODE    = 1,  // args: WslModeArgs
  WSL_CMD_GET_STATUS  = 2,  // -> WslStatusResp
  WSL_CMD_GET_TRACK   = 3,  // -> WslTrackResp (clears the accumulators)
  WSL_CMD_GET_DEVICES = 4,  // args: page -> WslDevResp
  WSL_CMD_GET_SSID    = 5,  // args: mac[6] -> WslSsidResp
  WSL_CMD_GET_REMOTE  = 6,  // -> WslRemoteResp (drains the relay queue)
};

enum : uint8_t { WSL_MODE_IDLE = 0, WSL_MODE_SCAN = 1, WSL_MODE_TRACK = 2 };

enum : uint8_t {
  WSL_KIND_UNKNOWN = 0,
  WSL_KIND_AP      = 1,
  WSL_KIND_STA     = 2,
  WSL_KIND_MASK    = 0x0F,
  WSL_KIND_HAS_SSID = 0x80,
};

enum : uint8_t { WSL_FLAG_RELAY = 0x01 };

enum : uint8_t { WSL_CHIP_OTHER = 0, WSL_CHIP_C3 = 1, WSL_CHIP_S3 = 2, WSL_CHIP_C6 = 3 };

#pragma pack(push, 1)

struct WslModeArgs {
  uint8_t mode;
  uint8_t channel;    // track channel (ignored while scanning)
  uint8_t mac[6];     // transmitter address to track
  uint8_t flags;      // WSL_FLAG_*
  uint8_t hop_10ms;   // scan dwell per channel, in 10 ms units
};

struct WslStatusResp {
  uint8_t magic, cmd;
  uint8_t proto, node_id, chip, mode, channel, flags;
  uint8_t dev_count;
  uint8_t remote_pending;
  uint16_t uptime_s;
  uint8_t crc;
};

struct WslTrackResp {
  uint8_t magic, cmd;
  uint8_t seq;
  uint8_t channel;
  uint16_t count;     // target frames heard since the previous GET_TRACK
  int32_t sum;        // sum of their RSSI values (dBm)
  int8_t max;
  uint16_t age_ms;    // time since the most recent target frame, WSL_AGE_NEVER if none
  uint8_t crc;
};

struct WslDevEntry {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t channel;
  uint8_t kind;       // WSL_KIND_* | WSL_KIND_HAS_SSID
  uint8_t age_s;
  uint16_t count;
};

struct WslDevResp {
  uint8_t magic, cmd;
  uint8_t page, pages, n;
  WslDevEntry e[WSL_DEV_PER_PAGE];
  uint8_t crc;
};

struct WslSsidResp {
  uint8_t magic, cmd;
  uint8_t mac[6];
  uint8_t len;        // 0 = unknown / hidden
  char ssid[32];
  uint8_t crc;
};

struct WslRemoteEntry {
  uint8_t node_id;    // 8..15
  uint8_t seq;
  uint8_t channel;
  int8_t max;
  uint16_t count;
  int32_t sum;
};

struct WslRemoteResp {
  uint8_t magic, cmd;
  uint8_t n;
  WslRemoteEntry e[WSL_REMOTE_PER_RESP];
  uint8_t crc;
};

// ESP-NOW payloads (broadcast).
#define WSL_NOW_MAGIC "WSL1"
enum : uint8_t { WSL_NOW_BEACON = 1, WSL_NOW_REPORT = 2 };

struct WslNowBeacon {   // relay node -> remote nodes, ~4 Hz while tracking
  char magic[4];
  uint8_t type;
  uint8_t mode;
  uint8_t channel;
  uint8_t mac[6];
};

struct WslNowReport {   // remote node -> relay node, ~5 Hz while tracking
  char magic[4];
  uint8_t type;
  WslRemoteEntry e;
};

#pragma pack(pop)

static inline uint8_t wsl_crc8(const uint8_t* d, size_t n) {
  uint8_t c = 0x5A;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

// Fill in / verify the trailing crc of a response struct.
template <typename T> static inline void wsl_seal(T& r) {
  r.crc = wsl_crc8((const uint8_t*)&r, sizeof(T) - 1);
}
template <typename T> static inline bool wsl_check(const T& r, uint8_t cmd) {
  return r.magic == WSL_MAGIC && r.cmd == cmd &&
         r.crc == wsl_crc8((const uint8_t*)&r, sizeof(T) - 1);
}
