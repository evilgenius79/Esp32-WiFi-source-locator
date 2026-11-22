// PC preview of the Tab5 screens: feeds the app synthetic nodes/devices and
// writes each screen to a .ppm image. See sim/build.sh.
#include <cstdio>
#include <cstring>
#include <random>

#include "../src/app.h"

static lgfx::LGFX_Sprite canvas;
static std::mt19937 rng(3);

static void shot(app::App& a, uint32_t now, const char* name) {
  app::draw(a, canvas, now);
  char path[256];
  snprintf(path, sizeof(path), "%s.ppm", name);
  FILE* f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", app::SCREEN_W, app::SCREEN_H);
  const uint16_t* px = (const uint16_t*)canvas.getBuffer();   // RGB565, byte-swapped
  for (int i = 0; i < app::SCREEN_W * app::SCREEN_H; i++) {
    const uint16_t v = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    fputc(((v >> 11) & 31) * 255 / 31, f);
    fputc(((v >> 5) & 63) * 255 / 63, f);
    fputc((v & 31) * 255 / 31, f);
  }
  fclose(f);
  printf("wrote %s\n", path);
}

static void tap(app::App& a, int x, int y, uint32_t now) {
  app::touch(a, true, x, y, now);
  app::touch(a, false, x, y, now);
}

static float antennaDb(float off) { return -15.0f * (1 - 0.5f * (1 + cosf(wsl::deg2rad(off)))); }

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : ".";
  char name[256];
  canvas.setColorDepth(16);
  canvas.createSprite(app::SCREEN_W, app::SCREEN_H);
  std::normal_distribution<float> noise(0, 1);

  app::App a;
  uint32_t now = 1000;
  snprintf(name, sizeof(name), "%s/0_no_nodes", out);
  shot(a, now, name);

  for (int i = 0; i < 4; i++) app::nodePresence(a, i, true, i < 2 ? WSL_CHIP_C3 : i == 2 ? WSL_CHIP_S3 : WSL_CHIP_C6, now);

  struct Fake { const char* ssid; uint8_t kind; uint8_t ch; int rssi; };
  const Fake fakes[] = {
      {"HomeNet-5F2A", WSL_KIND_AP, 6, -41},   {"", WSL_KIND_STA, 6, -48},  {"Workshop", WSL_KIND_AP, 11, -55},
      {"", WSL_KIND_STA, 11, -57},             {"", WSL_KIND_AP, 1, -60},   {"NETGEAR-Guest", WSL_KIND_AP, 1, -63},
      {"", WSL_KIND_STA, 6, -66},              {"", WSL_KIND_UNKNOWN, 3, -70}, {"A very long network name here 32", WSL_KIND_AP, 9, -72},
      {"", WSL_KIND_STA, 1, -75},              {"Printer-Setup", WSL_KIND_AP, 6, -78}, {"", WSL_KIND_STA, 13, -81},
      {"CoffeeShop", WSL_KIND_AP, 4, -84},     {"", WSL_KIND_STA, 4, -88},
  };
  int k = 0;
  for (auto& f : fakes) {
    WslDevEntry e = {};
    const uint8_t mac[6] = {(uint8_t)(f.kind == WSL_KIND_STA && (k & 1) ? 0xDA : 0x3C), 0x71, 0xBF, (uint8_t)(k * 37), (uint8_t)(k * 91 + 5), (uint8_t)(k * 13 + 0xA0)};
    memcpy(e.mac, mac, 6);
    e.channel = f.ch;
    e.kind = f.kind | (f.ssid[0] ? WSL_KIND_HAS_SSID : 0);
    for (int n = 0; n < 4; n++) {
      e.rssi = f.rssi - 3 * n;
      app::mergeDevice(a, n, e, now);
    }
    if (f.ssid[0]) app::setSsid(a, mac, f.ssid, strlen(f.ssid));
    k++;
  }
  now += 2000;
  app::tick(a, now);
  snprintf(name, sizeof(name), "%s/1_scan", out);
  shot(a, now, name);

  tap(a, 400, 136 + 60 + 20, now);   // second row
  snprintf(name, sizeof(name), "%s/2_scan_selected", out);
  shot(a, now, name);

  tap(a, 1280 - 496 + 50, 720 - 60, now);   // Hunt
  // Source at 60 degrees, with slow fading.
  for (int step = 0; step < 300; step++) {
    now += 100;
    const float base = -62 + 8 * sinf(step / 40.0f);
    for (int n = 0; n < 4; n++) {
      const int cnt = 3;
      int sum = 0;
      for (int c = 0; c < cnt; c++) sum += (int)lroundf(base + antennaDb(wsl::angleDiff(60, a.nodes[n].angle)) + 2 * noise(rng));
      app::feedTrack(a, n, cnt, sum, now);
    }
    app::tick(a, now);
  }
  snprintf(name, sizeof(name), "%s/3_hunt", out);
  shot(a, now, name);

  tap(a, 8 + 3 * 168 + 80, 30, now);   // Nodes tab
  snprintf(name, sizeof(name), "%s/5_nodes", out);
  shot(a, now, name);

  // Map: two remote nodes plus a walked survey around a source at (13, 6) m.
  tap(a, 8 + 2 * 168 + 80, 30, now);
  const float sx = 13, sy = 6;
  auto rssiAt = [&](float x, float y) { return -38 - 30 * log10f(fmaxf(0.5f, hypotf(x - sx, y - sy))) + 2.5f * noise(rng); };
  app::nodePresence(a, 8, true, 0, now);
  app::nodePresence(a, 9, true, 0, now);
  a.nodes[8].mx = 3;  a.nodes[8].my = 16;
  a.nodes[9].mx = 18; a.nodes[9].my = 15;
  const float walk[][2] = {{2, 2}, {6, 3}, {10, 2}, {17, 3}, {18, 9}, {9, 9}, {4, 10}};
  for (auto& w : walk) {
    for (int step = 0; step < 30; step++) {
      now += 100;
      const float r = rssiAt(w[0], w[1]);
      for (int n = 0; n < 4; n++) app::feedTrack(a, n, 2, 2 * (int)lroundf(r - 2 * n), now);
      app::feedTrack(a, 8, 2, 2 * (int)lroundf(rssiAt(3, 16)), now);
      app::feedTrack(a, 9, 2, 2 * (int)lroundf(rssiAt(18, 15)), now);
      app::nodePresence(a, 8, true, 0, now);
      app::nodePresence(a, 9, true, 0, now);
      app::tick(a, now);
    }
    tap(a, 16 + (int)(w[0] / 20 * 624), 80 + (int)(w[1] / 20 * 624), now);
  }
  now += 600;
  app::tick(a, now);
  printf("map estimate %.1f, %.1f (true %.0f, %.0f) rms %.1f dB\n", a.mapRes.x, a.mapRes.y, sx, sy, a.mapRes.rmsDb);
  snprintf(name, sizeof(name), "%s/4_map", out);
  shot(a, now, name);
  return 0;
}
