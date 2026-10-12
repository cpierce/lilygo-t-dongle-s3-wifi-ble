#include "survey/ble_table.h"
#include "survey/rssi.h"
#include "survey/text.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace survey {

const char* makerTag(uint16_t company) {
  switch (company) {
    case 0x004C: return "Apple";
    case 0x0006: return "MSFT";
    case 0x00E0: return "Googl";
    case 0x0075: return "Samsg";
    case 0x0087: return "Garmn";
    case 0x0059: return "Nordc";
    case 0x02E5: return "Espr";
    case 0x038F: return "Xiaom";
    case 0x0157: return "Huami";
    case 0x01DA: return "Logi";
    case 0x0171: return "Amazn";
    case 0x012D: return "Sony";
    case 0x009E: return "Bose";
    case 0x0224: return "Fitbt";
    case 0x05A7: return "Sonos";
    case 0x0499: return "Ruuvi";
    case 0x015D: return "Estmt";
    case 0x0118: return "Radus";
    case 0x000D: return "TI";
    case 0x0030: return "STM";
    case 0x0002: return "Intel";
    case 0x000A:
    case 0x001D: return "Qualc";
    case 0x000F: return "Brdcm";
    case 0x0131: return "Cyprs";
    case 0x0822: return "Adafr";
    default:     return "";
  }
}

void parseManufacturerData(const uint8_t* m, size_t len, BleAdv& a) {
  if (len < 2) return;
  a.haveMfr = true;
  a.company = (uint16_t)(m[0] | ((uint16_t)m[1] << 8));
  a.mfrLen  = (uint8_t)std::min<size_t>(len, 255);
  // iBeacon: Apple, subtype 0x02, length 0x15, 16-byte UUID, major, minor, tx.
  if (a.company == COMPANY_APPLE && len >= 25 && m[2] == 0x02 && m[3] == 0x15) {
    a.ibeacon = true;
    a.major = (uint16_t)(((uint16_t)m[20] << 8) | m[21]);
    a.minor = (uint16_t)(((uint16_t)m[22] << 8) | m[23]);
  }
  strCopy(a.tag, sizeof a.tag, a.ibeacon ? "iBcn" : makerTag(a.company));
}

void addService(BleAdv& a, uint16_t uuid16) {
  if (a.svcCount < MAX_SVC) a.svc[a.svcCount++] = uuid16;
}

void addLongService(BleAdv& a) { ++a.svcLong; }

// cppcheck-suppress constParameterReference ; strCopy writes a.tag
void finishAdv(BleAdv& a) {
  if (a.tag[0]) return;
  for (int i = 0; i < a.svcCount; ++i)
    if (a.svc[i] == SVC_EDDYSTONE) { strCopy(a.tag, sizeof a.tag, "Eddy"); return; }
}

void BleTable::clear() { count = 0; }

int BleTable::find(const uint8_t* addr) const {
  for (int i = 0; i < count; ++i)
    if (memcmp(devs[i].addr, addr, 6) == 0) return i;
  return -1;
}

bool BleTable::ingest(const BleAdv& a, uint32_t now) {
  int idx = find(a.addr);
  if (idx < 0) {
    if (count < MAX_BLE) {
      idx = count++;
    } else {
      // Table full: recycle the entry that has gone longest without being heard.
      idx = 0;
      for (int k = 1; k < count; ++k)
        if (devs[k].lastSeenMs < devs[idx].lastSeenMs) idx = k;
      if (fresh(devs[idx], now)) return false;  // everything is fresh, skip this one
    }
    BleDev& d = devs[idx];
    d = BleDev{};
    memcpy(d.addr, a.addr, 6);
    d.rssiAcc = (int16_t)rssiAccFrom(a.rssi);
    d.firstSeenMs = now;
  } else {
    devs[idx].rssiAcc = (int16_t)smoothRssiAcc(devs[idx].rssiAcc, a.rssi);
  }
  // Fields that only some advertisements carry are kept from the last one
  // that had them, so a device's details fill in over time.
  BleDev& d = devs[idx];
  d.rssi = (int16_t)rssiFromAcc(d.rssiAcc);
  d.addrType = a.addrType;
  d.connectable = a.connectable;
  if (a.name[0]) { strCopy(d.name, sizeof d.name, a.name); asciiOnly(d.name); }
  if (a.tag[0])  strCopy(d.tag, sizeof d.tag, a.tag);
  if (a.haveMfr) {
    d.haveMfr = true; d.company = a.company; d.mfrLen = a.mfrLen;
    if (a.ibeacon) { d.ibeacon = true; d.major = a.major; d.minor = a.minor; }
  }
  if (a.svcCount || a.svcLong) {
    memcpy(d.svc, a.svc, sizeof d.svc);
    d.svcCount = a.svcCount;
    d.svcLong  = a.svcLong;
  }
  if (a.haveTx) { d.haveTx = true; d.txPower = a.txPower; }
  d.lastRssi   = a.rssi;
  d.lastSeenMs = now;
  ++d.adverts;
  return true;
}

void BleTable::tidy(uint32_t now) {
  int w = 0;
  for (int r = 0; r < count; ++r)
    if (now - devs[r].lastSeenMs < BLE_STALE_MS) devs[w++] = devs[r];
  count = w;
  std::sort(devs, devs + count, [](const BleDev& x, const BleDev& y) {
    if (x.rssi != y.rssi) return x.rssi > y.rssi;
    return memcmp(x.addr, y.addr, 6) < 0;
  });
}

size_t formatBleLine(char* buf, size_t cap, const BleDev& d, uint32_t sweepNo, uint32_t now) {
  size_t n = (size_t)snprintf(buf, cap, "{\"scan\":%lu,\"ms\":%lu,\"name\":\"",
                              (unsigned long)sweepNo, (unsigned long)now);
  n = jsonAppend(buf, cap, n, d.name);
  n += (size_t)snprintf(buf + n, cap - n,
                        "\",\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"type\":\"%s\",\"rssi\":%d,\"tag\":\"%s\","
                        "\"age\":%lu,\"adv\":%lu,\"conn\":%s",
                        d.addr[0], d.addr[1], d.addr[2], d.addr[3], d.addr[4], d.addr[5],
                        d.addrType == ADDR_PUBLIC ? "pub" : "rnd", d.lastRssi, d.tag,
                        (unsigned long)(now - d.lastSeenMs), (unsigned long)d.adverts,
                        d.connectable ? "true" : "false");
  if (d.haveMfr && n < cap)
    n += (size_t)snprintf(buf + n, cap - n, ",\"mfr\":\"%04X\"", d.company);
  if (d.ibeacon && n < cap)
    n += (size_t)snprintf(buf + n, cap - n, ",\"major\":%u,\"minor\":%u", d.major, d.minor);
  if (d.haveTx && n < cap)
    n += (size_t)snprintf(buf + n, cap - n, ",\"tx\":%d", d.txPower);
  if (d.svcCount && n < cap) {
    n += (size_t)snprintf(buf + n, cap - n, ",\"svc\":[");
    for (int k = 0; k < d.svcCount && n < cap; ++k)
      n += (size_t)snprintf(buf + n, cap - n, "%s\"%04X\"", k ? "," : "", d.svc[k]);
    if (n < cap) n += (size_t)snprintf(buf + n, cap - n, "]");
  }
  if (n < cap) n += (size_t)snprintf(buf + n, cap - n, "}\n");
  if (n >= cap) n = cap - 1;
  return n;
}

}  // namespace survey
