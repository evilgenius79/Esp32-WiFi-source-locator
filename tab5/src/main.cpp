// WiFi Source Locator - M5Stack Tab5 firmware.
//
// Polls the XIAO sensor nodes over I2C on Port A (Grove: SDA=G53, SCL=G54),
// feeds the readings to the app and draws the UI. All the interesting logic
// lives in app.cpp and ../common/wsl_locate.h.
#include <M5Unified.h>
#include <Preferences.h>
#include <Wire.h>

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

// ---------------------------------------------------------------- settings

struct Saved {
  uint32_t magic;
  float cal[WSL_MAX_NODES], angle[WSL_MAX_NODES], mx[WSL_MAX_NODES], my[WSL_MAX_NODES];
  float nExp, mapSize;
  uint8_t mute, anglesCustom;
};
static constexpr uint32_t SAVED_MAGIC = 0x57534C31;   // "WSL1"

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
