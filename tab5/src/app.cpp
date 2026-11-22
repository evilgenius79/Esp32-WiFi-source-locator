#include "app.h"

#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace app {

using lgfx::LovyanGFX;
using lgfx::textdatum_t;

// ---------------------------------------------------------------- look

static constexpr uint32_t C_BG = 0x0E1116u, C_PANEL = 0x1A202Au, C_PANEL2 = 0x232B38u, C_LINE = 0x2F3A48u;
static constexpr uint32_t C_TEXT = 0xE6EDF3u, C_DIM = 0x8B98A9u, C_ACCENT = 0x35C2FFu;
static constexpr uint32_t C_GOOD = 0x3DDC84u, C_WARN = 0xFFB020u, C_BAD = 0xFF5A3Cu, C_BLACK = 0x000000u;

static const lgfx::IFont* const F18 = &fonts::DejaVu18;
static const lgfx::IFont* const F24 = &fonts::DejaVu24;
static const lgfx::IFont* const F40 = &fonts::DejaVu40;
static const lgfx::IFont* const F72 = &fonts::DejaVu72;

constexpr int TOP_H = 64;
constexpr int LIST_Y = 136, ROW_H = 60, FOOT_H = 96;
constexpr int MAP_X = 16, MAP_Y = 80, MAP_S = 624;
constexpr int CX = 350, CY = 400, CR = 262;

