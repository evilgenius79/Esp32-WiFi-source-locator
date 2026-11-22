// WiFi Source Locator - sensor node (Seeed XIAO ESP32-C3 / S3 / C6)
//
// Passively sniffs 802.11 frames and reports who is transmitting and how
// strongly. It never joins or talks to the networks it listens to; the only
// thing it ever transmits is small ESP-NOW broadcasts between a relay node and
// remote nodes, and only when remote nodes are in use.
//
// Wired build:  I2C slave on the XIAO's SDA/SCL pins (D4/D5), polled by the Tab5.
// Remote build (-DNODE_REMOTE=1): no wires; takes its orders from, and reports
// to, a wired relay node over ESP-NOW.
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "wsl_frame.h"
#include "wsl_proto.h"

#ifndef NODE_ID
#define NODE_ID 0
#endif
#ifndef NODE_REMOTE
#define NODE_REMOTE 0
#endif

#define MAX_DEVICES   96
#define DEV_STALE_MS  120000u
#define MAX_CHANNEL   13

struct Device {
  bool used;
  uint8_t mac[6];
  int8_t rssi;
  int8_t bestRssi;
  uint8_t channel;
  uint8_t kind;
  uint8_t ssidLen;
  char ssid[32];
  uint16_t count;
  uint32_t lastMs;
};

static Device devices[MAX_DEVICES];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static Preferences prefs;

static uint8_t nodeId = NODE_ID;          // 0..7 (remote nodes report as 8 + nodeId)
static volatile uint8_t curChannel = 1;

// Requested configuration (written from I2C / ESP-NOW context, applied in loop()).
static WslModeArgs wantCfg = {WSL_MODE_SCAN, 1, {0}, 0, 25};
static volatile bool cfgDirty = true;

// Active configuration.
static uint8_t mode = WSL_MODE_IDLE;
static uint8_t flags = 0;
static uint8_t target[6];
static uint32_t hopMs = 250;
static uint32_t lastHopMs = 0;

// Target accumulators (guarded by mux).
static uint16_t trkCount = 0;
static int32_t trkSum = 0;
static int8_t trkMax = -128;
static uint32_t trkLastMs = 0;
static bool trkEver = false;
static uint8_t trkSeq = 0;

// Reports relayed from remote nodes (guarded by mux).
#define RQ_SIZE 16
static WslRemoteEntry rq[RQ_SIZE];
static uint8_t rqHead = 0, rqCount = 0;

// Prepared I2C response.
static uint8_t resp[sizeof(WslDevResp)];
static size_t respLen = 0;

static uint32_t lastBeaconMs = 0;   // relay: last beacon sent; remote: last beacon heard
static bool beaconHeard = false;

// ---------------------------------------------------------------- sniffer

static Device* findOrAdd(const uint8_t* mac, uint32_t now) {
  Device* freeSlot = nullptr;
  Device* oldest = &devices[0];
  for (auto& d : devices) {
    if (d.used) {
      if (memcmp(d.mac, mac, 6) == 0) return &d;
      if (now - d.lastMs > now - oldest->lastMs) oldest = &d;
    } else if (!freeSlot) {
      freeSlot = &d;
    }
  }
  Device* d = freeSlot ? freeSlot : oldest;
  memset(d, 0, sizeof(*d));
  d->used = true;
  memcpy(d->mac, mac, 6);
  d->bestRssi = -128;
  return d;
}

static void onSniff(void* buf, wifi_promiscuous_pkt_type_t) {
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const int8_t rssi = pkt->rx_ctrl.rssi;
  WslFrameInfo f;
  if (!wsl_parse_frame(pkt->payload, pkt->rx_ctrl.sig_len, f)) return;
  const uint8_t* ta = f.ta;
  const uint8_t kind = f.kind, apChannel = f.apChannel, ssidLen = f.ssidLen;
  const uint8_t* ssid = f.ssid;

  const uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  if (mode == WSL_MODE_TRACK && memcmp(ta, target, 6) == 0) {
    trkCount++;
    trkSum += rssi;
    if (rssi > trkMax) trkMax = rssi;
    trkLastMs = now;
    trkEver = true;
  }
  Device* d = findOrAdd(ta, now);
  d->rssi = d->count ? (int8_t)((d->rssi * 3 + rssi) / 4) : rssi;
  if (d->count < 0xFFFF) d->count++;
  d->lastMs = now;
  if (kind != WSL_KIND_UNKNOWN) d->kind = kind;
  if (apChannel >= 1 && apChannel <= 14) {
    d->channel = apChannel;                 // the AP says where it lives
    d->bestRssi = 127;
  } else if (rssi >= d->bestRssi) {         // otherwise: the channel we hear it loudest on
    d->bestRssi = rssi;
    d->channel = curChannel;
  } else if (d->channel != curChannel && d->bestRssi > -127) {
    d->bestRssi--;                          // let a device that changed channel win eventually
  }
  if (ssidLen) {
    d->ssidLen = ssidLen;
    memcpy(d->ssid, ssid, ssidLen);
  }
  portEXIT_CRITICAL(&mux);
}

