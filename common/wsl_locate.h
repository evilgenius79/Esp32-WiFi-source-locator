// WiFi Source Locator - the locating math. Plain C++, no hardware dependencies,
// so it is unit-tested on a PC (see test/).
#pragma once
#include <math.h>
#include <stdint.h>

namespace wsl {

static constexpr float kPi = 3.14159265358979f;
inline float deg2rad(float d) { return d * kPi / 180.0f; }
inline float rad2deg(float r) { return r * 180.0f / kPi; }
inline float wrap360(float d) {
  d = fmodf(d, 360.0f);
  return d < 0 ? d + 360.0f : d;
}
// Smallest signed difference a - b, in (-180, 180].
inline float angleDiff(float a, float b) {
  float d = wrap360(a - b);
  return d > 180.0f ? d - 360.0f : d;
}

// ---------------------------------------------------------------- RSSI smoothing

// Turns the bursty per-poll (count, sum) pairs from a node into a steady dBm
// reading. Individual WiFi frames easily swing +-6 dB from multipath, so this
// is an exponential average whose time constant is tauMs.
struct RssiFilter {
  float value = -100.0f;
  bool primed = false;
  uint32_t lastFrameMs = 0;
  uint32_t lastUpdateMs = 0;
  float rate = 0;  // frames per second, smoothed

  void reset() { *this = RssiFilter(); }

  void update(uint32_t count, int32_t sum, uint32_t nowMs, float tauMs = 600.0f) {
    const float dt = primed || lastUpdateMs ? (float)(nowMs - lastUpdateMs) : 100.0f;
    lastUpdateMs = nowMs;
    if (dt > 0) {
      const float inst = count * 1000.0f / dt;
      rate += (inst - rate) * fminf(1.0f, dt / 1000.0f);
    }
    if (!count) return;
    const float mean = (float)sum / (float)count;
    if (!primed || nowMs - lastFrameMs > 3000) {
      value = mean;
      primed = true;
    } else {
      const float a = 1.0f - expf(-dt / tauMs);
      value += (mean - value) * a;
    }
    lastFrameMs = nowMs;
  }

