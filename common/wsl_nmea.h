// WiFi Source Locator - NMEA 0183 reader for any GPS / GNSS / RTK receiver,
// plus the conversion between latitude/longitude and local metres.
// Plain C++ so it is tested on a PC.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace wsl {

struct GpsFix {
  bool valid = false;        // position is usable
  uint8_t quality = 0;       // GGA: 0 none, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float, 6 dead reckoning
  uint8_t sats = 0;
  float hdop = 99.0f;
  double lat = 0, lon = 0;   // degrees, + = north / east
  float alt = 0;             // metres
  float speed = 0;           // m/s over ground
  float course = 0;          // degrees true, direction of travel
  bool courseValid = false;
};

inline const char* gpsQualityName(uint8_t q) {
  switch (q) {
    case 0: return "no fix";
    case 1: return "GPS";
    case 2: return "DGPS";
    case 4: return "RTK fixed";
    case 5: return "RTK float";
    case 6: return "dead reckoning";
    default: return "fix";
  }
}

// Feed it the receiver's bytes one at a time. Understands GGA (position, fix
// quality, satellites) and RMC (speed, course) from any constellation prefix
// ($GP, $GN, $GA ...). Sentences without a valid checksum are ignored.
class NmeaParser {
 public:
  enum Result { NONE = 0, SENTENCE = 1, POSITION = 2 };

  GpsFix fix;
  uint32_t good = 0;   // sentences with a valid checksum
  uint32_t bad = 0;    // sentences that failed it

  void reset() { *this = NmeaParser(); }

  Result feed(char c) {
    if (c == '$') {
      len_ = 0;
      buf_[len_++] = c;
      return NONE;
    }
    if (!len_) return NONE;
    if (c == '\r' || c == '\n') {
      buf_[len_] = 0;
      const Result r = finish();
      len_ = 0;
      return r;
    }
    if (len_ >= (int)sizeof(buf_) - 1) {   // too long to be NMEA
      len_ = 0;
      return NONE;
    }
    buf_[len_++] = c;
    return NONE;
  }

 private:
  char buf_[128];
  int len_ = 0;
  bool sawGga_ = false;

  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  // "ddmm.mmmm" / "dddmm.mmmm" -> degrees
  static bool angle(const char* f, const char* hemi, double& out) {
    if (!f[0] || !hemi[0]) return false;
    const double v = strtod(f, nullptr);
    const double deg = floor(v / 100.0);
    double a = deg + (v - deg * 100.0) / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') a = -a;
    else if (hemi[0] != 'N' && hemi[0] != 'E') return false;
    out = a;
    return true;
  }

  Result finish() {
    // $<body>*HH
    char* star = strrchr(buf_, '*');
    if (!star || star - buf_ < 6 || hex(star[1]) < 0 || hex(star[2]) < 0) return NONE;
    uint8_t sum = 0;
    for (const char* p = buf_ + 1; p < star; p++) sum ^= (uint8_t)*p;
    if (sum != (uint8_t)(hex(star[1]) * 16 + hex(star[2]))) {
      bad++;
      return NONE;
    }
    good++;
    *star = 0;

    // Split on commas, keeping empty fields.
    const char* f[24];
    int n = 0;
    f[n++] = buf_ + 1;
    for (char* p = buf_ + 1; *p && n < 24; p++) {
      if (*p == ',') {
        *p = 0;
        f[n++] = p + 1;
      }
    }
    if (strlen(f[0]) != 5) return SENTENCE;
    const char* type = f[0] + 2;

    if (!strcmp(type, "GGA") && n >= 10) {
      sawGga_ = true;
      fix.quality = (uint8_t)atoi(f[6]);
      fix.sats = (uint8_t)atoi(f[7]);
      if (f[8][0]) fix.hdop = strtof(f[8], nullptr);
      double lat, lon;
      if (fix.quality > 0 && angle(f[2], f[3], lat) && angle(f[4], f[5], lon)) {
        fix.lat = lat;
        fix.lon = lon;
        if (f[9][0]) fix.alt = strtof(f[9], nullptr);
        fix.valid = true;
        return POSITION;
      }
      fix.valid = false;
      return SENTENCE;
    }

    if (!strcmp(type, "RMC") && n >= 9) {
      const bool ok = f[2][0] == 'A';
      if (ok && f[7][0]) fix.speed = strtof(f[7], nullptr) * 0.514444f;   // knots -> m/s
      fix.courseValid = ok && f[8][0];
      if (fix.courseValid) fix.course = strtof(f[8], nullptr);
      if (sawGga_) return SENTENCE;   // GGA is the better position source when the receiver sends both
      double lat, lon;
      if (ok && angle(f[3], f[4], lat) && angle(f[5], f[6], lon)) {
        fix.lat = lat;
        fix.lon = lon;
        fix.quality = 1;
        fix.valid = true;
        return POSITION;
      }
      fix.valid = false;
      fix.quality = 0;
    }
    return SENTENCE;
  }
};

// ---------------------------------------------------------------- local metres

// Flat-earth conversion around an origin: x = metres east, y = metres north.
// Good to a few centimetres over the few hundred metres this tool works at.
static constexpr double kEarthR = 6371008.8;
static constexpr double kDegRad = 3.14159265358979323846 / 180.0;

inline void geoToLocal(double lat, double lon, double lat0, double lon0, float& x, float& y) {
  x = (float)((lon - lon0) * kDegRad * kEarthR * cos(lat0 * kDegRad));
  y = (float)((lat - lat0) * kDegRad * kEarthR);
}

inline void localToGeo(float x, float y, double lat0, double lon0, double& lat, double& lon) {
  lat = lat0 + (double)y / kEarthR / kDegRad;
  lon = lon0 + (double)x / (kEarthR * cos(lat0 * kDegRad)) / kDegRad;
}

inline const char* compassName(float bearingDeg) {
  static const char* const NAMES[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  const float d = fmodf(fmodf(bearingDeg, 360.0f) + 360.0f, 360.0f);
  return NAMES[(int)((d + 22.5f) / 45.0f) % 8];
}

}  // namespace wsl
