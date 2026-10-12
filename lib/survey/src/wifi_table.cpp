#include "survey/wifi_table.h"
#include "survey/rssi.h"
#include "survey/text.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace survey {

void WifiTable::clear() { count = 0; }

int WifiTable::find(const uint8_t* bssid) const {
  for (int i = 0; i < count; ++i)
    if (memcmp(aps[i].bssid, bssid, 6) == 0) return i;
  return -1;
}

const Ap* WifiTable::ingest(const ApRecord& r) {
  int idx = find(r.bssid);
  if (idx < 0) {
    if (count < MAX_APS) {
      idx = count++;
    } else {
      // Table full: recycle the entry that has gone longest without being heard.
      idx = 0;
      for (int k = 1; k < count; ++k)
        if (aps[k].lastSeen < aps[idx].lastSeen ||
            (aps[k].lastSeen == aps[idx].lastSeen && aps[k].rssi < aps[idx].rssi))
          idx = k;
      if (aps[idx].lastSeen == scanNo) return nullptr;  // everything is fresh, skip this one
    }
    aps[idx] = Ap{};
    memcpy(aps[idx].bssid, r.bssid, 6);
    aps[idx].rssiAcc = (int16_t)rssiAccFrom(r.rssi);
    aps[idx].firstSeen = scanNo;
  } else {
    aps[idx].rssiAcc = (int16_t)smoothRssiAcc(aps[idx].rssiAcc, r.rssi);
  }
  aps[idx].rssi = (int16_t)rssiFromAcc(aps[idx].rssiAcc);
  Ap& a = aps[idx];
  strCopy(a.ssid, sizeof a.ssid, r.ssid ? r.ssid : "");
  asciiOnly(a.ssid);
  a.lastRssi = (int8_t)r.rssi;
  a.channel  = r.channel;
  a.second   = r.second;
  a.auth     = r.auth;
  a.cipher   = r.cipher;
  a.phy      = r.phy;
  a.wps      = r.wps;
  a.lastSeen = scanNo;
  ++a.seenCount;
  return &a;
}

void WifiTable::endSweep() {
  int w = 0;
  for (int r = 0; r < count; ++r)
    if (scanNo - aps[r].lastSeen < STALE_SCANS) aps[w++] = aps[r];
  count = w;

  std::sort(aps, aps + count, [](const Ap& a, const Ap& b) {
    if (a.rssi != b.rssi) return a.rssi > b.rssi;
    return memcmp(a.bssid, b.bssid, 6) < 0;
  });
}

WifiTable::Siblings WifiTable::siblings(int idx) const {
  Siblings s = {0, -127, 0};
  if (idx < 0 || idx >= count) return s;
  for (int k = 0; k < count; ++k) {
    if (k == idx || strcmp(aps[k].ssid, aps[idx].ssid) != 0) continue;
    ++s.more;
    if (aps[k].rssi > s.bestRssi) { s.bestRssi = aps[k].rssi; s.bestCh = aps[k].channel; }
  }
  return s;
}

WifiTable::ChannelLoad WifiTable::channelLoad() const {
  ChannelLoad l;
  for (int c = 0; c < 14; ++c) { l.count[c] = 0; l.strongest[c] = -127; }
  l.maxCount = 1;
  for (int i = 0; i < count; ++i) {
    const int ch = aps[i].channel;
    if (ch < 1 || ch > 13) continue;
    l.count[ch]++;
    if (aps[i].rssi > l.strongest[ch]) l.strongest[ch] = aps[i].rssi;
    if (l.count[ch] > l.maxCount) l.maxCount = l.count[ch];
  }
  return l;
}

size_t formatApLine(char* buf, size_t cap, const Ap& a, uint32_t scanNo, uint32_t ms) {
  char phy[12];
  phyText(a.phy, phy, sizeof phy);
  size_t n = (size_t)snprintf(buf, cap, "{\"scan\":%lu,\"ms\":%lu,\"ssid\":\"",
                              (unsigned long)scanNo, (unsigned long)ms);
  n = jsonAppend(buf, cap, n, a.ssid);
  n += (size_t)snprintf(buf + n, cap - n,
                        "\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ch\":%u,\"rssi\":%d,\"auth\":\"%s\","
                        "\"bw\":%d,\"phy\":\"%s\",\"cipher\":\"%s\",\"wps\":%s}\n",
                        a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5],
                        a.channel, a.lastRssi, authName(a.auth),
                        a.second == SECOND_NONE ? 20 : 40, phy, cipherName(a.cipher),
                        a.wps ? "true" : "false");
  if (n >= cap) n = cap - 1;
  return n;
}

}  // namespace survey
