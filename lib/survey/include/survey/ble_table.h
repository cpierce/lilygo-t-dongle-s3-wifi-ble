// The table of Bluetooth Low Energy devices heard recently, and the decoding
// of what their advertisements carry.
#pragma once
#include <cstddef>
#include <cstdint>

namespace survey {

constexpr int      MAX_BLE      = 48;
constexpr int      MAX_SVC      = 4;      // 16-bit service UUIDs kept per device
constexpr uint32_t BLE_FRESH_MS = 2000;   // heard this recently: drawn in colour
constexpr uint32_t BLE_STALE_MS = 10000;  // forget a device after this long
constexpr uint8_t  ADDR_PUBLIC  = 0;      // addrType of a public address (BLE_ADDR_PUBLIC)

constexpr uint16_t COMPANY_APPLE  = 0x004C;
constexpr uint16_t SVC_EDDYSTONE  = 0xFEAA;

// One advertisement, boiled down. Also the shape handed from the BLE host
// task to the main loop.
struct BleAdv {
  uint8_t  addr[6];      // as printed, most significant byte first
  uint8_t  addrType;
  int8_t   rssi;
  char     name[21];
  char     tag[6];       // maker or beacon type
  bool     haveMfr;
  uint16_t company;      // Bluetooth SIG company id from the manufacturer data
  uint8_t  mfrLen;       // manufacturer data length, bytes
  bool     ibeacon;
  uint16_t major, minor; // iBeacon
  uint16_t svc[MAX_SVC]; // 16-bit service UUIDs
  uint8_t  svcCount;     // of those
  uint8_t  svcLong;      // 32/128-bit service UUIDs, count only
  bool     haveTx;
  int8_t   txPower;
  bool     connectable;
};

struct BleDev {
  uint8_t  addr[6];
  uint8_t  addrType;
  char     name[21];
  char     tag[6];
  int16_t  rssi;         // smoothed over recent advertisements, dBm
  int16_t  rssiAcc;      // the same at RSSI_ACC_SCALE resolution (what is actually averaged)
  int8_t   lastRssi;     // raw value from the last advertisement
  bool     haveMfr;
  uint16_t company;
  uint8_t  mfrLen;
  bool     ibeacon;
  uint16_t major, minor;
  uint16_t svc[MAX_SVC];
  uint8_t  svcCount;
  uint8_t  svcLong;
  bool     haveTx;
  int8_t   txPower;
  bool     connectable;
  uint32_t firstSeenMs;
  uint32_t lastSeenMs;
  uint32_t adverts;      // advertisements heard since firstSeenMs
};

// Bluetooth SIG company identifiers that show up in a typical office, as tags
// short enough for the list's middle column. "" when unknown.
const char* makerTag(uint16_t company);

// Decode a manufacturer-specific data payload (company id first, little
// endian) into a.company / mfrLen / iBeacon fields and the maker tag.
void parseManufacturerData(const uint8_t* m, size_t len, BleAdv& a);
// Record one advertised service UUID: 16-bit ones are kept (up to MAX_SVC),
// longer ones are only counted.
void addService(BleAdv& a, uint16_t uuid16);
void addLongService(BleAdv& a);
// Once everything is in: beacon tags that depend on the service list.
void finishAdv(BleAdv& a);

class BleTable {
 public:
  BleDev devs[MAX_BLE];
  int    count = 0;

  void clear();
  int  find(const uint8_t* addr) const;
  // Merge one advertisement. False when the table is full of devices heard
  // within BLE_FRESH_MS and this one had to be dropped.
  bool ingest(const BleAdv& a, uint32_t now);
  // Forget devices silent for BLE_STALE_MS and sort strongest first.
  void tidy(uint32_t now);

  static bool fresh(const BleDev& d, uint32_t now) { return now - d.lastSeenMs < BLE_FRESH_MS; }
};

// One JSON log line for a device, newline-terminated. Returns the length
// written (capped at cap - 1).
size_t formatBleLine(char* buf, size_t cap, const BleDev& d, uint32_t sweepNo, uint32_t now);

}  // namespace survey
