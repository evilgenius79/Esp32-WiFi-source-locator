// WiFi Source Locator - M5Stack Tab5 firmware.
//
// Polls the XIAO sensor nodes over I2C on Port A (Grove: SDA=G53, SCL=G54),
// reads an optional GPS receiver, feeds both to the app and draws the UI. All
// the interesting logic lives in app.cpp and ../common.
#include <M5Unified.h>
#include <Preferences.h>
#include <Wire.h>
#include <driver/gpio.h>

#include "app.h"

static constexpr uint32_t I2C_FREQ = 100000;   // several nodes on Grove cables: keep it gentle

static app::App A;
static M5Canvas canvas(&M5.Display);
static bool useCanvas = false;
static Preferences prefs;

// ---------------------------------------------------------------- I2C bus

static bool busWrite(uint8_t node, const uint8_t* cmd, size_t len) {
  uint8_t buf[16];
  memcpy(buf, cmd, len);
  buf[len] = wsl_crc8(buf, len);
  Wire.beginTransmission((uint8_t)(WSL_I2C_BASE + node));
  Wire.write(buf, len + 1);
  return Wire.endTransmission(true) == 0;
}

// Send a command and read back its fixed-size reply. Anything that does not
// carry the right magic, command echo and CRC is thrown away and retried once.
template <typename T> static bool busGet(uint8_t node, const uint8_t* cmd, size_t len, T& out) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!busWrite(node, cmd, len)) continue;
    delay(2);   // let the node build its reply
    const size_t got = Wire.requestFrom((uint8_t)(WSL_I2C_BASE + node), sizeof(T), true);
    const size_t n = Wire.readBytes((uint8_t*)&out, got < sizeof(T) ? got : sizeof(T));
    while (Wire.available()) Wire.read();
    if (n == sizeof(T) && wsl_check(out, cmd[0])) return true;
  }
  if (A.nodes[node].present) A.nodes[node].errors++;
  return false;
}

static int relayNode() {
  for (int i = 0; i < WSL_MAX_WIRED; i++)
    if (A.nodes[i].present) return i;
  return -1;
}

static WslModeArgs sentMode;   // what the nodes were last told (relay flag aside)

static void sendModeAll() {
  app::desiredMode(A, false, sentMode);
  const int relay = relayNode();
  for (int i = 0; i < WSL_MAX_WIRED; i++) {
    if (!A.nodes[i].present) continue;
    uint8_t cmd[1 + sizeof(WslModeArgs)] = {WSL_CMD_SET_MODE};
    WslModeArgs args;
    app::desiredMode(A, i == relay, args);
    memcpy(cmd + 1, &args, sizeof(args));
    busWrite(i, cmd, sizeof(cmd));
  }
}

// Once a second: look for nodes, drop dead ones, and (re)send everyone the
// current orders so a node that rebooted or was just plugged in falls in line.
static void discover(uint32_t now) {
  for (int i = 0; i < WSL_MAX_WIRED; i++) {
    app::Node& n = A.nodes[i];
    const uint8_t cmd[1] = {WSL_CMD_GET_STATUS};
    WslStatusResp st;
    if (busGet(i, cmd, 1, st) && st.proto == WSL_PROTO_VERSION) {
      n.fails = 0;
      app::nodePresence(A, i, true, st.chip, now);
    } else if (n.present && ++n.fails >= 3) {
      app::nodePresence(A, i, false, 0, now);
    }
  }
  sendModeAll();
}

static void pollTrack(uint32_t now) {
  const uint8_t cmd[1] = {WSL_CMD_GET_TRACK};
  for (int i = 0; i < WSL_MAX_WIRED; i++) {
    if (!A.nodes[i].present) continue;
    WslTrackResp r;
    if (!busGet(i, cmd, 1, r) || r.seq == A.nodes[i].lastSeq) continue;   // seq repeats = stale reply
    A.nodes[i].lastSeq = r.seq;
    if (r.channel == A.targetCh) app::feedTrack(A, i, r.count, r.sum, now);
  }
  const int relay = relayNode();
  if (relay < 0) return;
  const uint8_t rc[1] = {WSL_CMD_GET_REMOTE};
  WslRemoteResp rr;
  if (!busGet(relay, rc, 1, rr)) return;
  for (int k = 0; k < rr.n && k < WSL_REMOTE_PER_RESP; k++) {
    const WslRemoteEntry& e = rr.e[k];
    if (e.node_id < WSL_MAX_WIRED || e.node_id >= WSL_MAX_NODES) continue;
    app::Node& n = A.nodes[e.node_id];
    app::nodePresence(A, e.node_id, true, 0, now);
    if (e.seq == n.lastSeq) continue;   // duplicate
    n.lastSeq = e.seq;
    if (e.channel == A.targetCh) app::feedTrack(A, e.node_id, e.count, e.sum, now);
  }
}