  // "live" = we have heard the target recently enough to trust the value.
  bool live(uint32_t nowMs, uint32_t timeoutMs = 3000) const {
    return primed && nowMs - lastFrameMs < timeoutMs;
  }
};

// ---------------------------------------------------------------- direction finding

struct Bearing {
  bool valid = false;
  float deg = 0;         // clockwise from "forward" (top edge of the Tab5)
  float confidence = 0;  // 0..1
  float spreadDb = 0;    // strongest minus weakest antenna
};

// Amplitude-comparison direction finding. Each node has a directional antenna
// pointing at angleDeg[i]; the source is roughly where the strong antennas
// point. Every antenna votes for its own direction with a weight that falls
// off by 10x for every `dbPerDecade` dB it is below the strongest one.
inline Bearing estimateBearing(const float* angleDeg, const float* rssi, const bool* valid, int n,
                               float dbPerDecade = 12.0f) {
  Bearing b;
  int used = 0;
  float rmax = -1e9f, rmin = 1e9f;
  for (int i = 0; i < n; i++) {
    if (!valid[i]) continue;
    used++;
    if (rssi[i] > rmax) rmax = rssi[i];
    if (rssi[i] < rmin) rmin = rssi[i];
  }
  if (used < 2) return b;
  float vx = 0, vy = 0, wsum = 0;
  for (int i = 0; i < n; i++) {
    if (!valid[i]) continue;
    const float w = powf(10.0f, (rssi[i] - rmax) / dbPerDecade);
    vx += w * sinf(deg2rad(angleDeg[i]));
    vy += w * cosf(deg2rad(angleDeg[i]));
    wsum += w;
  }
  const float mag = sqrtf(vx * vx + vy * vy);
  b.spreadDb = rmax - rmin;
  if (mag < 1e-6f) return b;
  b.valid = true;
  b.deg = wrap360(rad2deg(atan2f(vx, vy)));
  // Confident when the antennas disagree clearly (several dB of spread) and
  // the votes line up rather than cancel.
  const float agree = mag / wsum;
  const float contrast = fminf(1.0f, b.spreadDb / 8.0f);
  b.confidence = fminf(1.0f, agree * 1.5f) * contrast;
  return b;
}

// Spin calibration: while the user turns a full circle every antenna sees the
// same average signal, so any difference between the per-node means is
// receiver/antenna gain mismatch. Returns the dB to add to each node.
inline void spinCalOffsets(const float* mean, const bool* valid, int n, float* offsetOut) {
  float total = 0;
  int used = 0;
  for (int i = 0; i < n; i++)
    if (valid[i]) { total += mean[i]; used++; }
  for (int i = 0; i < n; i++) offsetOut[i] = (valid[i] && used) ? total / used - mean[i] : 0.0f;
}

// ---------------------------------------------------------------- map solver

struct MapPoint {
  float x, y;   // metres
  float rssi;   // dBm measured at that spot
};

struct MapResult {
  bool valid = false;
  float x = 0, y = 0;
  float txDbm = 0;   // fitted RSSI at 1 m
  float rmsDb = 0;   // how well the model fits the readings
};

// Sum of squared error (dB^2) of the best log-distance fit with the source at (x, y):
//   rssi = A - 10 * nExp * log10(distance)
// A (the source's power) is unknown, so it is solved for at each candidate spot.
inline float mapSse(const MapPoint* p, int n, float x, float y, float nExp, float* aOut = nullptr) {
  float l[64];
  if (n > 64) n = 64;
  float a = 0;
  for (int i = 0; i < n; i++) {
    const float dx = p[i].x - x, dy = p[i].y - y;
    const float d2 = fmaxf(dx * dx + dy * dy, 0.25f);   // clamp to 0.5 m
    l[i] = 5.0f * nExp * log10f(d2);
    a += p[i].rssi + l[i];
  }
  a /= n;
  float sse = 0;
  for (int i = 0; i < n; i++) {
    const float e = p[i].rssi - (a - l[i]);
    sse += e * e;
  }
  if (aOut) *aOut = a;
  return sse;
}

// Grid search over the square [x0, x0+size] x [y0, y0+size]. If sseGrid is
// given (grid*grid floats, row-major, y first) it receives the error surface
// for drawing a heat map. Needs at least 3 readings from different places.
inline MapResult solveMap(const MapPoint* p, int n, float x0, float y0, float size, float nExp,
                          int grid, float* sseGrid = nullptr) {
  MapResult r;
  if (n < 3 || grid < 2) return r;
  const float step = size / grid;
  float best = 1e30f;
  int bi = 0, bj = 0;
  for (int j = 0; j < grid; j++) {
    for (int i = 0; i < grid; i++) {
      const float sse = mapSse(p, n, x0 + (i + 0.5f) * step, y0 + (j + 0.5f) * step, nExp);
      if (sseGrid) sseGrid[j * grid + i] = sse;
      if (sse < best) { best = sse; bi = i; bj = j; }
    }
  }
  // Refine inside the winning cell and its neighbours.
  const float cx = x0 + (bi + 0.5f) * step, cy = y0 + (bj + 0.5f) * step;
  r.x = cx; r.y = cy;
  const int sub = 8;
  for (int j = -sub; j <= sub; j++) {
    for (int i = -sub; i <= sub; i++) {
      const float x = cx + i * step / sub, y = cy + j * step / sub;
      if (x < x0 || x > x0 + size || y < y0 || y > y0 + size) continue;
      const float sse = mapSse(p, n, x, y, nExp);
      if (sse < best) { best = sse; r.x = x; r.y = y; }
    }
  }
  mapSse(p, n, r.x, r.y, nExp, &r.txDbm);
  r.rmsDb = sqrtf(best / n);
  r.valid = true;
  return r;
}

}  // namespace wsl
