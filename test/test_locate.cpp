// Host-side tests for the locating math and the wire protocol.
//   g++ -std=c++17 -I common test/test_locate.cpp -o /tmp/test_locate && /tmp/test_locate
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include <vector>

#include "wsl_frame.h"
#include "wsl_locate.h"
#include "wsl_nmea.h"
#include "wsl_proto.h"

static int failures = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      failures++;                                         \
      printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

// Directional antenna model: cardioid-ish pattern with `fb` dB front-to-back ratio.
static float antennaGainDb(float offAxisDeg, float fb) {
  const float c = 0.5f * (1.0f + cosf(wsl::deg2rad(offAxisDeg)));   // 1 on axis, 0 behind
  return -fb * (1.0f - c);
}

static void testBearing() {
  std::mt19937 rng(42);
  std::normal_distribution<float> noise(0.0f, 1.0f);
  for (int n : {3, 4, 6}) {
    for (float fb : {8.0f, 15.0f, 25.0f}) {
      float ang[8];
      bool valid[8];
      for (int i = 0; i < n; i++) { ang[i] = i * 360.0f / n; valid[i] = true; }
      float worstClean = 0, sumNoisy = 0;
      int cnt = 0;
      for (int src = 0; src < 360; src += 3) {
        float r[8];
        for (int i = 0; i < n; i++) r[i] = -60 + antennaGainDb(wsl::angleDiff(src, ang[i]), fb);
        wsl::Bearing b = wsl::estimateBearing(ang, r, valid, n);
        CHECK(b.valid, "n=%d fb=%.0f src=%d", n, fb, src);
        worstClean = fmaxf(worstClean, fabsf(wsl::angleDiff(b.deg, src)));
        for (int k = 0; k < 20; k++) {
          float rn[8];
          for (int i = 0; i < n; i++) rn[i] = r[i] + 1.5f * noise(rng);
          wsl::Bearing bn = wsl::estimateBearing(ang, rn, valid, n);
          sumNoisy += fabsf(wsl::angleDiff(bn.deg, src));
          cnt++;
        }
      }
      printf("bearing  n=%d front/back=%2.0f dB  worst clean err %5.1f deg, mean err with 1.5 dB noise %5.1f deg\n",
             n, fb, worstClean, sumNoisy / cnt);
      CHECK(worstClean < 30.0f, "n=%d fb=%.0f worst=%.1f", n, fb, worstClean);
      CHECK(sumNoisy / cnt < 30.0f, "n=%d fb=%.0f", n, fb);
    }
  }

  // Omni antennas (no directionality) must not produce a confident bearing.
  float ang[4] = {0, 90, 180, 270}, r[4] = {-60, -60.5f, -59.8f, -60.2f};
  bool valid[4] = {true, true, true, true};
  wsl::Bearing b = wsl::estimateBearing(ang, r, valid, 4);
  CHECK(b.confidence < 0.15f, "omni confidence %.2f", b.confidence);

  // One live node is not enough for a direction.
  bool one[4] = {true, false, false, false};
  CHECK(!wsl::estimateBearing(ang, r, one, 4).valid, "single node");

  // A dead node is ignored, the rest still work.
  float r2[4] = {-50, -58, 0, -58};
  bool three[4] = {true, true, false, true};
  b = wsl::estimateBearing(ang, r2, three, 4);
  CHECK(b.valid && fabsf(wsl::angleDiff(b.deg, 0)) < 5, "deg %.1f", b.deg);
}

static void testSpinCal() {
  float mean[4] = {-60, -64, -58, -62};
  bool valid[4] = {true, true, true, true};
  float off[4];
  wsl::spinCalOffsets(mean, valid, 4, off);
  for (int i = 0; i < 4; i++) CHECK(fabsf(mean[i] + off[i] + 61.0f) < 1e-4f, "node %d", i);
}