// One page of one node's device table per call, round-robin.
static void pollScan(uint32_t now) {
  static uint8_t node = 0, page = 0;
  for (int tries = 0; tries < WSL_MAX_WIRED && !A.nodes[node].present; tries++) {
    node = (node + 1) % WSL_MAX_WIRED;
    page = 0;
  }
  if (!A.nodes[node].present) return;

  const uint8_t cmd[2] = {WSL_CMD_GET_DEVICES, page};
  static WslDevResp r;
  uint8_t pages = 1;
  if (busGet(node, cmd, 2, r) && r.page == page) {
    pages = r.pages;
    for (int k = 0; k < r.n && k < WSL_DEV_PER_PAGE; k++) app::mergeDevice(A, node, r.e[k], now);
  }

  // Fetch a couple of network names this node knows and we do not.
  int asked = 0;
  for (auto& d : A.devs) {
    if (asked >= 2) break;
    if (d.ssid[0] || !(d.ssidMask & (1 << node))) continue;
    uint8_t sc[7] = {WSL_CMD_GET_SSID};
    memcpy(sc + 1, d.mac, 6);
    WslSsidResp sr;
    if (busGet(node, sc, 7, sr) && memcmp(sr.mac, d.mac, 6) == 0) app::setSsid(A, sr.mac, sr.ssid, sr.len);
    asked++;
  }

  if (++page >= pages) {
    page = 0;
    node = (node + 1) % WSL_MAX_WIRED;
  }
}

// ---------------------------------------------------------------- GPS receiver
//
// Any receiver that sends NMEA (GGA, ideally RMC too) on a 3.3 V serial line
// works: wire its TX to one of the header pins below and share ground. Only
// that one wire is used; nothing is ever sent to the receiver. The pin and
// baud rate are found automatically and remembered. To pin them down instead,
// build with e.g.  -DGPS_RX_PIN=38 -DGPS_BAUD=115200

#ifndef GPS_RX_PIN
#define GPS_RX_PIN -1
#endif
#ifndef GPS_BAUD
#define GPS_BAUD 0
#endif

// The Tab5's free M5-Bus GPIOs (taken from M5Unified's pin table).
static const uint8_t GPS_PINS[] = {38, 7, 16, 17, 6, 37, 3, 4, 2, 48, 47, 35, 51, 45, 52, 18, 19, 5};
static const uint32_t GPS_BAUDS[] = {115200, 38400, 9600, 460800, 230400, 57600, 921600};
static constexpr int GPS_NPINS = sizeof(GPS_PINS), GPS_NBAUDS = sizeof(GPS_BAUDS) / sizeof(GPS_BAUDS[0]);

struct EdgeCount {
  uint8_t pin;
  volatile uint16_t count;
};
static EdgeCount gpsEdges[GPS_NPINS];

static void IRAM_ATTR onGpsEdge(void* arg) {
  EdgeCount* e = (EdgeCount*)arg;
  const uint16_t c = e->count + 1;
  e->count = c;
  if (c >= 400) gpio_intr_disable((gpio_num_t)e->pin);   // plenty: stop interrupting
}

enum GpsState : uint8_t { GPS_SAVED, GPS_LISTEN, GPS_PROBE, GPS_RUN, GPS_WAIT };
static GpsState gpsState = GPS_SAVED;
static wsl::NmeaParser nmea;
static uint32_t gpsT0 = 0, gpsLastGood = 0, gpsBaud = 0;
static int gpsPin = -1;
static uint8_t gpsActive[GPS_NPINS];   // pins with traffic, to be probed
static int gpsActiveN = 0, gpsPinIdx = 0, gpsBaudIdx = 0;

static void gpsOpen(int pin, uint32_t baud, uint32_t now) {
  Serial1.end();
  Serial1.setRxBufferSize(4096);
  Serial1.begin(baud, SERIAL_8N1, pin, -1);
  nmea.reset();
  gpsPin = pin;
  gpsBaud = baud;
  gpsT0 = now;
}

// Returns the parser's verdicts for everything received since the last call.
static int gpsPump(uint32_t now) {
  int best = wsl::NmeaParser::NONE;
  for (int budget = 4096; budget > 0 && Serial1.available(); budget--) {
    const int r = nmea.feed((char)Serial1.read());
    if (r == wsl::NmeaParser::NONE) continue;
    gpsLastGood = now;
    if (r > best) best = r;
    if (gpsState == GPS_RUN) app::gpsUpdate(A, nmea.fix, now);
  }
  return best;
}