static void setChannel(uint8_t ch) {
  if (ch < 1 || ch > MAX_CHANNEL) ch = 1;
  curChannel = ch;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static void resetTrack() {
  portENTER_CRITICAL(&mux);
  trkCount = 0; trkSum = 0; trkMax = -128; trkEver = false;
  portEXIT_CRITICAL(&mux);
}

static void applyCfg() {
  WslModeArgs c;
  portENTER_CRITICAL(&mux);
  c = wantCfg;
  cfgDirty = false;
  portEXIT_CRITICAL(&mux);

  const bool newTarget = memcmp(target, c.mac, 6) != 0;
  const bool changed = c.mode != mode || (c.mode == WSL_MODE_TRACK && (newTarget || c.channel != curChannel));
  flags = c.flags;
  hopMs = c.hop_10ms ? c.hop_10ms * 10u : 250u;
  if (!changed) return;

  portENTER_CRITICAL(&mux);
  memcpy(target, c.mac, 6);
  mode = c.mode;
  portEXIT_CRITICAL(&mux);
  if (mode == WSL_MODE_TRACK) {
    setChannel(c.channel);
    resetTrack();
  } else if (mode == WSL_MODE_SCAN) {
    setChannel(1 + (nodeId * 3) % MAX_CHANNEL);   // stagger nodes across the band
    lastHopMs = millis();
  }
}

// ---------------------------------------------------------------- ESP-NOW

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void onNow(const esp_now_recv_info_t*, const uint8_t* data, int len) {
  if (len < 5 || memcmp(data, WSL_NOW_MAGIC, 4) != 0) return;
#if NODE_REMOTE
  if (data[4] == WSL_NOW_BEACON && len == (int)sizeof(WslNowBeacon)) {
    const WslNowBeacon* b = (const WslNowBeacon*)data;
    portENTER_CRITICAL(&mux);
    wantCfg.mode = b->mode;
    wantCfg.channel = b->channel;
    memcpy(wantCfg.mac, b->mac, 6);
    cfgDirty = true;
    portEXIT_CRITICAL(&mux);
    lastBeaconMs = millis();
    beaconHeard = true;
  }
#else
  if (data[4] == WSL_NOW_REPORT && len == (int)sizeof(WslNowReport)) {
    const WslNowReport* r = (const WslNowReport*)data;
    portENTER_CRITICAL(&mux);
    rq[(rqHead + rqCount) % RQ_SIZE] = r->e;
    if (rqCount < RQ_SIZE) rqCount++;
    else rqHead = (rqHead + 1) % RQ_SIZE;   // full: drop the oldest
    portEXIT_CRITICAL(&mux);
  }
#endif
}

static void nowInit() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init failed");
    return;
  }
  esp_now_register_recv_cb(onNow);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;               // "whatever channel we are on"
  peer.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&peer);
}

// ---------------------------------------------------------------- I2C slave

#if !NODE_REMOTE
template <typename T> static T* newResp(uint8_t cmd) {
  memset(resp, 0, sizeof(resp));
  resp[0] = WSL_MAGIC;
  resp[1] = cmd;
  respLen = sizeof(T);
  return (T*)resp;
}