static void testFilter() {
  wsl::RssiFilter f;
  CHECK(!f.live(0), "not primed");
  uint32_t t = 1000;
  f.update(5, -5 * 70, t);
  CHECK(f.live(t) && fabsf(f.value + 70) < 0.01f, "primed at first mean, %.1f", f.value);
  for (int i = 0; i < 50; i++) { t += 100; f.update(3, -3 * 50, t); }
  CHECK(fabsf(f.value + 50) < 0.5f, "converges, %.1f", f.value);
  CHECK(fabsf(f.rate - 30) < 3, "rate %.1f", f.rate);
  for (int i = 0; i < 40; i++) { t += 100; f.update(0, 0, t); }
  CHECK(!f.live(t), "times out when the target goes quiet");
  f.update(1, -80, t);
  CHECK(fabsf(f.value + 80) < 0.01f, "re-primes after silence instead of averaging stale data");
}

static void testMap() {
  std::mt19937 rng(7);
  std::normal_distribution<float> noise(0.0f, 1.0f);
  std::uniform_real_distribution<float> uni(0.0f, 20.0f);
  const float nExp = 3.0f;
  static float grid[48 * 48];

  // Exact data: must land on the source.
  {
    const float sx = 13.0f, sy = 6.0f;
    wsl::MapPoint p[5] = {{1, 1, 0}, {19, 2, 0}, {18, 18, 0}, {2, 17, 0}, {10, 10, 0}};
    for (auto& q : p) q.rssi = -40 - 10 * nExp * log10f(hypotf(q.x - sx, q.y - sy));
    wsl::MapResult r = wsl::solveMap(p, 5, 0, 0, 20, nExp, 48, grid);
    CHECK(r.valid && hypotf(r.x - sx, r.y - sy) < 0.3f, "got %.2f,%.2f", r.x, r.y);
    CHECK(fabsf(r.txDbm + 40) < 1.0f, "tx %.1f", r.txDbm);
    CHECK(r.rmsDb < 0.5f, "rms %.2f", r.rmsDb);
  }

  // Noisy data (4 dB sigma is typical indoors): report the error honestly.
  for (int npts : {4, 8, 16}) {
    float sum = 0;
    const int trials = 300;
    for (int t = 0; t < trials; t++) {
      const float sx = uni(rng), sy = uni(rng);
      wsl::MapPoint p[16];
      for (int i = 0; i < npts; i++) {
        p[i].x = uni(rng);
        p[i].y = uni(rng);
        const float d = fmaxf(0.5f, hypotf(p[i].x - sx, p[i].y - sy));
        p[i].rssi = -40 - 10 * nExp * log10f(d) + 4.0f * noise(rng);
      }
      wsl::MapResult r = wsl::solveMap(p, npts, 0, 0, 20, nExp, 48);
      sum += hypotf(r.x - sx, r.y - sy);
    }
    printf("map      %2d readings, 4 dB noise, 20 m area: mean position error %.1f m\n", npts, sum / trials);
    if (npts >= 8) CHECK(sum / trials < 5.0f, "npts=%d err=%.1f", npts, sum / trials);
  }

  wsl::MapPoint two[2] = {{0, 0, -50}, {5, 5, -60}};
  CHECK(!wsl::solveMap(two, 2, 0, 0, 20, nExp, 48).valid, "needs 3 points");
}

