// PC preview of the Tab5 screens: feeds the app synthetic nodes/devices and
// writes each screen to a .ppm image. See sim/build.sh.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

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
  app::draw(a, canvas, now);   // buttons exist once they have been drawn
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

  // GPS: walk an L-shaped path past a source, fed as real NMEA text at 5 Hz.
  int rc = 0;
  {
    app::App b;
    uint32_t t = 1000;
    for (int i = 0; i < 4; i++) app::nodePresence(b, i, true, WSL_CHIP_C3, t);
    app::gpsStatus(b, true, "G38 @ 115200");
    WslDevEntry e = {};
    const uint8_t mac[6] = {0x3C, 0x71, 0xBF, 0x10, 0x20, 0x30};
    memcpy(e.mac, mac, 6);
    e.channel = 11;
    e.kind = WSL_KIND_AP | WSL_KIND_HAS_SSID;
    e.rssi = -60;
    app::mergeDevice(b, 0, e, t);
    app::setSsid(b, mac, "Workshop", 8);
    t += 2000;
    app::tick(b, t);
    tap(b, 400, 136 + 20, t);                 // select it
    tap(b, 1280 - 248 + 50, 720 - 60, t);     // Map

    const double lat0 = 47.6205, lon0 = -122.3493;
    const float srcX = 28, srcY = 24;         // metres east / north of the start
    wsl::NmeaParser parser;
    auto sentence = [](const char* body) {
      uint8_t sum = 0;
      for (const char* p = body; *p; p++) sum ^= (uint8_t)*p;
      char out[200];
      snprintf(out, sizeof(out), "$%s*%02X\r\n", body, sum);
      return std::string(out);
    };
    float x = 0, y = 0;
    for (int step = 0; step < 5 * 62; step++) {   // 62 s at 1.3 m/s: 45 m east, then 35 m north
      t += 200;
      const bool east = x < 45;
      (east ? x : y) += 1.3f * 0.2f;
      double lat, lon;
      wsl::localToGeo(x + 0.02f * noise(rng), y + 0.02f * noise(rng), lat0, lon0, lat, lon);
      char body[160];
      const double alat = fabs(lat), alon = fabs(lon);
      snprintf(body, sizeof(body), "GNGGA,120000.00,%02d%010.7f,%c,%03d%010.7f,%c,4,26,0.6,56.0,M,-18.0,M,1.0,0000",
               (int)alat, (alat - (int)alat) * 60, lat >= 0 ? 'N' : 'S', (int)alon, (alon - (int)alon) * 60,
               lon >= 0 ? 'E' : 'W');
      std::string text = sentence(body);
      snprintf(body, sizeof(body), "GNRMC,120000.00,A,%02d%010.7f,%c,%03d%010.7f,%c,2.53,%05.1f,221126,,,R",
               (int)alat, (alat - (int)alat) * 60, lat >= 0 ? 'N' : 'S', (int)alon, (alon - (int)alon) * 60,
               lon >= 0 ? 'E' : 'W', east ? 90.0 : 0.0);
      text += sentence(body);
      for (char c : text)
        if (parser.feed(c) != wsl::NmeaParser::NONE) app::gpsUpdate(b, parser.fix, t);

      for (int half = 0; half < 2; half++) {
        const uint32_t tt = t + half * 100;
        const float d = fmaxf(0.5f, hypotf(x - srcX, y - srcY));
        const float base = -36 - 30 * log10f(d) + 2.5f * noise(rng);
        // Antennas face travel direction (0), right, back, left; source bearing relative to travel.
        const float rel = wsl::rad2deg(atan2f(srcX - x, srcY - y)) - (east ? 90.0f : 0.0f);
        for (int n = 0; n < 4; n++)
          app::feedTrack(b, n, 3, 3 * (int)lroundf(base + antennaDb(wsl::angleDiff(rel, b.nodes[n].angle))), tt);
        app::tick(b, tt);
      }
    }
    const float err = hypotf(b.mapRes.x - srcX, b.mapRes.y - srcY);
    printf("gps map: %d spots, estimate %.1f, %.1f (true %.0f, %.0f), error %.1f m, view %g m\n", (int)b.survey.size(),
           b.mapRes.x, b.mapRes.y, srcX, srcY, err, b.vspan);
    if (!b.mapRes.valid || err > 8 || !b.surveyGps || !b.gps.fix) {
      printf("FAIL: GPS mapping did not locate the source\n");
      rc = 1;
    }
    snprintf(name, sizeof(name), "%s/6_map_gps", out);
    shot(b, t, name);
    tap(b, 8 + 3 * 168 + 80, 30, t);   // Nodes tab shows the receiver status
    snprintf(name, sizeof(name), "%s/7_nodes_gps", out);
    shot(b, t, name);
  }
  return rc;
}