enum {
  A_TAB = 0,
  A_FILTER = 10,
  A_CLEAR = 20, A_HUNT, A_MAP, A_STOP, A_CAL, A_MUTE, A_CH_DN, A_CH_UP,
  A_N_DN, A_N_UP, A_AREA, A_UNDO, A_CLRPTS, A_EVEN, A_RESETCAL,
  A_ANG_DN = 100,
  A_ANG_UP = 120,
  A_ROW = 1000,
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float level(float rssi) { return clamp01((rssi + 95.0f) / 60.0f); }

static uint32_t blend(uint32_t a, uint32_t b, float t) {
  t = clamp01(t);
  uint32_t out = 0;
  for (int s = 16; s >= 0; s -= 8) {
    const float ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
    out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
  }
  return out;
}

// cold blue -> green -> yellow -> hot red
static uint32_t heatColor(float t) {
  t = clamp01(t);
  if (t < 0.35f) return blend(0x2D6CDFu, 0x3DDC84u, t / 0.35f);
  if (t < 0.65f) return blend(0x3DDC84u, 0xFFD23Cu, (t - 0.35f) / 0.30f);
  return blend(0xFFD23Cu, 0xFF4B2Bu, (t - 0.65f) / 0.35f);
}

static void txt(LovyanGFX& g, const char* s, int x, int y, const lgfx::IFont* f, uint32_t c,
                textdatum_t d = textdatum_t::top_left) {
  g.setFont(f);
  g.setTextColor(c);
  g.setTextDatum(d);
  g.drawString(s, x, y);
}

static void txtf(LovyanGFX& g, int x, int y, const lgfx::IFont* f, uint32_t c, textdatum_t d, const char* fmt, ...)
    __attribute__((format(printf, 7, 8)));
static void txtf(LovyanGFX& g, int x, int y, const lgfx::IFont* f, uint32_t c, textdatum_t d, const char* fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  txt(g, buf, x, y, f, c, d);
}

static void hit(App& a, int x, int y, int w, int h, int id) {
  a.btns.push_back({(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, id});
}

static void button(App& a, LovyanGFX& g, int x, int y, int w, int h, const char* label, int id,
                   bool active = false, uint32_t activeColor = C_ACCENT) {
  g.fillRoundRect(x, y, w, h, 10, active ? activeColor : C_PANEL2);
  if (!active) g.drawRoundRect(x, y, w, h, 10, C_LINE);
  txt(g, label, x + w / 2, y + h / 2, F24, active ? C_BLACK : C_TEXT, textdatum_t::middle_center);
  hit(a, x, y, w, h, id);
}

static void toast(App& a, uint32_t now, const char* msg) {
  snprintf(a.toast, sizeof(a.toast), "%s", msg);
  a.toastUntil = now + 2500;
}

static void macStr(const uint8_t* m, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static const char* kindLabel(uint8_t k) {
  return k == WSL_KIND_AP ? "AP" : k == WSL_KIND_STA ? "CLIENT" : "?";
}

static void devName(const Dev& d, char* out, size_t n) {
  if (d.ssid[0]) snprintf(out, n, "%s", d.ssid);
  else if (d.kind == WSL_KIND_AP) snprintf(out, n, "(hidden network)");
  else if (d.kind == WSL_KIND_STA) snprintf(out, n, (d.mac[0] & 0x02) ? "Client (private MAC)" : "Client device");
  else snprintf(out, n, "Unknown device");
}

// ---------------------------------------------------------------- state

App::App() {
  for (auto& h : hist) h = -128;
  devs.reserve(MAX_DEVS);
  order.reserve(MAX_DEVS);
  survey.reserve(MAX_SURVEY);
}

static Dev* findDev(App& a, const uint8_t* mac) {
  for (auto& d : a.devs)
    if (memcmp(d.mac, mac, 6) == 0) return &d;
  return nullptr;
}

static void evenAngles(App& a) {
  int n = 0;
  for (int i = 0; i < WSL_MAX_WIRED; i++) n += a.nodes[i].present;
  int k = 0;
  for (int i = 0; i < WSL_MAX_WIRED; i++)
    if (a.nodes[i].present) a.nodes[i].angle = k++ * 360.0f / n;
}

void nodePresence(App& a, int node, bool present, uint8_t chip, uint32_t now) {
  if (node < 0 || node >= WSL_MAX_NODES) return;
  Node& n = a.nodes[node];
  if (present) {
    n.lastOkMs = now;
    n.chip = chip;
  }
  if (n.present == present) return;
  n.present = present;
  n.filt.reset();
  if (node < WSL_MAX_WIRED) {
    if (!a.anglesCustom) evenAngles(a);
  } else if (present && n.mx == 0 && n.my == 0) {
    const int k = node - WSL_MAX_WIRED;   // park new remote nodes near the corners
    n.mx = a.mapSize * (0.12f + 0.76f * (k & 1) + 0.1f * (k >> 2));
    n.my = a.mapSize * (0.12f + 0.76f * ((k >> 1) & 1));
  }
}

void mergeDevice(App& a, int node, const WslDevEntry& e, uint32_t now) {
  if (node < 0 || node >= WSL_MAX_WIRED || e.age_s > 60) return;
  Dev* d = findDev(a, e.mac);
  if (!d) {
    if ((int)a.devs.size() >= MAX_DEVS) return;
    a.devs.emplace_back();
    d = &a.devs.back();
    memcpy(d->mac, e.mac, 6);
    for (auto& r : d->rssi) r = -128;
    for (auto& s : d->seenMs) s = 0;
  }
  const uint8_t kind = e.kind & WSL_KIND_MASK;
  if (kind == WSL_KIND_AP || d->kind == WSL_KIND_UNKNOWN) {
    if (kind != WSL_KIND_UNKNOWN) d->kind = kind;
  }
  if (e.channel && (kind == WSL_KIND_AP || !d->channel || e.rssi >= d->best - 3)) d->channel = e.channel;
  if (e.kind & WSL_KIND_HAS_SSID) d->ssidMask |= 1 << node;
  d->rssi[node] = e.rssi;
  const uint32_t age = e.age_s * 1000u;
  d->seenMs[node] = now > age ? now - age : 1;
  if (e.rssi > d->best) d->best = e.rssi;
}

void setSsid(App& a, const uint8_t* mac, const char* ssid, int len) {
  Dev* d = findDev(a, mac);
  if (!d) return;
  d->ssidMask = 0;   // asked; do not ask again
  if (len <= 0 || len > 32) return;
  int o = 0;
  for (int i = 0; i < len; i++) d->ssid[o++] = (ssid[i] >= 32 && ssid[i] < 127) ? ssid[i] : '?';
  d->ssid[o] = 0;
  if (a.hasTarget && memcmp(a.target, mac, 6) == 0) snprintf(a.targetName, sizeof(a.targetName), "%s", d->ssid);
}

void feedTrack(App& a, int node, uint32_t count, int32_t sum, uint32_t now) {
  if (node < 0 || node >= WSL_MAX_NODES) return;
  a.nodes[node].filt.update(count, sum, now);
}

static void resetTrackState(App& a) {
  for (auto& n : a.nodes) n.filt.reset();
  for (auto& h : a.hist) h = -128;
  a.bvx = a.bvy = 0;
  a.bearing = wsl::Bearing();
  a.headLive = false;
  a.calRunning = false;
}

void setTarget(App& a, const Dev& d) {
  a.hasTarget = true;
  memcpy(a.target, d.mac, 6);
  a.targetCh = d.channel >= 1 && d.channel <= 13 ? d.channel : 1;
  a.targetKind = d.kind;
  devName(d, a.targetName, sizeof(a.targetName));
  a.survey.clear();
  a.mapRes = wsl::MapResult();
  a.heatValid = false;
  resetTrackState(a);
}

bool tracking(const App& a) { return a.hasTarget && a.screen != SCR_SCAN; }

void desiredMode(const App& a, bool relay, WslModeArgs& out) {
  memset(&out, 0, sizeof(out));
  out.mode = tracking(a) ? WSL_MODE_TRACK : WSL_MODE_SCAN;
  out.channel = a.targetCh;
  memcpy(out.mac, a.target, 6);
  out.flags = relay ? WSL_FLAG_RELAY : 0;
  out.hop_10ms = 25;
}

int beepIntervalMs(const App& a, int* freqOut) {
  if (a.screen != SCR_HUNT || a.mute || !a.hasTarget || !a.headLive) return 0;
  const float l = level(a.headRssi);
  if (freqOut) *freqOut = 700 + (int)(1500 * l);
  return 900 - (int)(800 * l);
}

static void sortDevices(App& a, uint32_t now) {
  // Forget devices nobody has heard for a while (but never the ones in use).
  for (size_t i = 0; i < a.devs.size();) {
    Dev& d = a.devs[i];
    d.best = -128;
    d.lastMs = 0;
    for (int n = 0; n < WSL_MAX_WIRED; n++) {
      if (!d.seenMs[n]) continue;
      if (d.seenMs[n] > d.lastMs) d.lastMs = d.seenMs[n];
      if (now - d.seenMs[n] < 20000 && d.rssi[n] > d.best) d.best = d.rssi[n];
    }
    const bool keep = (a.hasSel && !memcmp(a.selMac, d.mac, 6)) || (a.hasTarget && !memcmp(a.target, d.mac, 6));
    if (!keep && now - d.lastMs > 90000) {
      d = a.devs.back();
      a.devs.pop_back();
    } else {
      i++;
    }
  }
  a.order.clear();
  for (size_t i = 0; i < a.devs.size(); i++) {
    const Dev& d = a.devs[i];
    if (a.filter == 1 && d.kind != WSL_KIND_AP) continue;
    if (a.filter == 2 && d.kind == WSL_KIND_AP) continue;
    a.order.push_back((uint16_t)i);
  }
  std::sort(a.order.begin(), a.order.end(), [&](uint16_t x, uint16_t y) {
    const Dev &dx = a.devs[x], &dy = a.devs[y];
    if (dx.best != dy.best) return dx.best > dy.best;
    return memcmp(dx.mac, dy.mac, 6) < 0;
  });
}

static void finishCal(App& a, uint32_t now) {
  a.calRunning = false;
  float mean[WSL_MAX_WIRED], off[WSL_MAX_WIRED];
  bool valid[WSL_MAX_WIRED];
  int used = 0;
  for (int i = 0; i < WSL_MAX_WIRED; i++) {
    valid[i] = a.nodes[i].present && a.calTicks && a.calN[i] >= a.calTicks / 2;
    mean[i] = valid[i] ? a.calSum[i] / a.calN[i] : 0;
    used += valid[i];
  }
  if (used < 2) {
    toast(a, now, "Calibration failed: not enough signal");
    return;
  }
  wsl::spinCalOffsets(mean, valid, WSL_MAX_WIRED, off);
  for (int i = 0; i < WSL_MAX_WIRED; i++)
    if (valid[i]) a.nodes[i].cal = off[i];
  a.prefsDirty = true;
  toast(a, now, "Calibration saved");
}

void tick(App& a, uint32_t now) {
  if (now - a.lastTickMs < 100) return;
  a.lastTickMs = now;

  for (int i = WSL_MAX_WIRED; i < WSL_MAX_NODES; i++)
    if (a.nodes[i].present && now - a.nodes[i].lastOkMs > 4000) nodePresence(a, i, false, 0, now);

  if (!a.tDown && now - a.lastSortMs >= 1500) {
    a.lastSortMs = now;
    sortDevices(a, now);
  }
  if (!a.hasTarget) return;

  // Handheld head: strongest calibrated antenna + bearing.
  float ang[WSL_MAX_WIRED], rssi[WSL_MAX_WIRED];
  bool valid[WSL_MAX_WIRED];
  a.headLive = false;
  a.headRssi = -127;
  for (int i = 0; i < WSL_MAX_WIRED; i++) {
    const Node& n = a.nodes[i];
    valid[i] = n.present && n.filt.live(now);
    ang[i] = n.angle;
    rssi[i] = n.filt.value + n.cal;
    if (valid[i] && rssi[i] > a.headRssi) {
      a.headRssi = rssi[i];
      a.headLive = true;
    }
  }
  a.bearing = wsl::estimateBearing(ang, rssi, valid, WSL_MAX_WIRED);
  if (a.bearing.valid) {
    const float r = wsl::deg2rad(a.bearing.deg);
    a.bvx += (a.bearing.confidence * sinf(r) - a.bvx) * 0.25f;
    a.bvy += (a.bearing.confidence * cosf(r) - a.bvy) * 0.25f;
  } else {
    a.bvx *= 0.9f;
    a.bvy *= 0.9f;
  }

  if (now - a.lastHistMs >= 250) {
    a.lastHistMs = now;
    a.hist[a.histPos] = a.headLive ? (int8_t)lroundf(fmaxf(a.headRssi, -127.0f)) : -128;
    a.histPos = (a.histPos + 1) % HIST_N;
  }

  if (a.calRunning) {
    a.calTicks++;
    for (int i = 0; i < WSL_MAX_WIRED; i++) {
      if (!valid[i]) continue;
      a.calSum[i] += a.nodes[i].filt.value;
      a.calN[i]++;
    }
    if (now - a.calStartMs >= CAL_MS) finishCal(a, now);
  }

  if (a.screen == SCR_MAP && now - a.lastMapMs >= 500) {
    a.lastMapMs = now;
    wsl::MapPoint pts[64];
    int n = 0;
    for (auto& s : a.survey) pts[n++] = s;
    for (int i = WSL_MAX_WIRED; i < WSL_MAX_NODES; i++) {
      const Node& nd = a.nodes[i];
      if (nd.present && nd.filt.live(now)) pts[n++] = {nd.mx, nd.my, nd.filt.value + nd.cal};
    }
    a.mapRes = wsl::solveMap(pts, n, 0, 0, a.mapSize, a.nExp, MAP_GRID, a.heat);
    a.heatValid = a.mapRes.valid;
  }
}

// ---------------------------------------------------------------- drawing

static const char* const TAB_NAMES[4] = {"Scan", "Hunt", "Map", "Nodes"};

static void drawTopBar(App& a, LovyanGFX& g, uint32_t now) {
  g.fillRect(0, 0, SCREEN_W, TOP_H, C_PANEL);
  for (int i = 0; i < 4; i++) {
    const int x = 8 + i * 168;
    const bool on = a.screen == i;
    if (on) g.fillRoundRect(x, 8, 160, TOP_H - 16, 10, C_ACCENT);
    txt(g, TAB_NAMES[i], x + 80, TOP_H / 2, F24, on ? C_BLACK : C_TEXT, textdatum_t::middle_center);
    hit(a, x, 0, 160, TOP_H, A_TAB + i);
  }
  int wired = 0, remote = 0;
  for (int i = 0; i < WSL_MAX_NODES; i++)
    if (a.nodes[i].present) (i < WSL_MAX_WIRED ? wired : remote)++;
  char buf[96];
  if (tracking(a)) snprintf(buf, sizeof(buf), "Tracking on ch %u", a.targetCh);
  else snprintf(buf, sizeof(buf), "Scanning ch 1-13");
  txt(g, buf, SCREEN_W - 16, 20, F18, tracking(a) ? C_GOOD : C_DIM, textdatum_t::middle_right);
  if (remote) snprintf(buf, sizeof(buf), "%d wired + %d remote nodes", wired, remote);
  else snprintf(buf, sizeof(buf), "%d node%s", wired, wired == 1 ? "" : "s");
  txt(g, buf, SCREEN_W - 16, 44, F18, wired ? C_TEXT : C_BAD, textdatum_t::middle_right);
  (void)now;
}

static void drawNoNodes(LovyanGFX& g, int y) {
  txt(g, "No sensor nodes found", SCREEN_W / 2, y, F40, C_WARN, textdatum_t::middle_center);
  txt(g, "Connect the XIAO nodes to the Tab5's Port A (Grove): SDA, SCL, 5V, GND.", SCREEN_W / 2, y + 60,
      F24, C_DIM, textdatum_t::middle_center);
  txt(g, "Each node needs its own id 0-7 (I2C address 0x30 + id).", SCREEN_W / 2, y + 100, F24, C_DIM,
      textdatum_t::middle_center);
}

static bool anyWired(const App& a) {
  for (int i = 0; i < WSL_MAX_WIRED; i++)
    if (a.nodes[i].present) return true;
  return false;
}

static void drawScan(App& a, LovyanGFX& g, uint32_t now) {
  const int listBottom = SCREEN_H - (a.hasSel ? FOOT_H : 0);
  const int rows = (int)a.order.size();
  const int maxScroll = std::max(0, rows * ROW_H - (listBottom - LIST_Y));
  a.scrollY = std::min(std::max(a.scrollY, 0), maxScroll);

  if (!rows) {
    if (!anyWired(a)) drawNoNodes(g, 330);
    else txt(g, "Listening...", SCREEN_W / 2, 360, F40, C_DIM, textdatum_t::middle_center);
  }

  g.setClipRect(0, LIST_Y, SCREEN_W, listBottom - LIST_Y);
  for (int i = 0; i < rows; i++) {
    const int y = LIST_Y + i * ROW_H - a.scrollY;
    if (y + ROW_H <= LIST_Y || y >= listBottom) continue;
    const Dev& d = a.devs[a.order[i]];
    const bool sel = a.hasSel && !memcmp(a.selMac, d.mac, 6);
    const bool stale = d.best == -128;
    if (sel) g.fillRect(0, y, SCREEN_W, ROW_H - 2, 0x17384Au);
    else if (i & 1) g.fillRect(0, y, SCREEN_W, ROW_H - 2, 0x12161Du);
    const int my = y + ROW_H / 2 - 1;

    const uint32_t kc = d.kind == WSL_KIND_AP ? C_ACCENT : d.kind == WSL_KIND_STA ? C_GOOD : C_DIM;
    g.drawRoundRect(16, y + 12, 96, ROW_H - 26, 8, kc);
    txt(g, kindLabel(d.kind), 64, my, F18, kc, textdatum_t::middle_center);

    char name[48], mac[20];
    devName(d, name, sizeof(name));
    macStr(d.mac, mac);
    g.setClipRect(0, std::max(y, LIST_Y), 600, std::min(y + ROW_H, listBottom) - std::max(y, LIST_Y));
    txt(g, name, 132, my, F24, stale ? C_DIM : C_TEXT, textdatum_t::middle_left);
    g.setClipRect(0, LIST_Y, SCREEN_W, listBottom - LIST_Y);
    txt(g, mac, 616, my, F18, C_DIM, textdatum_t::middle_left);
    if (d.channel) txtf(g, 878, my, F18, C_DIM, textdatum_t::middle_right, "ch %u", d.channel);

    g.fillRoundRect(904, y + 22, 260, 14, 7, C_PANEL2);
    if (!stale) {
      const float l = level(d.best);
      g.fillRoundRect(904, y + 22, std::max(14, (int)(260 * l)), 14, 7, heatColor(l));
      txtf(g, SCREEN_W - 16, my, F24, C_TEXT, textdatum_t::middle_right, "%d", d.best);
    } else {
      txt(g, "--", SCREEN_W - 16, my, F24, C_DIM, textdatum_t::middle_right);
    }
    hit(a, 0, std::max(y, LIST_Y), SCREEN_W, std::min(y + ROW_H, listBottom) - std::max(y, LIST_Y), A_ROW + i);
  }
  g.clearClipRect();

  if (maxScroll > 0) {   // scroll position
    const int trackH = listBottom - LIST_Y;
    const int barH = std::max(40, trackH * trackH / (rows * ROW_H));
    g.fillRoundRect(SCREEN_W - 6, LIST_Y + (trackH - barH) * a.scrollY / maxScroll, 4, barH, 2, C_DIM);
  }

  // Filter row.
  static const char* const FILTERS[3] = {"All", "Access points", "Clients"};
  static const int FW[3] = {100, 220, 150};
  int x = 16;
  for (int i = 0; i < 3; i++) {
    const bool on = a.filter == i;
    g.fillRoundRect(x, TOP_H + 12, FW[i], 48, 24, on ? C_TEXT : C_PANEL2);
    txt(g, FILTERS[i], x + FW[i] / 2, TOP_H + 36, F24, on ? C_BLACK : C_TEXT, textdatum_t::middle_center);
    hit(a, x, TOP_H + 4, FW[i], 64, A_FILTER + i);
    x += FW[i] + 12;
  }
  txtf(g, x + 12, TOP_H + 36, F18, C_DIM, textdatum_t::middle_left, "%d device%s  -  tap one to select", rows,
       rows == 1 ? "" : "s");
  button(a, g, SCREEN_W - 156, TOP_H + 12, 140, 48, "Clear", A_CLEAR);

  if (a.hasSel) {
    const Dev* d = findDev(a, a.selMac);
    g.fillRect(0, listBottom, SCREEN_W, FOOT_H, C_PANEL);
    g.drawFastHLine(0, listBottom, SCREEN_W, C_LINE);
    if (d) {
      char name[48], mac[20];
      devName(*d, name, sizeof(name));
      macStr(d->mac, mac);
      g.setClipRect(0, listBottom, 760, FOOT_H);
      txt(g, name, 24, listBottom + 30, F24, C_TEXT, textdatum_t::middle_left);
      g.clearClipRect();
      txtf(g, 24, listBottom + 66, F18, C_DIM, textdatum_t::middle_left, "%s   %s   channel %u", mac,
           kindLabel(d->kind), d->channel);
    }
    button(a, g, SCREEN_W - 496, listBottom + 16, 232, 64, "Hunt", A_HUNT, true);
    button(a, g, SCREEN_W - 248, listBottom + 16, 232, 64, "Map", A_MAP, true);
  }
  (void)now;
}

static void drawTargetHeader(App& a, LovyanGFX& g, int x, int y, int w) {
  char mac[20];
  macStr(a.target, mac);
  g.setClipRect(x, y, w, 80);
  txt(g, a.targetName, x, y, F24, C_TEXT);
  g.clearClipRect();
  txtf(g, x, y + 36, F18, C_DIM, textdatum_t::top_left, "%s   %s   channel %u", mac, kindLabel(a.targetKind),
       a.targetCh);
}

static void drawNoTarget(LovyanGFX& g) {
  txt(g, "No target selected", SCREEN_W / 2, 340, F40, C_DIM, textdatum_t::middle_center);
  txt(g, "Pick a device on the Scan tab, then tap Hunt or Map.", SCREEN_W / 2, 400, F24, C_DIM,
      textdatum_t::middle_center);
}

static void quad(LovyanGFX& g, float x0, float y0, float x1, float y1, float halfW, uint32_t c) {
  const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 1) return;
  const float px = -dy / len * halfW, py = dx / len * halfW;
  g.fillTriangle(x0 + px, y0 + py, x0 - px, y0 - py, x1 + px, y1 + py, c);
  g.fillTriangle(x0 - px, y0 - py, x1 - px, y1 - py, x1 + px, y1 + py, c);
}

static void drawHunt(App& a, LovyanGFX& g, uint32_t now) {
  if (!a.hasTarget) return drawNoTarget(g);

  // --- compass
  g.fillCircle(CX, CY, CR, C_PANEL);
  for (int r = CR; r > 60; r -= 67) g.drawCircle(CX, CY, r, C_LINE);
  for (int d = 0; d < 360; d += 45) {
    const float s = sinf(wsl::deg2rad(d)), c = cosf(wsl::deg2rad(d));
    g.drawLine(CX + s * 60, CY - c * 60, CX + s * CR, CY - c * CR, C_LINE);
  }
  txt(g, "FRONT", CX, CY - CR - 6, F18, C_DIM, textdatum_t::bottom_center);

  // One wedge per antenna: longer and hotter = stronger.
  int wired = 0;
  for (int i = 0; i < WSL_MAX_WIRED; i++) wired += a.nodes[i].present;
  const float half = wired ? std::min(40.0f, 180.0f / wired - 5.0f) : 0;
  for (int i = 0; i < WSL_MAX_WIRED; i++) {
    const Node& n = a.nodes[i];
    if (!n.present) continue;
    const bool live = n.filt.live(now);
    const float l = live ? level(n.filt.value + n.cal) : 0;
    const int r1 = 64 + (int)((CR - 70) * std::max(l, 0.04f));
    const float a0 = n.angle - 90 - half, a1 = n.angle - 90 + half;
    g.fillArc(CX, CY, 64, r1, a0, a1, live ? blend(C_PANEL, heatColor(l), 0.8f) : C_LINE);
    const float s = sinf(wsl::deg2rad(n.angle)), c = cosf(wsl::deg2rad(n.angle));
    const int lx = CX + s * (CR - 30), ly = CY - c * (CR - 30);
    g.fillCircle(lx, ly, 22, C_BG);
    g.drawCircle(lx, ly, 22, live ? C_TEXT : C_LINE);
    txtf(g, lx, ly, F18, live ? C_TEXT : C_DIM, textdatum_t::middle_center, "%d", i);
  }

  // Arrow.
  const float conf = sqrtf(a.bvx * a.bvx + a.bvy * a.bvy);
  const bool have = a.headLive && conf > 0.08f;
  if (have) {
    const float s = a.bvx / conf, c = a.bvy / conf;
    const uint32_t col = conf > 0.45f ? C_ACCENT : blend(C_DIM, C_ACCENT, conf / 0.45f);
    const float tipX = CX + s * (CR - 58), tipY = CY - c * (CR - 58);
    const float baseX = CX + s * (CR - 130), baseY = CY - c * (CR - 130);
    quad(g, CX - s * 40, CY + c * 40, baseX, baseY, 11, col);
    g.fillTriangle(tipX, tipY, baseX - c * 40, baseY - s * 40, baseX + c * 40, baseY + s * 40, col);
  }
  g.fillCircle(CX, CY, 56, C_BG);
  g.drawCircle(CX, CY, 56, C_LINE);
  if (have) {
    float deg = wsl::wrap360(wsl::rad2deg(atan2f(a.bvx, a.bvy)));
    txtf(g, CX, CY, F40, C_TEXT, textdatum_t::middle_center, "%d", (int)lroundf(deg) % 360);
  } else {
    txt(g, "?", CX, CY, F40, C_DIM, textdatum_t::middle_center);
  }

  // --- right panel
  const int px = 696, pw = SCREEN_W - px - 16;
  drawTargetHeader(a, g, px, 80, pw);

  if (a.headLive) {
    const float l = level(a.headRssi);
    txtf(g, px, 152, F72, heatColor(l), textdatum_t::top_left, "%d", (int)lroundf(a.headRssi));
    txt(g, "dBm", px + 200, 200, F24, C_DIM);
    g.fillRoundRect(px, 246, pw, 28, 14, C_PANEL2);
    g.fillRoundRect(px, 246, std::max(28, (int)(pw * l)), 28, 14, heatColor(l));
  } else {
    txt(g, "no signal", px, 164, F40, C_DIM);
    txt(g, "Target is quiet or on another channel", px, 216, F18, C_DIM);
    g.fillRoundRect(px, 246, pw, 28, 14, C_PANEL2);
  }

  // direction in words + confidence
  if (have) {
    const float deg = wsl::angleDiff(wsl::rad2deg(atan2f(a.bvx, a.bvy)), 0);
    const float ad = fabsf(deg);
    char words[40];
    if (ad < 20) snprintf(words, sizeof(words), "Straight ahead");
    else if (ad > 160) snprintf(words, sizeof(words), "Behind you");
    else snprintf(words, sizeof(words), "%d deg to the %s", (int)lroundf(ad), deg > 0 ? "right" : "left");
    txt(g, words, px + pw, 150, F24, C_TEXT, textdatum_t::top_right);
    txtf(g, px + pw, 186, F18, conf > 0.45f ? C_GOOD : C_WARN, textdatum_t::top_right, "%s direction (%d%%)",
         conf > 0.45f ? "confident" : "weak", (int)(conf * 100));
  } else if (a.headLive) {
    txt(g, "No clear direction", px + pw, 150, F24, C_DIM, textdatum_t::top_right);
    txt(g, "turn slowly / move on", px + pw, 186, F18, C_DIM, textdatum_t::top_right);
  }

  // signal history
  const int hx = px, hy = 296, hw = pw, hh = 150;
  g.fillRoundRect(hx, hy, hw, hh, 8, C_PANEL);
  txt(g, "last 60 s", hx + hw - 10, hy + 8, F18, C_DIM, textdatum_t::top_right);
  int prevX = -1, prevY = 0;
  for (int i = 0; i < HIST_N; i++) {
    const int8_t v = a.hist[(a.histPos + i) % HIST_N];
    if (v == -128) { prevX = -1; continue; }
    const int x = hx + 6 + (hw - 12) * i / (HIST_N - 1);
    const int y = hy + hh - 8 - (int)((hh - 16) * level(v));
    if (prevX >= 0) {
      g.drawLine(prevX, prevY, x, y, C_ACCENT);
      g.drawLine(prevX, prevY + 1, x, y + 1, C_ACCENT);
    }
    prevX = x;
    prevY = y;
  }

  // per-antenna readout
  int col = 0;
  for (int i = 0; i < WSL_MAX_WIRED; i++) {
    const Node& n = a.nodes[i];
    if (!n.present) continue;
    const int x = px + (col % 4) * (pw / 4), y = 458 + (col / 4) * 30;
    if (n.filt.live(now))
      txtf(g, x, y, F18, C_TEXT, textdatum_t::top_left, "#%d  %d dBm", i, (int)lroundf(n.filt.value + n.cal));
    else
      txtf(g, x, y, F18, C_DIM, textdatum_t::top_left, "#%d  --", i);
    col++;
  }

  // controls
  const int bw = (pw - 2 * 12) / 3;
  button(a, g, px, 540, 84, 64, "-", A_CH_DN);
  txtf(g, px + 84 + (pw - 168) / 2, 572, F24, C_TEXT, textdatum_t::middle_center, "Channel %u", a.targetCh);
  button(a, g, px + pw - 84, 540, 84, 64, "+", A_CH_UP);
  if (a.calRunning) {
    const float t = clamp01((now - a.calStartMs) / (float)CAL_MS);
    g.fillRoundRect(px, 632, bw, 64, 10, C_PANEL2);
    g.fillRoundRect(px, 632, std::max(20, (int)(bw * t)), 64, 10, C_WARN);
    txt(g, "Turning...", px + bw / 2, 664, F24, C_TEXT, textdatum_t::middle_center);
  } else {
    button(a, g, px, 632, bw, 64, "Spin cal", A_CAL);
  }
  button(a, g, px + bw + 12, 632, bw, 64, a.mute ? "Sound off" : "Sound on", A_MUTE);
  button(a, g, px + 2 * (bw + 12), 632, bw, 64, "Stop", A_STOP);
  if (a.calRunning)
    txt(g, "Turn one full circle on the spot, holding the Tab5 level", CX, SCREEN_H - 8, F18, C_WARN,
        textdatum_t::bottom_center);
}

static int mapSX(const App& a, float x) { return MAP_X + (int)lroundf(x / a.mapSize * MAP_S); }
static int mapSY(const App& a, float y) { return MAP_Y + (int)lroundf(y / a.mapSize * MAP_S); }

static void drawMap(App& a, LovyanGFX& g, uint32_t now) {
  if (!a.hasTarget) return drawNoTarget(g);

  g.fillRect(MAP_X, MAP_Y, MAP_S, MAP_S, C_PANEL);
  if (a.heatValid) {   // where the readings say the source probably is
    float mn = 1e30f;
    for (float v : a.heat) mn = std::min(mn, v);
    const float sigma2 = 2.0f * 4.0f * 4.0f;
    for (int j = 0; j < MAP_GRID; j++) {
      const int y0 = MAP_Y + j * MAP_S / MAP_GRID, y1 = MAP_Y + (j + 1) * MAP_S / MAP_GRID;
      for (int i = 0; i < MAP_GRID; i++) {
        const float l = expf(-(a.heat[j * MAP_GRID + i] - mn) / sigma2);
        if (l < 0.04f) continue;
        const int x0 = MAP_X + i * MAP_S / MAP_GRID, x1 = MAP_X + (i + 1) * MAP_S / MAP_GRID;
        g.fillRect(x0, y0, x1 - x0, y1 - y0, blend(C_PANEL, heatColor(l), 0.25f + 0.6f * l));
      }
    }
  }
  for (int k = 1; k < 4; k++) {
    g.drawFastVLine(MAP_X + k * MAP_S / 4, MAP_Y, MAP_S, C_LINE);
    g.drawFastHLine(MAP_X, MAP_Y + k * MAP_S / 4, MAP_S, C_LINE);
  }
  g.drawRect(MAP_X, MAP_Y, MAP_S, MAP_S, C_LINE);
  txtf(g, MAP_X + 8, MAP_Y + MAP_S - 8, F18, C_DIM, textdatum_t::bottom_left, "%g m x %g m   grid %g m",
       a.mapSize, a.mapSize, a.mapSize / 4);

  g.setClipRect(MAP_X, MAP_Y, MAP_S, MAP_S);
  for (auto& s : a.survey) {
    const int x = mapSX(a, s.x), y = mapSY(a, s.y);
    g.fillCircle(x, y, 11, C_BG);
    g.fillCircle(x, y, 8, heatColor(level(s.rssi)));
  }
  for (int i = WSL_MAX_WIRED; i < WSL_MAX_NODES; i++) {
    const Node& n = a.nodes[i];
    if (!n.present) continue;
    const bool live = n.filt.live(now);
    const int x = mapSX(a, n.mx), y = mapSY(a, n.my);
    g.fillCircle(x, y, 24, C_BG);
    g.drawCircle(x, y, 24, live ? heatColor(level(n.filt.value + n.cal)) : C_DIM);
    g.drawCircle(x, y, 23, live ? heatColor(level(n.filt.value + n.cal)) : C_DIM);
    txtf(g, x, y, F18, live ? C_TEXT : C_DIM, textdatum_t::middle_center, "R%d", i - WSL_MAX_WIRED);
  }
  if (a.mapRes.valid) {
    const int x = mapSX(a, a.mapRes.x), y = mapSY(a, a.mapRes.y);
    for (int r = 26; r <= 28; r++) g.drawCircle(x, y, r, C_TEXT);
    quad(g, x - 40, y, x - 14, y, 1.5f, C_TEXT);
    quad(g, x + 14, y, x + 40, y, 1.5f, C_TEXT);
    quad(g, x, y - 40, x, y - 14, 1.5f, C_TEXT);
    quad(g, x, y + 14, x, y + 40, 1.5f, C_TEXT);
  }
  g.clearClipRect();

  // --- right panel
  const int px = 664, pw = SCREEN_W - px - 16;
  drawTargetHeader(a, g, px, 80, pw);

  txt(g, "Signal where you are", px, 150, F18, C_DIM);
  if (a.headLive) txtf(g, px, 174, F40, heatColor(level(a.headRssi)), textdatum_t::top_left, "%d dBm",
                       (int)lroundf(a.headRssi));
  else txt(g, "no signal", px, 174, F40, C_DIM);

  txt(g, "Estimated source", px + pw / 2, 150, F18, C_DIM);
  int live = (int)a.survey.size();
  for (int i = WSL_MAX_WIRED; i < WSL_MAX_NODES; i++) live += a.nodes[i].present && a.nodes[i].filt.live(now);
  if (a.mapRes.valid) {
    txtf(g, px + pw / 2, 174, F40, C_TEXT, textdatum_t::top_left, "%.1f, %.1f m", a.mapRes.x, a.mapRes.y);
    const uint32_t c = a.mapRes.rmsDb < 4 ? C_GOOD : a.mapRes.rmsDb < 7 ? C_WARN : C_BAD;
    txtf(g, px + pw / 2, 224, F18, c, textdatum_t::top_left, "%d readings, fit error %.1f dB", live, a.mapRes.rmsDb);
  } else {
    txtf(g, px + pw / 2, 180, F24, C_DIM, textdatum_t::top_left, "need 3 readings");
    txtf(g, px + pw / 2, 224, F18, C_DIM, textdatum_t::top_left, "have %d", live);
  }

  txt(g, "Walk around. At each stop, wait 2 s, then tap the map", px, 272, F18, C_DIM);
  txt(g, "where you are standing. More spread-out stops = better.", px, 298, F18, C_DIM);
  txt(g, "Drag remote nodes (R0, R1...) to where they really are.", px, 324, F18, C_DIM);

  int col = 0;
  for (int i = WSL_MAX_WIRED; i < WSL_MAX_NODES; i++) {
    const Node& n = a.nodes[i];
    if (!n.present) continue;
    const int x = px + (col % 4) * (pw / 4), y = 368 + (col / 4) * 30;
    if (n.filt.live(now))
      txtf(g, x, y, F18, C_TEXT, textdatum_t::top_left, "R%d  %d dBm", i - WSL_MAX_WIRED,
           (int)lroundf(n.filt.value + n.cal));
    else
      txtf(g, x, y, F18, C_DIM, textdatum_t::top_left, "R%d  --", i - WSL_MAX_WIRED);
    col++;
  }

  button(a, g, px, 448, 84, 64, "-", A_N_DN);
  txtf(g, px + pw / 2, 468, F24, C_TEXT, textdatum_t::middle_center, "Path loss n = %.1f", a.nExp);
  txt(g, a.nExp < 2.4f ? "open space" : a.nExp < 3.3f ? "indoors, few walls" : "many walls", px + pw / 2, 496, F18,
      C_DIM, textdatum_t::middle_center);
  button(a, g, px + pw - 84, 448, 84, 64, "+", A_N_UP);

  const int bw = (pw - 2 * 12) / 3;
  char area[24];
  snprintf(area, sizeof(area), "Area %g m", a.mapSize);
  button(a, g, px, 540, bw, 64, area, A_AREA);
  button(a, g, px + bw + 12, 540, bw, 64, "Undo", A_UNDO);
  button(a, g, px + 2 * (bw + 12), 540, bw, 64, "Clear", A_CLRPTS);
  button(a, g, px, 632, pw, 64, "Stop", A_STOP);
}

static void drawNodes(App& a, LovyanGFX& g, uint32_t now) {
  int count = 0;
  for (auto& n : a.nodes) count += n.present;
  if (!count) {
    drawNoNodes(g, 300);
    return;
  }
  static const char* const CHIP[4] = {"ESP32", "XIAO C3", "XIAO S3", "XIAO C6"};
  const int y0 = 84;
  txt(g, "Node", 24, y0, F18, C_DIM);
  txt(g, "Board", 150, y0, F18, C_DIM);
  txt(g, "Signal", 330, y0, F18, C_DIM);
  txt(g, "Frames/s", 480, y0, F18, C_DIM);
  txt(g, "Cal", 630, y0, F18, C_DIM);
  txt(g, "Errors", 740, y0, F18, C_DIM);
  txt(g, "Antenna direction", 900, y0, F18, C_DIM);
  int row = 0;
  for (int i = 0; i < WSL_MAX_NODES && row < 8; i++) {
    const Node& n = a.nodes[i];
    if (!n.present) continue;
    const int y = y0 + 34 + row * 60, my = y + 28;
    if (row & 1) g.fillRect(0, y, SCREEN_W, 58, 0x12161Du);
    const bool wired = i < WSL_MAX_WIRED;
    txtf(g, 24, my, F24, C_TEXT, textdatum_t::middle_left, wired ? "#%d" : "R%d", wired ? i : i - WSL_MAX_WIRED);
    txt(g, wired ? CHIP[n.chip & 3] : "remote", 150, my, F24, C_TEXT, textdatum_t::middle_left);
    if (n.filt.live(now)) {
      txtf(g, 330, my, F24, C_TEXT, textdatum_t::middle_left, "%d dBm", (int)lroundf(n.filt.value + n.cal));
      txtf(g, 480, my, F24, C_TEXT, textdatum_t::middle_left, "%.0f", n.filt.rate);
    } else {
      txt(g, tracking(a) ? "--" : "scanning", 330, my, F24, C_DIM, textdatum_t::middle_left);
    }
    txtf(g, 630, my, F24, C_TEXT, textdatum_t::middle_left, "%+.1f", n.cal);
    txtf(g, 740, my, F24, n.errors ? C_WARN : C_DIM, textdatum_t::middle_left, "%u", (unsigned)n.errors);
    if (wired) {
      button(a, g, 900, y + 4, 72, 50, "-", A_ANG_DN + i);
      txtf(g, 1040, my, F24, C_TEXT, textdatum_t::middle_center, "%d deg", (int)lroundf(n.angle));
      button(a, g, 1108, y + 4, 72, 50, "+", A_ANG_UP + i);
    } else {
      txtf(g, 900, my, F24, C_DIM, textdatum_t::middle_left, "map %.1f, %.1f m", n.mx, n.my);
    }
    row++;
  }
  txt(g, "Antenna direction: 0 = top edge of the Tab5, clockwise seen from above.", 24, 596, F18, C_DIM);
  button(a, g, 24, 632, 300, 64, "Even spacing", A_EVEN);
  button(a, g, 340, 632, 300, 64, "Reset calibration", A_RESETCAL);
}

void draw(App& a, LovyanGFX& g, uint32_t now) {
  a.btns.clear();
  g.fillScreen(C_BG);
  switch (a.screen) {
    case SCR_SCAN: drawScan(a, g, now); break;
    case SCR_HUNT: drawHunt(a, g, now); break;
    case SCR_MAP: drawMap(a, g, now); break;
    case SCR_NODES: drawNodes(a, g, now); break;
  }
  drawTopBar(a, g, now);
  if (a.toast[0] && now < a.toastUntil) {
    g.setFont(F24);
    const int w = g.textWidth(a.toast) + 56;
    // Over the compass / map on those screens so it never hides a button.
    const bool left = a.screen == SCR_HUNT || a.screen == SCR_MAP;
    const int cx = left ? CX : SCREEN_W / 2, ty = left ? SCREEN_H - 72 : SCREEN_H - 190;
    g.fillRoundRect(cx - w / 2, ty, w, 56, 28, C_TEXT);
    txt(g, a.toast, cx, ty + 28, F24, C_BLACK, textdatum_t::middle_center);
  }
}

// ---------------------------------------------------------------- input

static void act(App& a, int id, uint32_t now) {
  if (id >= A_ROW) {
    const int i = id - A_ROW;
    if (i < (int)a.order.size()) {
      const Dev& d = a.devs[a.order[i]];
      if (a.hasSel && !memcmp(a.selMac, d.mac, 6)) a.hasSel = false;
      else {
        a.hasSel = true;
        memcpy(a.selMac, d.mac, 6);
      }
    }
    return;
  }
  if (id >= A_ANG_DN && id < A_ANG_UP + WSL_MAX_WIRED) {
    const bool up = id >= A_ANG_UP;
    Node& n = a.nodes[id - (up ? A_ANG_UP : A_ANG_DN)];
    n.angle = wsl::wrap360(n.angle + (up ? 15 : -15));
    a.anglesCustom = true;
    a.prefsDirty = true;
    return;
  }
  if (id >= A_TAB && id < A_TAB + 4) {
    const bool was = tracking(a);
    a.screen = (Screen)(id - A_TAB);
    if (tracking(a) != was) resetTrackState(a);
    return;
  }
  if (id >= A_FILTER && id < A_FILTER + 3) {
    a.filter = id - A_FILTER;
    a.scrollY = 0;
    a.lastSortMs = 0;
    sortDevices(a, now);
    return;
  }
  switch (id) {
    case A_CLEAR:
      a.devs.clear();
      a.order.clear();
      a.hasSel = false;
      a.scrollY = 0;
      break;
    case A_HUNT:
    case A_MAP: {
      const Dev* d = findDev(a, a.selMac);
      if (!d) break;
      if (!a.hasTarget || memcmp(a.target, d->mac, 6) != 0) setTarget(a, *d);
      else resetTrackState(a);
      a.screen = id == A_HUNT ? SCR_HUNT : SCR_MAP;
      break;
    }
    case A_STOP:
      a.hasTarget = false;
      a.screen = SCR_SCAN;
      break;
    case A_CAL:
      if (!a.headLive) {
        toast(a, now, "No signal to calibrate on");
        break;
      }
      a.calRunning = true;
      a.calStartMs = now;
      a.calTicks = 0;
      for (int i = 0; i < WSL_MAX_WIRED; i++) a.calSum[i] = 0, a.calN[i] = 0;
      break;
    case A_MUTE:
      a.mute = !a.mute;
      a.prefsDirty = true;
      break;
    case A_CH_DN:
    case A_CH_UP:
      a.targetCh = id == A_CH_UP ? a.targetCh % 13 + 1 : (a.targetCh + 11) % 13 + 1;
      resetTrackState(a);
      break;
    case A_N_DN:
    case A_N_UP:
      a.nExp = std::min(4.5f, std::max(1.8f, a.nExp + (id == A_N_UP ? 0.2f : -0.2f)));
      a.lastMapMs = 0;
      a.prefsDirty = true;
      break;
    case A_AREA: {
      static const float SIZES[] = {5, 10, 20, 50, 100};
      int k = 0;
      while (k < 5 && SIZES[k] <= a.mapSize) k++;
      a.mapSize = SIZES[k % 5];
      // Readings keep their real-world coordinates; remote nodes must stay on the map.
      for (int i = WSL_MAX_WIRED; i < WSL_MAX_NODES; i++) {
        a.nodes[i].mx = std::min(a.nodes[i].mx, a.mapSize);
        a.nodes[i].my = std::min(a.nodes[i].my, a.mapSize);
      }
      a.lastMapMs = 0;
      a.prefsDirty = true;
      break;
    }
    case A_UNDO:
      if (!a.survey.empty()) a.survey.pop_back();
      a.lastMapMs = 0;
      break;
    case A_CLRPTS:
      a.survey.clear();
      a.lastMapMs = 0;
      break;
    case A_EVEN:
      a.anglesCustom = false;
      evenAngles(a);
      a.prefsDirty = true;
      break;
    case A_RESETCAL:
      for (auto& n : a.nodes) n.cal = 0;
      a.prefsDirty = true;
      toast(a, now, "Calibration cleared");
      break;
  }
}

static bool inMap(int x, int y) { return x >= MAP_X && x < MAP_X + MAP_S && y >= MAP_Y && y < MAP_Y + MAP_S; }

void touch(App& a, bool pressed, int x, int y, uint32_t now) {
  if (pressed && !a.tDown) {   // finger down
    a.tDown = true;
    a.tMoved = false;
    a.tx0 = a.tx = x;
    a.ty0 = a.ty = y;
    a.tScroll0 = a.scrollY;
    a.tOnList = a.screen == SCR_SCAN && y >= LIST_Y && y < SCREEN_H - (a.hasSel ? FOOT_H : 0);
    a.tDragNode = -1;
    if (a.screen == SCR_MAP && a.hasTarget && inMap(x, y)) {
      int bestD = 44 * 44;
      for (int i = WSL_MAX_WIRED; i < WSL_MAX_NODES; i++) {
        if (!a.nodes[i].present) continue;
        const int dx = mapSX(a, a.nodes[i].mx) - x, dy = mapSY(a, a.nodes[i].my) - y;
        if (dx * dx + dy * dy < bestD) {
          bestD = dx * dx + dy * dy;
          a.tDragNode = i;
        }
      }
    }
  } else if (pressed) {        // finger moving
    a.tx = x;
    a.ty = y;
    if (abs(x - a.tx0) + abs(y - a.ty0) > 18) a.tMoved = true;
    if (a.tDragNode >= 0 && a.tMoved) {
      Node& n = a.nodes[a.tDragNode];
      n.mx = clamp01((x - MAP_X) / (float)MAP_S) * a.mapSize;
      n.my = clamp01((y - MAP_Y) / (float)MAP_S) * a.mapSize;
      a.lastMapMs = 0;
    } else if (a.tOnList && a.tMoved) {
      a.scrollY = a.tScroll0 - (y - a.ty0);
    }
  } else if (a.tDown) {        // finger up
    a.tDown = false;
    if (a.tDragNode >= 0 && a.tMoved) {
      a.prefsDirty = true;
      return;
    }
    if (a.tMoved) return;
    for (int i = (int)a.btns.size() - 1; i >= 0; i--) {
      const Btn& b = a.btns[i];
      if (a.tx0 >= b.x && a.tx0 < b.x + b.w && a.ty0 >= b.y && a.ty0 < b.y + b.h) {
        act(a, b.id, now);
        return;
      }
    }
    if (a.screen == SCR_MAP && a.hasTarget && inMap(a.tx0, a.ty0)) {
      if (!a.headLive) toast(a, now, "No signal here - reading not added");
      else if ((int)a.survey.size() >= MAX_SURVEY) toast(a, now, "Reading limit reached - clear some");
      else {
        a.survey.push_back({(a.tx0 - MAP_X) / (float)MAP_S * a.mapSize, (a.ty0 - MAP_Y) / (float)MAP_S * a.mapSize,
                            a.headRssi});
        a.lastMapMs = 0;
        toast(a, now, "Reading added");
      }
    }
  }
}

}  // namespace app
