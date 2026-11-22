// WiFi Source Locator - Tab5 application state, logic and UI.
//
// Nothing in here touches hardware: main.cpp feeds it node data and touch
// input and hands it something to draw on. That keeps it runnable on a PC
// (see tab5/sim) for checking the screens without a Tab5.
#pragma once
#include <M5GFX.h>
#include <stdint.h>
#include <vector>

#include "wsl_locate.h"
#include "wsl_proto.h"

namespace app {

constexpr int SCREEN_W = 1280, SCREEN_H = 720;
constexpr int MAP_GRID = 40;
constexpr int HIST_N = 240;          // 60 s at 4 Hz
constexpr int MAX_DEVS = 300;
constexpr int MAX_SURVEY = 48;
constexpr uint32_t CAL_MS = 12000;

enum Screen : uint8_t { SCR_SCAN, SCR_HUNT, SCR_MAP, SCR_NODES };

struct Node {
  bool present = false;
  uint8_t chip = 0;
  uint8_t fails = 0;
  uint32_t errors = 0;
  uint32_t lastOkMs = 0;
  uint8_t lastSeq = 0;
  wsl::RssiFilter filt;
  float cal = 0;       // dB added to this node's readings
  float angle = 0;     // where its antenna points, degrees clockwise from the Tab5's top edge
  float mx = 0, my = 0;  // map position in metres (remote nodes)
};

struct Dev {
  uint8_t mac[6];
  uint8_t kind = 0;
  uint8_t channel = 0;
  uint8_t ssidMask = 0;   // which wired nodes know this AP's name
  char ssid[33] = {0};
  int8_t rssi[WSL_MAX_WIRED];
  uint32_t seenMs[WSL_MAX_WIRED];
  int8_t best = -128;
  uint32_t lastMs = 0;
};

struct Btn {
  int16_t x, y, w, h;
  int id;
};

struct App {
  Screen screen = SCR_SCAN;
  Node nodes[WSL_MAX_NODES];
  bool anglesCustom = false;

  // Scan
  std::vector<Dev> devs;
  std::vector<uint16_t> order;   // indices into devs, filtered and sorted for display
  int filter = 0;                // 0 all, 1 access points, 2 clients
  int scrollY = 0;
  bool hasSel = false;
  uint8_t selMac[6] = {0};
  uint32_t lastSortMs = 0;

  // Target
  bool hasTarget = false;
  uint8_t target[6] = {0};
  uint8_t targetCh = 1;
  uint8_t targetKind = 0;
  char targetName[33] = {0};

  // Hunt
  float headRssi = -100;         // strongest calibrated wired reading
  bool headLive = false;
  wsl::Bearing bearing;          // latest raw estimate
  float bvx = 0, bvy = 0;        // smoothed bearing vector, length = confidence
  int8_t hist[HIST_N];
  int histPos = 0;
  uint32_t lastHistMs = 0;
  bool mute = false;
  bool calRunning = false;
  uint32_t calStartMs = 0;
  float calSum[WSL_MAX_WIRED] = {0};
  uint32_t calN[WSL_MAX_WIRED] = {0};
  uint32_t calTicks = 0;

  // Map
  float mapSize = 20.0f;         // metres across
  float nExp = 3.0f;             // path-loss exponent
  std::vector<wsl::MapPoint> survey;
  wsl::MapResult mapRes;
  float heat[MAP_GRID * MAP_GRID];
  bool heatValid = false;
  uint32_t lastMapMs = 0;

  // UI plumbing
  std::vector<Btn> btns;
  bool tDown = false, tMoved = false, tOnList = false;
  int tx0 = 0, ty0 = 0, tx = 0, ty = 0, tScroll0 = 0, tDragNode = -1;
  char toast[80] = {0};
  uint32_t toastUntil = 0;
  uint32_t lastTickMs = 0;
  bool prefsDirty = false;

  App();
};

// --- data in
void mergeDevice(App& a, int node, const WslDevEntry& e, uint32_t now);
void setSsid(App& a, const uint8_t* mac, const char* ssid, int len);
void feedTrack(App& a, int node, uint32_t count, int32_t sum, uint32_t now);
void nodePresence(App& a, int node, bool present, uint8_t chip, uint32_t now);

// --- logic
void tick(App& a, uint32_t now);
bool tracking(const App& a);                       // nodes should lock onto the target
void desiredMode(const App& a, bool relay, WslModeArgs& out);
void setTarget(App& a, const Dev& d);
int beepIntervalMs(const App& a, int* freqOut);    // 0 = silent

// --- UI
void draw(App& a, lgfx::LovyanGFX& g, uint32_t now);
void touch(App& a, bool pressed, int x, int y, uint32_t now);

}  // namespace app