static void testProto() {
  CHECK(sizeof(WslStatusResp) == 13, "%zu", sizeof(WslStatusResp));
  CHECK(sizeof(WslDevEntry) == 12, "%zu", sizeof(WslDevEntry));
  CHECK(sizeof(WslDevResp) == 198, "%zu", sizeof(WslDevResp));
  CHECK(sizeof(WslRemoteEntry) == 10, "%zu", sizeof(WslRemoteEntry));
  CHECK(sizeof(WslNowReport) <= 250 && sizeof(WslNowBeacon) <= 250, "ESP-NOW payload limit");
  WslTrackResp r = {};
  r.magic = WSL_MAGIC;
  r.cmd = WSL_CMD_GET_TRACK;
  r.count = 7;
  r.sum = -420;
  wsl_seal(r);
  CHECK(wsl_check(r, WSL_CMD_GET_TRACK), "sealed response verifies");
  CHECK(!wsl_check(r, WSL_CMD_GET_STATUS), "wrong cmd rejected");
  r.sum = -421;
  CHECK(!wsl_check(r, WSL_CMD_GET_TRACK), "corruption rejected");
  uint8_t idle[sizeof(WslTrackResp)];
  memset(idle, 0xFF, sizeof(idle));   // what a floating I2C bus reads as
  CHECK(!wsl_check(*(WslTrackResp*)idle, WSL_CMD_GET_TRACK), "all-ones rejected");
  memset(idle, 0, sizeof(idle));
  CHECK(!wsl_check(*(WslTrackResp*)idle, WSL_CMD_GET_TRACK), "all-zeros rejected");
}

// Build an 802.11 frame: fc0/fc1, 3 addresses, body, then 4 FCS bytes.
static std::vector<uint8_t> frame(uint8_t fc0, uint8_t fc1, const uint8_t* a1, const uint8_t* a2, const uint8_t* a3,
                                  std::vector<uint8_t> body = {}) {
  std::vector<uint8_t> f = {fc0, fc1, 0, 0};
  f.insert(f.end(), a1, a1 + 6);
  if (a2) f.insert(f.end(), a2, a2 + 6);
  if (a3) {
    f.insert(f.end(), a3, a3 + 6);
    f.push_back(0);
    f.push_back(0);   // sequence control
  }
  f.insert(f.end(), body.begin(), body.end());
  f.insert(f.end(), 4, 0xEE);   // FCS
  return f;
}

static void testFrames() {
  const uint8_t ap[6] = {0x3C, 0x71, 0xBF, 1, 2, 3}, sta[6] = {0xDA, 0x10, 0x20, 4, 5, 6};
  const uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  WslFrameInfo fi;

  // Beacon: 12 fixed bytes, SSID "Home", rates, DS channel 6.
  std::vector<uint8_t> body(12, 0);
  const uint8_t ies[] = {0, 4, 'H', 'o', 'm', 'e', 1, 2, 0x82, 0x84, 3, 1, 6};
  body.insert(body.end(), ies, ies + sizeof(ies));
  auto f = frame(0x80, 0x00, bc, ap, ap, body);
  CHECK(wsl_parse_frame(f.data(), f.size(), fi), "beacon parses");
  CHECK(fi.kind == WSL_KIND_AP && !memcmp(fi.ta, ap, 6), "beacon is an AP, TA = BSSID");
  CHECK(fi.ssidLen == 4 && !memcmp(fi.ssid, "Home", 4), "ssid len %u", fi.ssidLen);
  CHECK(fi.apChannel == 6, "ds channel %u", fi.apChannel);

  // Truncated / lying information element must not read past the frame.
  body.assign(12, 0);
  body.push_back(0);
  body.push_back(200);   // claims 200 bytes of SSID, none present
  f = frame(0x80, 0x00, bc, ap, ap, body);
  CHECK(wsl_parse_frame(f.data(), f.size(), fi) && fi.ssidLen == 0, "bad IE ignored");

  // Hidden network: SSID IE present but zero-filled.
  body.assign(12, 0);
  const uint8_t hidden[] = {0, 4, 0, 0, 0, 0};
  body.insert(body.end(), hidden, hidden + sizeof(hidden));
  f = frame(0x80, 0x00, bc, ap, ap, body);
  CHECK(wsl_parse_frame(f.data(), f.size(), fi) && fi.ssidLen == 0, "hidden ssid");

  // Data, client -> AP (ToDS): transmitter is the client.
  f = frame(0x88, 0x01, ap, sta, ap, {1, 2, 3, 4});
  CHECK(wsl_parse_frame(f.data(), f.size(), fi) && fi.kind == WSL_KIND_STA && !memcmp(fi.ta, sta, 6), "toDS data");
  // Data, AP -> client (FromDS): transmitter is the AP.
  f = frame(0x08, 0x02, sta, ap, ap, {1, 2, 3, 4});
  CHECK(wsl_parse_frame(f.data(), f.size(), fi) && fi.kind == WSL_KIND_AP && !memcmp(fi.ta, ap, 6), "fromDS data");
  // Probe request from a client.
  f = frame(0x40, 0x00, bc, sta, bc, {0, 0});
  CHECK(wsl_parse_frame(f.data(), f.size(), fi) && fi.kind == WSL_KIND_STA, "probe request");

  // RTS carries a transmitter; ACK and CTS do not.
  f = frame(0xB4, 0x00, ap, sta, nullptr);
  CHECK(wsl_parse_frame(f.data(), f.size(), fi) && !memcmp(fi.ta, sta, 6) && fi.kind == WSL_KIND_UNKNOWN, "RTS");
  f = frame(0xD4, 0x00, sta, nullptr, nullptr);
  CHECK(!wsl_parse_frame(f.data(), f.size(), fi), "ACK has no transmitter");
  f = frame(0xC4, 0x00, sta, nullptr, nullptr);
  CHECK(!wsl_parse_frame(f.data(), f.size(), fi), "CTS has no transmitter");

  // Our own ESP-NOW frames must not show up as devices.
  f = frame(0xD0, 0x00, bc, sta, bc, {127, 0x18, 0xFE, 0x34, 1, 2, 3, 4});
  CHECK(!wsl_parse_frame(f.data(), f.size(), fi), "ESP-NOW ignored");
  // Group "transmitter" address is bogus.
  f = frame(0x08, 0x01, ap, bc, ap, {1});
  CHECK(!wsl_parse_frame(f.data(), f.size(), fi), "multicast TA rejected");
  // Runt.
  CHECK(!wsl_parse_frame(f.data(), 10, fi), "runt rejected");
}