static void onI2cReceive(int n) {
  uint8_t in[16];
  int len = 0;
  while (Wire.available()) {
    const uint8_t b = Wire.read();
    if (len < (int)sizeof(in)) in[len++] = b;
  }
  (void)n;
  if (len < 2 || wsl_crc8(in, len - 1) != in[len - 1]) return;
  const uint32_t now = millis();

  switch (in[0]) {
    case WSL_CMD_SET_MODE:
      if (len == 1 + (int)sizeof(WslModeArgs) + 1) {
        portENTER_CRITICAL(&mux);
        memcpy(&wantCfg, in + 1, sizeof(WslModeArgs));
        cfgDirty = true;
        portEXIT_CRITICAL(&mux);
      }
      break;

    case WSL_CMD_GET_STATUS: {
      auto* r = newResp<WslStatusResp>(in[0]);
      r->proto = WSL_PROTO_VERSION;
      r->node_id = nodeId;
#if CONFIG_IDF_TARGET_ESP32C3
      r->chip = WSL_CHIP_C3;
#elif CONFIG_IDF_TARGET_ESP32S3
      r->chip = WSL_CHIP_S3;
#elif CONFIG_IDF_TARGET_ESP32C6
      r->chip = WSL_CHIP_C6;
#endif
      r->mode = mode;
      r->channel = curChannel;
      r->flags = flags;
      uint8_t cnt = 0;
      for (auto& d : devices) cnt += d.used && now - d.lastMs < DEV_STALE_MS;
      r->dev_count = cnt;
      r->remote_pending = rqCount;
      r->uptime_s = (uint16_t)(now / 1000);
      wsl_seal(*r);
      break;
    }

    case WSL_CMD_GET_TRACK: {
      auto* r = newResp<WslTrackResp>(in[0]);
      portENTER_CRITICAL(&mux);
      r->seq = ++trkSeq;
      r->channel = curChannel;
      r->count = trkCount;
      r->sum = trkSum;
      r->max = trkMax;
      const uint32_t age = now - trkLastMs;
      r->age_ms = !trkEver ? WSL_AGE_NEVER : age > 0xFFFE ? 0xFFFE : (uint16_t)age;
      trkCount = 0; trkSum = 0; trkMax = -128;
      portEXIT_CRITICAL(&mux);
      wsl_seal(*r);
      break;
    }

    case WSL_CMD_GET_DEVICES: {
      auto* r = newResp<WslDevResp>(in[0]);
      r->page = in[1];
      r->pages = MAX_DEVICES / WSL_DEV_PER_PAGE;
      portENTER_CRITICAL(&mux);
      for (int i = 0; i < WSL_DEV_PER_PAGE; i++) {
        const int slot = in[1] * WSL_DEV_PER_PAGE + i;
        if (slot >= MAX_DEVICES) break;
        const Device& d = devices[slot];
        if (!d.used || now - d.lastMs >= DEV_STALE_MS) continue;
        WslDevEntry& e = r->e[r->n++];
        memcpy(e.mac, d.mac, 6);
        e.rssi = d.rssi;
        e.channel = d.channel;
        e.kind = d.kind | (d.ssidLen ? WSL_KIND_HAS_SSID : 0);
        e.age_s = (uint8_t)((now - d.lastMs) / 1000);
        e.count = d.count;
      }
      portEXIT_CRITICAL(&mux);
      wsl_seal(*r);
      break;
    }

    case WSL_CMD_GET_SSID: {
      if (len != 8) break;
      auto* r = newResp<WslSsidResp>(in[0]);
      memcpy(r->mac, in + 1, 6);
      portENTER_CRITICAL(&mux);
      for (auto& d : devices) {
        if (d.used && memcmp(d.mac, in + 1, 6) == 0) {
          r->len = d.ssidLen;
          memcpy(r->ssid, d.ssid, d.ssidLen);
          break;
        }
      }
      portEXIT_CRITICAL(&mux);
      wsl_seal(*r);
      break;
    }

    case WSL_CMD_GET_REMOTE: {
      auto* r = newResp<WslRemoteResp>(in[0]);
      portENTER_CRITICAL(&mux);
      while (rqCount && r->n < WSL_REMOTE_PER_RESP) {
        r->e[r->n++] = rq[rqHead];
        rqHead = (rqHead + 1) % RQ_SIZE;
        rqCount--;
      }
      portEXIT_CRITICAL(&mux);
      wsl_seal(*r);
      break;
    }
  }
}

static void onI2cRequest() {
  // A reply is served once: if the master reads before the next command has
  // been processed it gets a byte that fails its check, never stale data.
  if (respLen) Wire.write(resp, respLen);
  else Wire.write((uint8_t)0);
  respLen = 0;
}
#endif

// ---------------------------------------------------------------- serial console

static void printInfo() {
  Serial.printf("WiFi Source Locator node  role=%s id=%u", NODE_REMOTE ? "remote" : "wired", nodeId);
#if !NODE_REMOTE
  Serial.printf(" i2c=0x%02X sda=%d scl=%d", WSL_I2C_BASE + nodeId, SDA, SCL);
#endif
  uint8_t cnt = 0;
  for (auto& d : devices) cnt += d.used;
  Serial.printf(" mode=%u ch=%u devices=%u\n", mode, curChannel, cnt);
  Serial.println("commands:  id <0-7>   info");
}

