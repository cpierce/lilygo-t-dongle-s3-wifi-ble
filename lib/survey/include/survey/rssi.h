// Signal-strength bands shared by the screen, the LED and the log.
#pragma once

namespace survey {

// RSSI bands in dBm, best to worst: green / yellow / orange / red, and below
// `poor` the bar meter is empty.
struct RssiScale { int great, good, fair, poor; };

// Wi-Fi: -67 is the usual floor for voice/video to work well, and anything
// below -75 is where clients start roaming or dropping.
constexpr RssiScale WIFI_SCALE = {-60, -67, -75, -85};
// BLE: a phone or beacon transmits ~15 dB quieter than an AP, so the bands
// sit lower. -75 is a comfortable floor for beacon positioning.
constexpr RssiScale BLE_SCALE  = {-65, -75, -85, -92};

enum class Band { Edge = 0, Poor = 1, Fair = 2, Good = 3, Great = 4 };

inline Band rssiBand(int r, const RssiScale& s) {
  if (r >= s.great) return Band::Great;
  if (r >= s.good)  return Band::Good;
  if (r >= s.fair)  return Band::Fair;
  if (r >= s.poor)  return Band::Poor;
  return Band::Edge;
}

// Bars on the 4-bar meter.
inline int rssiBars(int r, const RssiScale& s) { return static_cast<int>(rssiBand(r, s)); }

inline const char* rssiWord(int r, const RssiScale& s) {
  switch (rssiBand(r, s)) {
    case Band::Great: return "GREAT";
    case Band::Good:  return "GOOD";
    case Band::Fair:  return "FAIR";
    case Band::Poor:  return "POOR";
    default:          return "EDGE";
  }
}

// Every RSSI the tables show is an exponential average: 3 parts old, 1 part
// new. It is kept at 16x resolution because the plain integer form,
// (old * 3 + fresh) / 4, truncates toward zero and so never quite reaches a
// weaker level: a signal that fades to a steady -70 would read -67 for ever,
// on the wrong side of the "good" line.
constexpr int RSSI_ACC_SCALE = 16;
inline int rssiAccFrom(int dbm) { return dbm * RSSI_ACC_SCALE; }
inline int smoothRssiAcc(int acc, int fresh) { return (acc * 3 + fresh * RSSI_ACC_SCALE) / 4; }
inline int rssiFromAcc(int acc) {   // nearest dBm, halves away from zero
  return (acc < 0 ? acc - RSSI_ACC_SCALE / 2 : acc + RSSI_ACC_SCALE / 2) / RSSI_ACC_SCALE;
}

}  // namespace survey