// Wrap an NMEA body in $...*HH\r\n with the right checksum.
static std::string nmea(const std::string& body) {
  uint8_t sum = 0;
  for (char c : body) sum ^= (uint8_t)c;
  char tail[8];
  snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
  return "$" + body + tail;
}

static int feedAll(wsl::NmeaParser& p, const std::string& text) {
  int best = 0;
  for (char c : text) best = std::max(best, (int)p.feed(c));
  return best;
}

static void testNmea() {
  wsl::NmeaParser p;

  // RTK-fixed GGA from a multi-constellation receiver, with centimetre-level digits.
  CHECK(feedAll(p, nmea("GNGGA,123519.00,4807.0380123,N,01131.0004567,E,4,24,0.6,545.4,M,46.9,M,1.0,0000")) ==
            wsl::NmeaParser::POSITION, "GGA gives a position");
  CHECK(p.fix.valid && p.fix.quality == 4 && p.fix.sats == 24, "q=%u sats=%u", p.fix.quality, p.fix.sats);
  CHECK(fabs(p.fix.lat - (48.0 + 7.0380123 / 60.0)) < 1e-9, "lat %.9f", p.fix.lat);
  CHECK(fabs(p.fix.lon - (11.0 + 31.0004567 / 60.0)) < 1e-9, "lon %.9f", p.fix.lon);
  CHECK(fabsf(p.fix.hdop - 0.6f) < 1e-4f && fabsf(p.fix.alt - 545.4f) < 1e-3f, "hdop/alt");
  CHECK(!strcmp(wsl::gpsQualityName(p.fix.quality), "RTK fixed"), "quality name");

  // RMC adds speed and course but must not replace the better GGA position.
  CHECK(feedAll(p, nmea("GNRMC,123520.00,A,4807.0000,N,01131.0000,E,2.72,084.4,230394,,,A")) ==
            wsl::NmeaParser::SENTENCE, "RMC parsed");
  CHECK(fabsf(p.fix.speed - 2.72f * 0.514444f) < 1e-3f && p.fix.courseValid && fabsf(p.fix.course - 84.4f) < 1e-3f,
        "speed %.2f course %.1f", p.fix.speed, p.fix.course);
  CHECK(fabs(p.fix.lat - (48.0 + 7.0380123 / 60.0)) < 1e-9, "RMC left the GGA position alone");

  // Southern / western hemisphere.
  feedAll(p, nmea("GPGGA,010203,3351.2340,S,15112.5670,W,1,08,1.1,12.0,M,0.0,M,,"));
  CHECK(p.fix.lat < 0 && p.fix.lon < 0 && fabs(p.fix.lat + (33.0 + 51.234 / 60.0)) < 1e-9, "S/W negative");

  // No fix: empty fields, quality 0.
  CHECK(feedAll(p, nmea("GNGGA,010204.00,,,,,0,00,99.99,,,,,,")) == wsl::NmeaParser::SENTENCE, "no-fix GGA parses");
  CHECK(!p.fix.valid && p.fix.quality == 0, "no fix reported");

  // Corruption and noise are ignored.
  const uint32_t good = p.good;
  std::string bad = nmea("GNGGA,123519.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
  bad[20] = '9';
  CHECK(feedAll(p, bad) == wsl::NmeaParser::NONE && p.good == good && p.bad == 1, "bad checksum rejected");
  CHECK(feedAll(p, "\xB5\x62\x01\x07garbage\r\n$GNGGA,no,checksum\r\n" + std::string(300, 'x')) ==
            wsl::NmeaParser::NONE, "binary and junk ignored");
  CHECK(!p.fix.valid, "junk did not create a fix");
  CHECK(feedAll(p, nmea("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00")) ==
            wsl::NmeaParser::SENTENCE, "other sentences count as traffic");

  // A receiver that only sends RMC still yields positions.
  wsl::NmeaParser r;
  CHECK(feedAll(r, nmea("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W")) ==
            wsl::NmeaParser::POSITION, "RMC-only position");
  CHECK(r.fix.valid && fabs(r.fix.lat - 48.1173) < 1e-6, "lat %.6f", r.fix.lat);
  feedAll(r, nmea("GPRMC,123520,V,,,,,,,230394,,"));
  CHECK(!r.fix.valid, "RMC void clears the fix");
}

static void testGeo() {
  const double lat0 = 47.6205, lon0 = -122.3493;
  float x, y;
  wsl::geoToLocal(lat0 + 0.001, lon0, lat0, lon0, x, y);
  CHECK(fabsf(y - 111.195f) < 0.05f && fabsf(x) < 1e-3f, "0.001 deg of latitude = %.3f m", y);
  wsl::geoToLocal(lat0, lon0 + 0.001, lat0, lon0, x, y);
  CHECK(fabsf(x - 111.195f * cosf(47.6205f * 3.14159265f / 180)) < 0.05f, "longitude shrinks with latitude: %.3f", x);
  // There and back again stays within a centimetre.
  for (float px : {-350.0f, 12.34f, 480.0f}) {
    for (float py : {-420.0f, 0.01f, 275.5f}) {
      double la, lo;
      wsl::localToGeo(px, py, lat0, lon0, la, lo);
      wsl::geoToLocal(la, lo, lat0, lon0, x, y);
      CHECK(fabsf(x - px) < 0.01f && fabsf(y - py) < 0.01f, "round trip %.3f,%.3f -> %.3f,%.3f", px, py, x, y);
    }
  }
  CHECK(!strcmp(wsl::compassName(0), "N") && !strcmp(wsl::compassName(44), "NE") && !strcmp(wsl::compassName(-90), "W") &&
            !strcmp(wsl::compassName(359), "N") && !strcmp(wsl::compassName(180), "S"), "compass names");
}

int main() {
  testFrames();
  testNmea();
  testGeo();
  testBearing();
  testSpinCal();
  testFilter();
  testMap();
  testProto();
  printf(failures ? "\n%d FAILURE(S)\n" : "\nall tests passed\n", failures);
  return failures ? 1 : 0;
}