static void pollSerial() {
  static char line[32];
  static uint8_t n = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (strncmp(line, "id ", 3) == 0) {
        const int v = atoi(line + 3);
        if (v >= 0 && v < WSL_MAX_WIRED) {
          prefs.putUChar("id", (uint8_t)v);
          Serial.printf("node id set to %d, restarting\n", v);
          delay(100);
          ESP.restart();
        } else {
          Serial.println("id must be 0..7");
        }
      } else if (n) {
        printInfo();
      }
      n = 0;
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  prefs.begin("wsl", false);
  nodeId = prefs.getUChar("id", NODE_ID) % WSL_MAX_WIRED;

#if defined(USE_EXT_ANTENNA) && CONFIG_IDF_TARGET_ESP32C6
  // XIAO ESP32-C6: route the radio to the U.FL connector instead of the ceramic antenna.
  pinMode(WIFI_ENABLE, OUTPUT);
  digitalWrite(WIFI_ENABLE, LOW);
  pinMode(WIFI_ANT_CONFIG, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG, HIGH);
#endif

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  wifi_country_t country = {"01", 1, MAX_CHANNEL, 20, WIFI_COUNTRY_POLICY_MANUAL};
  esp_wifi_set_country(&country);

  wifi_promiscuous_filter_t filt = {WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
                                    WIFI_PROMIS_FILTER_MASK_CTRL};
  esp_wifi_set_promiscuous_filter(&filt);
  wifi_promiscuous_filter_t ctrl = {WIFI_PROMIS_CTRL_FILTER_MASK_ALL};
  esp_wifi_set_promiscuous_ctrl_filter(&ctrl);
  esp_wifi_set_promiscuous_rx_cb(onSniff);
  esp_wifi_set_promiscuous(true);
  nowInit();

#if NODE_REMOTE
  wantCfg.mode = WSL_MODE_IDLE;
#else
  Wire.setBufferSize(sizeof(WslDevResp) + 8);
  Wire.onReceive(onI2cReceive);
  Wire.onRequest(onI2cRequest);
  Wire.begin((uint8_t)(WSL_I2C_BASE + nodeId), SDA, SCL, 0);
#endif
  printInfo();
}

void loop() {
  const uint32_t now = millis();
  pollSerial();
  if (cfgDirty) applyCfg();

#if NODE_REMOTE
  static uint32_t lastReportMs = 0;
  static uint8_t seq = 0;
  const bool linked = beaconHeard && now - lastBeaconMs < 5000;
  if (!linked) {
    // No relay in earshot: walk the band until we hear its beacon.
    if (now - lastHopMs >= 350) {
      lastHopMs = now;
      mode = WSL_MODE_IDLE;
      setChannel(curChannel % MAX_CHANNEL + 1);
    }
  } else if (mode == WSL_MODE_TRACK && now - lastReportMs >= 200) {
    lastReportMs = now;
    WslNowReport r;
    memcpy(r.magic, WSL_NOW_MAGIC, 4);
    r.type = WSL_NOW_REPORT;
    portENTER_CRITICAL(&mux);
    r.e.node_id = WSL_MAX_WIRED + nodeId;
    r.e.seq = ++seq;
    r.e.channel = curChannel;
    r.e.max = trkMax;
    r.e.count = trkCount;
    r.e.sum = trkSum;
    trkCount = 0; trkSum = 0; trkMax = -128;
    portEXIT_CRITICAL(&mux);
    esp_now_send(BROADCAST, (const uint8_t*)&r, sizeof(r));
  }
#else
  if (mode == WSL_MODE_SCAN && now - lastHopMs >= hopMs) {
    lastHopMs = now;
    setChannel(curChannel % MAX_CHANNEL + 1);
  }
  if ((flags & WSL_FLAG_RELAY) && mode == WSL_MODE_TRACK && now - lastBeaconMs >= 250) {
    lastBeaconMs = now;
    WslNowBeacon b;
    memcpy(b.magic, WSL_NOW_MAGIC, 4);
    b.type = WSL_NOW_BEACON;
    b.mode = mode;
    b.channel = curChannel;
    memcpy(b.mac, target, 6);
    esp_now_send(BROADCAST, (const uint8_t*)&b, sizeof(b));
  }
#endif
  delay(1);
}
