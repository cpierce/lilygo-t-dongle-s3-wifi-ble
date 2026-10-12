// The table of access points heard on recent sweeps.
#pragma once
#include <cstddef>
#include <cstdint>

namespace survey {

constexpr int      MAX_APS     = 48;
constexpr uint32_t STALE_SCANS = 3;   // forget an AP after this many missed sweeps

struct Ap {
  uint8_t  bssid[6];
  char     ssid[33];
  int16_t  rssi;       // smoothed over recent scans, dBm
  int16_t  rssiAcc;    // the same at RSSI_ACC_SCALE resolution (what is actually averaged)
  int8_t   lastRssi;   // raw value from the last scan that heard it
  uint8_t  channel;
  uint8_t  second;     // SECOND_NONE (20 MHz), or 40 MHz with the other half SECOND_ABOVE / SECOND_BELOW
  uint8_t  auth;       // AUTH_*
  uint8_t  cipher;     // CIPHER_*, pairwise
  uint8_t  phy;        // PHY_* bits
  bool     wps;
  uint32_t firstSeen;  // scan number
  uint32_t lastSeen;   // scan number
  uint32_t seenCount;  // sweeps that heard it
};

// What one sweep reports about one AP, as the driver's record boils down to.
struct ApRecord {
  uint8_t     bssid[6];
  const char* ssid;
  int         rssi;
  uint8_t     channel;
  uint8_t     second;
  uint8_t     auth;
  uint8_t     cipher;
  uint8_t     phy;
  bool        wps;
};

class WifiTable {
 public:
  Ap       aps[MAX_APS];
  int      count  = 0;
  uint32_t scanNo = 0;

  void clear();
  int  find(const uint8_t* bssid) const;

  // A sweep is: beginSweep(), ingest() for every record, endSweep().
  void      beginSweep() { ++scanNo; }
  // Returns the updated entry, or nullptr when the table is full of entries
  // heard on this very sweep and the record had to be dropped.
  const Ap* ingest(const ApRecord& r);
  // Forget APs missed for STALE_SCANS sweeps and sort strongest first.
  void      endSweep();

  bool fresh(const Ap& a) const { return a.lastSeen == scanNo; }

  // Other APs broadcasting the same SSID as aps[idx], and the best of them.
  struct Siblings { int more; int bestRssi; int bestCh; };
  Siblings siblings(int idx) const;

  // APs per 2.4 GHz channel (index 1..13) and the loudest on each.
  struct ChannelLoad { int count[14]; int strongest[14]; int maxCount; };
  ChannelLoad channelLoad() const;
};

// One JSON log line for an AP, newline-terminated. Returns the length written
// (capped at cap - 1).
size_t formatApLine(char* buf, size_t cap, const Ap& a, uint32_t scanNo, uint32_t ms);

}  // namespace survey