static void gpsFound(uint32_t now) {
  char port[24];
  snprintf(port, sizeof(port), "G%d @ %lu", gpsPin, (unsigned long)gpsBaud);
  app::gpsStatus(A, true, port);
  gpsState = GPS_RUN;
  gpsLastGood = now;
  if (prefs.getChar("gpsPin", -1) != gpsPin || prefs.getUInt("gpsBaud", 0) != gpsBaud) {
    prefs.putChar("gpsPin", (int8_t)gpsPin);
    prefs.putUInt("gpsBaud", gpsBaud);
  }
}

static void gpsStartProbe(uint32_t now) {
  const uint32_t baud = GPS_BAUD ? (uint32_t)GPS_BAUD : GPS_BAUDS[gpsBaudIdx];
  gpsOpen(gpsActive[gpsPinIdx], baud, now);
  gpsState = GPS_PROBE;
  char msg[24];
  snprintf(msg, sizeof(msg), "trying G%d @ %lu", gpsPin, (unsigned long)baud);
  app::gpsStatus(A, false, msg);
}

// Watch every candidate pin for a moment: a receiver's TX line is the one that wiggles.
static void gpsStartListen(uint32_t now) {
  Serial1.end();
  gpsBaudIdx = 0;
  gpsPinIdx = 0;
  if (GPS_RX_PIN >= 0) {   // pin fixed at build time: nothing to look for
    gpsActive[0] = (uint8_t)GPS_RX_PIN;
    gpsActiveN = 1;
    gpsStartProbe(now);
    return;
  }
  for (int i = 0; i < GPS_NPINS; i++) {
    gpsEdges[i].pin = GPS_PINS[i];
    gpsEdges[i].count = 0;
    pinMode(GPS_PINS[i], INPUT_PULLUP);
    attachInterruptArg(GPS_PINS[i], onGpsEdge, &gpsEdges[i], FALLING);
  }
  gpsState = GPS_LISTEN;
  gpsT0 = now;
  app::gpsStatus(A, false, "searching...");
}

static void gpsLoop(uint32_t now) {
  switch (gpsState) {
    case GPS_SAVED: {   // first thing after boot: is it where it was last time?
      if (gpsT0 == 0) {
        const int pin = GPS_RX_PIN >= 0 ? GPS_RX_PIN : prefs.getChar("gpsPin", -1);
        const uint32_t baud = GPS_BAUD ? (uint32_t)GPS_BAUD : prefs.getUInt("gpsBaud", 0);
        if (pin < 0 || !baud) return gpsStartListen(now);
        gpsOpen(pin, baud, now ? now : 1);
        app::gpsStatus(A, false, "starting...");
      }
      gpsPump(now);
      if (nmea.good >= 2) gpsFound(now);
      else if (now - gpsT0 > 2500) gpsStartListen(now);
      break;
    }
    case GPS_LISTEN: {
      if (now - gpsT0 < 1500) break;
      gpsActiveN = 0;
      for (int i = 0; i < GPS_NPINS; i++) {
        detachInterrupt(GPS_PINS[i]);
        pinMode(GPS_PINS[i], INPUT);
        if (gpsEdges[i].count >= 20) gpsActive[gpsActiveN++] = GPS_PINS[i];
      }
      if (!gpsActiveN) {
        app::gpsStatus(A, false, "not found");
        gpsState = GPS_WAIT;
        gpsT0 = now;
      } else {
        gpsStartProbe(now);
      }
      break;
    }
    case GPS_PROBE: {
      gpsPump(now);
      if (nmea.good >= 2) return gpsFound(now);
      if (now - gpsT0 < 1500) break;
      if (!GPS_BAUD && ++gpsBaudIdx < GPS_NBAUDS) return gpsStartProbe(now);   // next speed
      gpsBaudIdx = 0;
      if (++gpsPinIdx < gpsActiveN) return gpsStartProbe(now);                 // next pin
      Serial1.end();
      char msg[24];
      snprintf(msg, sizeof(msg), "G%d: no NMEA", gpsPin);   // something talks there, but not in NMEA
      app::gpsStatus(A, false, msg);
      gpsState = GPS_WAIT;
      gpsT0 = now;
      break;
    }
    case GPS_RUN:
      gpsPump(now);
      if (now - gpsLastGood > 8000) {   // unplugged or switched off
        app::gpsStatus(A, false, "lost");
        gpsStartListen(now);
      }
      break;
    case GPS_WAIT:   // look again every so often, so a receiver can be plugged in later
      if (now - gpsT0 > 8000) gpsStartListen(now);
      break;
  }
}

// ---------------------------------------------------------------- settings

struct Saved {
  uint32_t magic;
  float cal[WSL_MAX_NODES], angle[WSL_MAX_NODES], mx[WSL_MAX_NODES], my[WSL_MAX_NODES];
  float nExp, mapSize;
  uint8_t mute, anglesCustom, useGps, originSet;
  double lat0, lon0;
};
static constexpr uint32_t SAVED_MAGIC = 0x57534C32;   // "WSL2"

static void loadPrefs() {
  Saved s;
  if (prefs.getBytes("cfg", &s, sizeof(s)) != sizeof(s) || s.magic != SAVED_MAGIC) return;
  for (int i = 0; i < WSL_MAX_NODES; i++) {
    A.nodes[i].cal = s.cal[i];
    A.nodes[i].angle = s.angle[i];
    A.nodes[i].mx = s.mx[i];
    A.nodes[i].my = s.my[i];
  }
  A.nExp = s.nExp;
  A.mapSize = s.mapSize;
  A.mute = s.mute;
  A.anglesCustom = s.anglesCustom;
  A.useGps = s.useGps;
  A.originSet = s.originSet;
  A.lat0 = s.lat0;
  A.lon0 = s.lon0;
}

static void savePrefs() {
  Saved s = {};
  s.magic = SAVED_MAGIC;
  for (int i = 0; i < WSL_MAX_NODES; i++) {
    s.cal[i] = A.nodes[i].cal;
    s.angle[i] = A.nodes[i].angle;
    s.mx[i] = A.nodes[i].mx;
    s.my[i] = A.nodes[i].my;
  }
  s.nExp = A.nExp;
  s.mapSize = A.mapSize;
  s.mute = A.mute;
  s.anglesCustom = A.anglesCustom;
  s.useGps = A.useGps;
  s.originSet = A.originSet;
  s.lat0 = A.lat0;
  s.lon0 = A.lon0;
  prefs.putBytes("cfg", &s, sizeof(s));
}

// ---------------------------------------------------------------- main

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  for (int rot : {3, 1, 0, 2}) {   // landscape, 1280 x 720
    M5.Display.setRotation(rot);
    if (M5.Display.width() > M5.Display.height()) break;
  }
  M5.Display.setBrightness(180);
  M5.Speaker.setVolume(96);

  // Port A (Grove): 5 V out for the nodes, I2C master on SDA=G53 / SCL=G54.
  M5.Power.setExtOutput(true, m5::ext_PA);
  Wire.setBufferSize(sizeof(WslDevResp) + 8);
  Wire.begin((int)M5.Ex_I2C.getSDA(), (int)M5.Ex_I2C.getSCL(), I2C_FREQ);
  Wire.setTimeOut(20);

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  useCanvas = canvas.createSprite(app::SCREEN_W, app::SCREEN_H) != nullptr;

  prefs.begin("wsl", false);
  loadPrefs();
}

void loop() {
  static uint32_t lastDiscover = 0, lastPoll = 0, lastDraw = 0, lastBeep = 0, dirtySince = 0;
  M5.update();
  uint32_t now = millis();

  const bool trk = app::tracking(A);
  if (now - lastDiscover >= 1000 || lastDiscover == 0) {
    lastDiscover = now;
    discover(now);
  } else {   // target, channel or mode changed: tell the nodes right away
    WslModeArgs want;
    app::desiredMode(A, false, want);
    if (memcmp(&want, &sentMode, sizeof(want)) != 0) sendModeAll();
  }

  if (now - lastPoll >= (trk ? 100u : 120u)) {
    lastPoll = now;
    if (trk) pollTrack(now);
    else pollScan(now);
  }

  now = millis();
  gpsLoop(now);
  const auto t = M5.Touch.getDetail();
  app::touch(A, M5.Touch.getCount() > 0 && t.isPressed(), t.x, t.y, now);
  app::tick(A, now);

  if (now - lastDraw >= 50) {
    lastDraw = now;
    if (useCanvas) {
      app::draw(A, canvas, now);
      canvas.pushSprite(0, 0);
    } else {
      M5.Display.startWrite();
      app::draw(A, M5.Display, now);
      M5.Display.endWrite();
    }
  }

  int freq = 0;
  const int interval = app::beepIntervalMs(A, &freq);
  if (interval && now - lastBeep >= (uint32_t)interval) {
    lastBeep = now;
    M5.Speaker.tone(freq, 40);
  }

  if (A.prefsDirty) {   // debounce flash writes
    if (!dirtySince) dirtySince = now;
    if (now - dirtySince > 2000 && !A.tDown) {
      savePrefs();
      A.prefsDirty = false;
      dirtySince = 0;
    }
  }
  delay(1);
}
