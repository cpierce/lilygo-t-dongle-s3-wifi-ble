// T-Dongle-S3 Wi-Fi / Bluetooth survey
//
// Continuously scans 2.4 GHz Wi-Fi, or Bluetooth Low Energy, and shows every
// access point or BLE device it can hear, sorted by signal strength, on the
// dongle's 160x80 screen. Built for walking a building to decide whether more
// access points (or beacons) are needed.
//
// Button (the one on the dongle, GPIO 0):
//   press         - List -> Details of highlighted item -> Track it -> Channel load (Wi-Fi only) -> List
//   double press  - toggle radio: Wi-Fi <-> Bluetooth
//   hold          - move to the next item (in the list, or while looking at one);
//                   keep holding and it keeps stepping
//
// The RGB LED mirrors the colour of the highlighted item (List nav.view) or the
// item being looked at (Details / Track), so you can watch coverage without
// reading the tiny screen.
//
// Every scan is also written to the USB serial port as JSON lines, one per
// device, so a laptop can log a walk: pio device monitor  (115200 baud).
// Sending 'w', 'b' or 'r' over that port selects Wi-Fi, Bluetooth, or toggles,
// and 'n' / 'v' act like hold / press, so a laptop can drive it while logging.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <hal/usb_serial_jtag_ll.h>
#include <NimBLEDevice.h>
#include <TFT_eSPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <algorithm>
#include <cstring>

// Everything that does not need the radios or the screen lives in lib/survey
// so it can be unit-tested on the host (pio test -e native).
#include <survey/ble_table.h>
#include <survey/button.h>
#include <survey/navigator.h>
#include <survey/rssi.h>
#include <survey/text.h>
#include <survey/wifi_table.h>

// The library mirrors a few ESP-IDF / NimBLE enum values so it can stay free
// of their headers. Catch any drift here.
static_assert(survey::AUTH_OPEN == WIFI_AUTH_OPEN && survey::AUTH_WEP == WIFI_AUTH_WEP &&
              survey::AUTH_WPA == WIFI_AUTH_WPA_PSK && survey::AUTH_WPA2 == WIFI_AUTH_WPA2_PSK &&
              survey::AUTH_WPA_WPA2 == WIFI_AUTH_WPA_WPA2_PSK && survey::AUTH_WPA2_ENT == WIFI_AUTH_WPA2_ENTERPRISE &&
              survey::AUTH_WPA3 == WIFI_AUTH_WPA3_PSK && survey::AUTH_WPA2_WPA3 == WIFI_AUTH_WPA2_WPA3_PSK,
              "wifi_auth_mode_t changed");
static_assert(survey::CIPHER_NONE == WIFI_CIPHER_TYPE_NONE && survey::CIPHER_WEP40 == WIFI_CIPHER_TYPE_WEP40 &&
              survey::CIPHER_WEP104 == WIFI_CIPHER_TYPE_WEP104 && survey::CIPHER_TKIP == WIFI_CIPHER_TYPE_TKIP &&
              survey::CIPHER_CCMP == WIFI_CIPHER_TYPE_CCMP && survey::CIPHER_TKIP_CCMP == WIFI_CIPHER_TYPE_TKIP_CCMP &&
              survey::CIPHER_AES_CMAC128 == WIFI_CIPHER_TYPE_AES_CMAC128 && survey::CIPHER_GCMP == WIFI_CIPHER_TYPE_GCMP &&
              survey::CIPHER_GCMP256 == WIFI_CIPHER_TYPE_GCMP256,
              "wifi_cipher_type_t changed");
static_assert(survey::SECOND_NONE == WIFI_SECOND_CHAN_NONE && survey::SECOND_ABOVE == WIFI_SECOND_CHAN_ABOVE &&
              survey::SECOND_BELOW == WIFI_SECOND_CHAN_BELOW, "wifi_second_chan_t changed");
static_assert(survey::ADDR_PUBLIC == BLE_ADDR_PUBLIC, "BLE address type changed");

#ifndef SCREEN_ROTATION
#define SCREEN_ROTATION 1
#endif

namespace {

using namespace survey;

// ---------------------------------------------------------------- hardware --
constexpr int W = 160;
constexpr int H = 80;

constexpr int BTN_PIN    = 0;
constexpr int LED_DI_PIN = 40;   // APA102 data
constexpr int LED_CI_PIN = 39;   // APA102 clock

// ---------------------------------------------------------------- tuning ----
// Button timing, table sizes and staleness live in lib/survey.
constexpr uint32_t SCAN_MS_PER_CH = 150;   // ~2 s per full sweep of 13 channels
constexpr uint32_t BLE_TIDY_MS    = 500;   // re-sort and prune the BLE list
constexpr uint32_t BLE_LOG_MS     = 2000;  // serial dump of the BLE list
constexpr int      LIST_ROWS      = 7;
constexpr uint32_t REDRAW_MS      = 100;
constexpr uint8_t  LED_BRIGHTNESS = 3;     // 0..31, the LED is very bright

// ---------------------------------------------------------------- colours ---
constexpr uint16_t C_BG     = TFT_BLACK;
constexpr uint16_t C_HEADER = 0x000C;   // dark blue
constexpr uint16_t C_ROWSEL = 0x18E3;   // dark grey highlight
constexpr uint16_t C_TEXT   = TFT_WHITE;
constexpr uint16_t C_DIM    = 0x7BEF;   // mid grey
constexpr uint16_t C_FAINT  = 0x39E7;   // darker grey

uint16_t rssiColor(int r, const RssiScale& s) {
  switch (rssiBand(r, s)) {
    case Band::Great: return TFT_GREEN;
    case Band::Good:  return TFT_YELLOW;
    case Band::Fair:  return TFT_ORANGE;
    default:          return TFT_RED;
  }
}

// The USB Serial/JTAG peripheral only sends a packet shorter than 64 bytes when
// it is explicitly flushed. Serial.flush() drains the ring buffer into the FIFO
// but does not always issue that final flush, so the tail of the last line of a
// burst would wait for the next write. Push it out ourselves.
void pushSerial() {
  Serial.flush();
  delay(2);
  usb_serial_jtag_ll_txfifo_flush();
}

// ---------------------------------------------------------------- radio -----
enum class Radio { WiFi, Ble };

Radio radio = Radio::WiFi;

const RssiScale& scale() { return radio == Radio::WiFi ? WIFI_SCALE : BLE_SCALE; }

// ---------------------------------------------------------------- AP table --
WifiTable wifi;
bool     scanning = false;
uint32_t scanStartedMs = 0;
uint32_t nextScanMs = 0;
volatile uint32_t scanDoneStatus = 0;   // from the last SCAN_DONE event (0 = ok)
volatile uint32_t scanDoneEvents = 0;

constexpr uint32_t SCAN_WATCHDOG_MS = 10000;
constexpr uint32_t SCAN_GAP_MS      = 250;    // let the driver settle between sweeps

// Each record goes out as one write so the USB CDC driver moves it in whole
// 64-byte packets instead of one byte at a time.
void logAp(const Ap& a) {
  char line[320];
  const size_t n = formatApLine(line, sizeof line, a, wifi.scanNo, millis());
  Serial.write(reinterpret_cast<const uint8_t*>(line), n);
}

// Start a sweep with the IDF call directly. Results are still collected by the
// Arduino core's SCAN_DONE handler, so WiFi.scanComplete()/BSSID()/RSSI() work
// as usual; we just own the start/timeout side of the state machine.
void startScan() {
  wifi_scan_config_t cfg = {};
  cfg.show_hidden = true;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 100;
  cfg.scan_time.active.max = SCAN_MS_PER_CH;
  cfg.scan_time.passive = SCAN_MS_PER_CH;
  WiFi.scanDelete();                       // clears the SCAN_DONE flag
  const esp_err_t err = esp_wifi_scan_start(&cfg, false);
  if (err == ESP_OK) {
    scanning = true;
    scanStartedMs = millis();
  } else {
    Serial.printf("{\"event\":\"scan_start_failed\",\"err\":%d}\n", (int)err);
    nextScanMs = millis() + 1000;
  }
}

void ingestScan(int n) {
  wifi.beginSweep();
  Serial.printf("{\"event\":\"sweep\",\"scan\":%lu,\"ms\":%lu,\"found\":%d,\"took\":%lu,\"status\":%lu}\n",
                (unsigned long)wifi.scanNo, (unsigned long)millis(), n,
                (unsigned long)(millis() - scanStartedMs), (unsigned long)scanDoneStatus);
  for (int i = 0; i < n; ++i) {
    const wifi_ap_record_t* rec = static_cast<const wifi_ap_record_t*>(WiFi.getScanInfoByIndex(i));
    if (!rec) continue;
    ApRecord r = {};
    memcpy(r.bssid, rec->bssid, 6);
    r.ssid    = reinterpret_cast<const char*>(rec->ssid);
    r.rssi    = rec->rssi;
    r.channel = rec->primary;
    r.second  = rec->second;
    r.auth    = rec->authmode;
    r.cipher  = rec->pairwise_cipher;
    r.phy     = (rec->phy_11b ? PHY_B : 0) | (rec->phy_11g ? PHY_G : 0) |
                (rec->phy_11n ? PHY_N : 0) | (rec->phy_lr ? PHY_LR : 0);
    r.wps     = rec->wps;
    if (const Ap* a = wifi.ingest(r)) logAp(*a);
  }
  wifi.endSweep();
  pushSerial();
}

// ---------------------------------------------------------------- BLE table -
BleTable          ble;
uint32_t          bleSweepNo = 0;
QueueHandle_t     bleQueue = nullptr;
NimBLEScan*       bleScan = nullptr;
volatile uint32_t bleDropped = 0;   // adverts lost because loop() fell behind

// Runs in the NimBLE host task: pack the advertisement and queue it.
class AdvSink : public NimBLEScanCallbacks {
  // cppcheck-suppress unusedFunction ; virtual, called by the NimBLE host
  void onResult(const NimBLEAdvertisedDevice* d) override {
    BleAdv a = {};
    const uint8_t* le = d->getAddress().getBase()->val;   // little-endian
    for (int i = 0; i < 6; ++i) a.addr[i] = le[5 - i];
    a.addrType = d->getAddressType();
    a.rssi = d->getRSSI();
    a.connectable = d->isConnectable();
    if (d->haveName()) strlcpy(a.name, d->getName().c_str(), sizeof a.name);
    if (d->haveTXPower()) { a.haveTx = true; a.txPower = d->getTXPower(); }
    if (d->haveManufacturerData()) {
      const std::string m = d->getManufacturerData();
      parseManufacturerData(reinterpret_cast<const uint8_t*>(m.data()), m.size(), a);
    }
    const int nSvc = d->getServiceUUIDCount();
    for (int i = 0; i < nSvc; ++i) {
      const NimBLEUUID u = d->getServiceUUID(i);
      if (u.bitSize() == 16) {
        const uint8_t* v = u.getValue();
        addService(a, (uint16_t)(v[0] | ((uint16_t)v[1] << 8)));
      } else {
        addLongService(a);
      }
    }
    finishAdv(a);
    if (xQueueSend(bleQueue, &a, 0) != pdTRUE) ++bleDropped;
  }
};

void logBle() {
  ++bleSweepNo;
  const uint32_t now = millis();
  Serial.printf("{\"event\":\"ble_sweep\",\"scan\":%lu,\"ms\":%lu,\"found\":%d,\"dropped\":%lu}\n",
                (unsigned long)bleSweepNo, (unsigned long)now, ble.count, (unsigned long)bleDropped);
  for (int i = 0; i < ble.count; ++i) {
    char line[320];
    const size_t n = formatBleLine(line, sizeof line, ble.devs[i], bleSweepNo, now);
    Serial.write(reinterpret_cast<const uint8_t*>(line), n);
  }
  pushSerial();
}

// ---------------------------------------------------------------- UI state --
Navigator nav;                   // nav.view on screen and the highlighted item
char     trackSsid[33] = "";     // Wi-Fi item being looked at (Details / Track)
uint8_t  trackBssid[6] = {0};
bool     trackHidden = false;
uint8_t  trackAddr[6] = {0};     // BLE item being looked at
char     trackName[21] = "";
bool     dirty = true;

TFT_eSPI    tft;
TFT_eSprite frame(&tft);

// The list the navigator moves through, for whichever radio is active.
struct WifiItems : ItemSource {
  int            count() const override { return wifi.count; }
  const uint8_t* key(int i) const override { return wifi.aps[i].bssid; }
  int            find(const uint8_t* k) const override { return wifi.find(k); }
} wifiItems;
struct BleItems : ItemSource {
  int            count() const override { return ble.count; }
  const uint8_t* key(int i) const override { return ble.devs[i].addr; }
  int            find(const uint8_t* k) const override { return ble.find(k); }
} bleItems;

const ItemSource& items() {
  return radio == Radio::WiFi ? static_cast<const ItemSource&>(wifiItems)
                              : static_cast<const ItemSource&>(bleItems);
}

int itemCount() { return items().count(); }

// Make the highlighted item the one Details and Track show.
void targetFromCursor() {
  if (itemCount() == 0) return;
  if (radio == Radio::WiFi) {
    strlcpy(trackSsid, wifi.aps[nav.cursor].ssid, sizeof trackSsid);
    memcpy(trackBssid, wifi.aps[nav.cursor].bssid, 6);
    trackHidden = (trackSsid[0] == '\0');
  } else {
    memcpy(trackAddr, ble.devs[nav.cursor].addr, 6);
    strlcpy(trackName, ble.devs[nav.cursor].name, sizeof trackName);
  }
}

bool isTracked(const Ap& a) {
  if (trackHidden) return memcmp(a.bssid, trackBssid, 6) == 0;
  return strcmp(a.ssid, trackSsid) == 0;
}

void setRadio(Radio r) {
  if (r == radio) return;
  if (radio == Radio::WiFi) {
    if (scanning) { esp_wifi_scan_stop(); scanning = false; }
  } else {
    bleScan->stop();
  }
  radio = r;
  nav.reset();
  wifi.clear();
  ble.clear();
  if (radio == Radio::WiFi) {
    nextScanMs = millis() + SCAN_GAP_MS;
  } else {
    xQueueReset(bleQueue);
    bleScan->start(0, false, true);   // 0 = scan until told to stop
  }
  Serial.printf("{\"event\":\"radio\",\"ms\":%lu,\"radio\":\"%s\"}\n",
                (unsigned long)millis(), radio == Radio::WiFi ? "wifi" : "ble");
  pushSerial();
  dirty = true;
}

void toggleRadio() { setRadio(radio == Radio::WiFi ? Radio::Ble : Radio::WiFi); }

// Logged whenever the button changes what is on screen, so a laptop log shows
// which item was being looked at when.
void logView() {
  Serial.printf("{\"event\":\"view\",\"ms\":%lu,\"view\":\"%s\",\"cursor\":%d",
                (unsigned long)millis(), viewName(nav.view), nav.cursor);
  if (nav.haveSel) {
    char key[18];
    macText(nav.selKey, key, sizeof key);
    Serial.printf(",\"key\":\"%s\"", key);
  }
  Serial.println("}");
}

// Hold: next item.
void onNext() {
  if (nav.next(items())) targetFromCursor();
  logView();
  dirty = true;
}

// Press: next nav.view.
void onCycleView() {
  if (nav.cycle(items(), radio == Radio::WiFi)) targetFromCursor();
  logView();
  dirty = true;
}

ButtonDecoder button;

Gesture pollButton() { return button.update(digitalRead(BTN_PIN) == LOW, millis()); }

void pollSerial() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == 'w' || c == 'W')      setRadio(Radio::WiFi);
    else if (c == 'b' || c == 'B') setRadio(Radio::Ble);
    else if (c == 'r' || c == 'R') toggleRadio();
    else if (c == 'n' || c == 'N') onNext();        // same as hold
    else if (c == 'v' || c == 'V') onCycleView();   // same as press
  }
}

// ---------------------------------------------------------------- LED -------
void apaByte(uint8_t v) {
  for (int i = 7; i >= 0; --i) {
    digitalWrite(LED_DI_PIN, (v >> i) & 1);
    digitalWrite(LED_CI_PIN, HIGH);
    digitalWrite(LED_CI_PIN, LOW);
  }
}

void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t last = 0xFFFFFFFF;
  const uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (key == last) return;
  last = key;
  for (int i = 0; i < 4; ++i) apaByte(0x00);                  // start frame
  apaByte(0xE0 | LED_BRIGHTNESS); apaByte(b); apaByte(g); apaByte(r);
  for (int i = 0; i < 4; ++i) apaByte(0xFF);                  // end frame
}

void ledForRssi(int r, bool valid, const RssiScale& s) {
  if (!valid)          { ledSet(0, 0, 0);     return; }
  if (r >= s.great)    { ledSet(0, 255, 0);   return; }
  if (r >= s.good)     { ledSet(255, 200, 0); return; }
  if (r >= s.fair)     { ledSet(255, 80, 0);  return; }
  ledSet(255, 0, 0);
}

// ---------------------------------------------------------------- drawing ---
void drawBars(int x, int yBottom, int rssi, uint16_t color, const RssiScale& s) {
  const int n = rssiBars(rssi, s);
  for (int b = 0; b < 4; ++b) {
    const int h = 2 + b * 2;
    frame.fillRect(x + b * 3, yBottom - h, 2, h, b < n ? color : C_FAINT);
  }
}

void drawHeader(const char* left, const char* right) {
  frame.fillRect(0, 0, W, 10, C_HEADER);
  frame.setTextFont(1);
  frame.setTextColor(C_TEXT);
  frame.setTextDatum(TL_DATUM);
  frame.drawString(left, 2, 1);
  frame.setTextDatum(TR_DATUM);
  frame.drawString(right, W - 2, 1);
}

const char* spinner() {
  static const char* const glyphs[] = {"|", "/", "-", "\\"};
  return glyphs[(millis() / 250) % 4];
}

void apName(const Ap& a, char* out, size_t n) {
  if (a.ssid[0] == '\0') strlcpy(out, "<hidden>", n);
  else                   strlcpy(out, a.ssid, n);
}

// Unnamed devices show the last three bytes of their address.
void bleName(const BleDev& d, char* out, size_t n) {
  if (d.name[0] == '\0') snprintf(out, n, "%02X:%02X:%02X", d.addr[3], d.addr[4], d.addr[5]);
  else                   strlcpy(out, d.name, n);
}

struct RowInfo {
  char name[16];
  char mid[6];      // short text in the middle column
  int  rssi;
  bool fresh;       // heard in the latest sweep / the last couple of seconds
  bool dimName;     // name is a stand-in (hidden SSID, unnamed BLE device)
};

template <typename F>
void drawRows(const RssiScale& sc, int count, F rowOf) {
  const int first = (nav.cursor / LIST_ROWS) * LIST_ROWS;
  for (int row = 0; row < LIST_ROWS; ++row) {
    const int i = first + row;
    if (i >= count) break;
    RowInfo ri = {};
    rowOf(i, ri);
    const int y = 11 + row * 10;
    const uint16_t c = rssiColor(ri.rssi, sc);

    if (i == nav.cursor) frame.fillRect(0, y - 1, W, 10, C_ROWSEL);

    drawBars(2, y + 8, ri.rssi, ri.fresh ? c : C_DIM, sc);

    frame.setTextFont(1);
    frame.setTextDatum(TL_DATUM);
    frame.setTextColor(ri.fresh && !ri.dimName ? C_TEXT : C_DIM);
    frame.drawString(ri.name, 16, y);

    frame.setTextDatum(TR_DATUM);
    frame.setTextColor(C_DIM);
    frame.drawString(ri.mid, 112, y);

    char r[6];
    snprintf(r, sizeof r, "%d", ri.rssi);
    frame.setTextColor(ri.fresh ? c : C_DIM);
    frame.drawString(r, W - 2, y);
  }
}

void drawList() {
  const int n = itemCount();
  char hdr[32];
  if (radio == Radio::WiFi) snprintf(hdr, sizeof hdr, "2.4GHz  %d AP%s", n, n == 1 ? "" : "s");
  else                      snprintf(hdr, sizeof hdr, "BLE  %d dev%s", n, n == 1 ? "" : "s");
  char pg[16];
  const int pages = n ? (n + LIST_ROWS - 1) / LIST_ROWS : 1;
  snprintf(pg, sizeof pg, "%d/%d %s", n ? nav.cursor / LIST_ROWS + 1 : 1, pages, spinner());
  drawHeader(hdr, pg);

  if (n == 0) {
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(C_DIM);
    frame.drawString("scanning...", W / 2, H / 2 + 4, 2);
    ledForRssi(0, false, scale());
    return;
  }

  if (radio == Radio::WiFi) {
    drawRows(WIFI_SCALE, n, [](int i, RowInfo& r) {
      const Ap& a = wifi.aps[i];
      apName(a, r.name, 14);                       // 13 characters fit before the channel column
      snprintf(r.mid, sizeof r.mid, "%u", a.channel);
      r.rssi = a.rssi;
      r.fresh = (a.lastSeen == wifi.scanNo);
      r.dimName = (a.ssid[0] == '\0');
    });
    const Ap& sel = wifi.aps[nav.cursor];
    ledForRssi(sel.rssi, sel.lastSeen == wifi.scanNo, WIFI_SCALE);
  } else {
    const uint32_t now = millis();
    drawRows(BLE_SCALE, n, [now](int i, RowInfo& r) {
      const BleDev& d = ble.devs[i];
      bleName(d, r.name, 12);                      // 11 characters leave room for the maker tag
      strlcpy(r.mid, d.tag, sizeof r.mid);
      r.rssi = d.rssi;
      r.fresh = (now - d.lastSeenMs < BLE_FRESH_MS);
      r.dimName = (d.name[0] == '\0');
    });
    const BleDev& sel = ble.devs[nav.cursor];
    ledForRssi(sel.rssi, now - sel.lastSeenMs < BLE_FRESH_MS, BLE_SCALE);
  }
}

void drawNotInRange() {
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(TFT_RED);
  frame.drawString("not in range", W / 2, 34, 2);
  frame.setTextColor(C_DIM);
  frame.drawString("hold: next   press: leave", W / 2, 60, 1);
  ledForRssi(0, false, scale());
}

// One line of a details page: left text, and optional right-aligned text in
// its own colour. 26 characters fit across.
void detailLine(int y, const char* left, uint16_t leftColor, const char* right = "", uint16_t rightColor = C_TEXT) {
  frame.setTextFont(1);
  frame.setTextDatum(TL_DATUM);
  frame.setTextColor(leftColor);
  frame.drawString(left, 2, y);
  if (right[0]) {
    frame.setTextDatum(TR_DATUM);
    frame.setTextColor(rightColor);
    frame.drawString(right, W - 2, y);
  }
}

void drawDetails() {
  const int i = wifi.find(trackBssid);
  char hdr[24];
  if (trackHidden) strlcpy(hdr, "<hidden>", sizeof hdr);
  else             strlcpy(hdr, trackSsid, sizeof hdr);
  drawHeader(hdr, spinner());
  if (i < 0) { drawNotInRange(); return; }

  const Ap& a = wifi.aps[i];
  const bool fresh = (a.lastSeen == wifi.scanNo);
  const uint16_t c = rssiColor(a.rssi, WIFI_SCALE);
  const uint16_t cc = fresh ? c : C_DIM;
  char l[32], r[32];

  snprintf(l, sizeof l, "%02X:%02X:%02X:%02X:%02X:%02X",
           a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5]);
  snprintf(r, sizeof r, "%ddBm", (int)a.rssi);
  detailLine(12, l, C_TEXT, r, cc);

  if (a.second == WIFI_SECOND_CHAN_NONE) snprintf(l, sizeof l, "ch %u  20MHz", a.channel);
  else snprintf(l, sizeof l, "ch %u  40MHz%s", a.channel, a.second == WIFI_SECOND_CHAN_ABOVE ? "+" : "-");
  detailLine(22, l, C_TEXT, authName(a.auth), C_TEXT);

  char phy[12];
  phyText(a.phy, phy, sizeof phy);
  snprintf(l, sizeof l, "%s%s", phy, a.wps ? "  WPS" : "");
  detailLine(32, l, C_DIM, cipherName(a.cipher), C_DIM);

  snprintf(l, sizeof l, "now %d  avg %d", (int)a.lastRssi, (int)a.rssi);
  detailLine(42, l, C_DIM, rssiWord(a.rssi, WIFI_SCALE), cc);

  const uint32_t sweeps = wifi.scanNo - a.firstSeen + 1;
  snprintf(l, sizeof l, "heard %lu of %lu sweeps", (unsigned long)a.seenCount, (unsigned long)sweeps);
  detailLine(52, l, C_DIM);

  // Other APs broadcasting the same SSID, strongest one named.
  if (trackHidden) {
    strlcpy(l, "hidden SSID", sizeof l);
  } else {
    const WifiTable::Siblings sib = wifi.siblings(i);
    if (sib.more == 0) strlcpy(l, "only AP with this SSID", sizeof l);
    else snprintf(l, sizeof l, "+%d same SSID, best %d ch%d", sib.more, sib.bestRssi, sib.bestCh);
  }
  detailLine(62, l, C_DIM);

  detailLine(72, "hold: next   press: track", C_FAINT);
  ledForRssi(a.rssi, fresh, WIFI_SCALE);
}

void drawBleDetails() {
  const int i = ble.find(trackAddr);
  char hdr[24];
  if (trackName[0]) strlcpy(hdr, trackName, sizeof hdr);
  else snprintf(hdr, sizeof hdr, "%02X:%02X:%02X", trackAddr[3], trackAddr[4], trackAddr[5]);
  drawHeader(hdr, spinner());
  if (i < 0) { drawNotInRange(); return; }

  const BleDev& d = ble.devs[i];
  const uint32_t now = millis();
  const uint32_t age = now - d.lastSeenMs;
  const bool fresh = (age < BLE_FRESH_MS);
  const uint16_t c = rssiColor(d.rssi, BLE_SCALE);
  const uint16_t cc = fresh ? c : C_DIM;
  char l[32], r[32];

  snprintf(l, sizeof l, "%02X:%02X:%02X:%02X:%02X:%02X",
           d.addr[0], d.addr[1], d.addr[2], d.addr[3], d.addr[4], d.addr[5]);
  snprintf(r, sizeof r, "%ddBm", (int)d.rssi);
  detailLine(12, l, C_TEXT, r, cc);

  if (!d.haveMfr)     strlcpy(r, "no mfr data", sizeof r);
  else if (d.tag[0] && !d.ibeacon) snprintf(r, sizeof r, "%s %04X", d.tag, d.company);
  else                snprintf(r, sizeof r, "mfr %04X", d.company);
  detailLine(22, d.addrType == BLE_ADDR_PUBLIC ? "public addr" : "random addr", C_TEXT, r, C_TEXT);

  snprintf(l, sizeof l, "now %d  avg %d", (int)d.lastRssi, (int)d.rssi);
  detailLine(32, l, C_DIM, rssiWord(d.rssi, BLE_SCALE), cc);

  const uint32_t alive = std::max<uint32_t>(now - d.firstSeenMs, 1);
  snprintf(l, sizeof l, "%lu adv  %lu.%lu/s  %lu.%lus ago", (unsigned long)d.adverts,
           (unsigned long)(d.adverts * 1000 / alive), (unsigned long)(d.adverts * 10000 / alive % 10),
           (unsigned long)(age / 1000), (unsigned long)(age % 1000 / 100));
  detailLine(42, l, C_DIM);

  if (d.ibeacon) {
    snprintf(l, sizeof l, "iBeacon %u / %u", d.major, d.minor);
  } else if (d.svcCount) {
    size_t pos = (size_t)snprintf(l, sizeof l, "svc");
    for (int k = 0; k < d.svcCount && pos < sizeof l; ++k)
      pos += (size_t)snprintf(l + pos, sizeof l - pos, " %04X", d.svc[k]);
    if (d.svcLong && pos < sizeof l) snprintf(l + pos, sizeof l - pos, " +%u", d.svcLong);
  } else if (d.svcLong) {
    snprintf(l, sizeof l, "%u 128-bit service%s", d.svcLong, d.svcLong == 1 ? "" : "s");
  } else {
    strlcpy(l, "no services advertised", sizeof l);
  }
  detailLine(52, l, C_DIM);

  if (d.haveTx) snprintf(l, sizeof l, "tx %ddBm  %s", d.txPower, d.connectable ? "connectable" : "non-conn");
  else          snprintf(l, sizeof l, "tx n/a  %s", d.connectable ? "connectable" : "non-conn");
  if (d.haveMfr) snprintf(r, sizeof r, "mfr %uB", d.mfrLen); else r[0] = '\0';
  detailLine(62, l, C_DIM, r, C_DIM);

  detailLine(72, "hold: next   press: track", C_FAINT);
  ledForRssi(d.rssi, fresh, BLE_SCALE);
}

// Gauge from `lo` to `hi` dBm with a white tick at the "good" line.
void drawGauge(int rssi, uint16_t color, int lo, int hi, const RssiScale& s) {
  const int gx = 2, gy = 42, gw = W - 4, gh = 5;
  const int fill = map(constrain(rssi, lo, hi), lo, hi, 0, gw - 2);
  frame.drawRect(gx, gy, gw, gh, C_FAINT);
  frame.fillRect(gx + 1, gy + 1, fill, gh - 2, color);
  const int tick = gx + 1 + map(s.good, lo, hi, 0, gw - 2);
  frame.drawFastVLine(tick, gy - 1, gh + 2, C_TEXT);
}

void drawTrack() {
  // Gather the APs that belong to the tracked network, strongest first.
  const Ap* hits[MAX_APS];
  int nHits = 0;
  for (int i = 0; i < wifi.count; ++i)
    if (isTracked(wifi.aps[i])) hits[nHits++] = &wifi.aps[i];

  char hdr[24];
  if (trackHidden) strlcpy(hdr, "<hidden>", sizeof hdr);
  else             strlcpy(hdr, trackSsid, sizeof hdr);
  char cnt[12];
  snprintf(cnt, sizeof cnt, "%d AP %s", nHits, spinner());
  drawHeader(hdr, cnt);

  if (nHits == 0) { drawNotInRange(); return; }

  const Ap& best = *hits[0];
  const bool fresh = (best.lastSeen == wifi.scanNo);
  const uint16_t c = rssiColor(best.rssi, WIFI_SCALE);

  // Big number for the strongest AP.
  char r[6];
  snprintf(r, sizeof r, "%d", (int)best.rssi);
  frame.setTextDatum(TL_DATUM);
  frame.setTextColor(fresh ? c : C_DIM);
  frame.drawString(r, 2, 12, 4);

  frame.setTextColor(C_DIM);
  frame.drawString("dBm", 50, 14, 1);
  char ch[12];
  snprintf(ch, sizeof ch, "ch %u", best.channel);
  frame.drawString(ch, 50, 24, 1);
  char mac[12];
  snprintf(mac, sizeof mac, "%02X:%02X:%02X", best.bssid[3], best.bssid[4], best.bssid[5]);
  frame.drawString(mac, 50, 34, 1);

  frame.setTextDatum(TR_DATUM);
  frame.setTextColor(fresh ? c : C_DIM);
  frame.drawString(rssiWord(best.rssi, WIFI_SCALE), W - 2, 16, 2);

  drawGauge(best.rssi, fresh ? c : C_DIM, -95, -35, WIFI_SCALE);

  // Other APs of the same network.
  frame.setTextFont(1);
  for (int k = 1; k < nHits && k <= 3; ++k) {
    const Ap& a = *hits[k];
    const int y = 50 + (k - 1) * 10;
    const bool f = (a.lastSeen == wifi.scanNo);
    snprintf(mac, sizeof mac, "%02X:%02X:%02X", a.bssid[3], a.bssid[4], a.bssid[5]);
    frame.setTextDatum(TL_DATUM);
    frame.setTextColor(C_DIM);
    frame.drawString(mac, 2, y);
    snprintf(ch, sizeof ch, "ch %u", a.channel);
    frame.drawString(ch, 66, y);
    drawBars(108, y + 8, a.rssi, f ? rssiColor(a.rssi, WIFI_SCALE) : C_DIM, WIFI_SCALE);
    snprintf(r, sizeof r, "%d", (int)a.rssi);
    frame.setTextDatum(TR_DATUM);
    frame.setTextColor(f ? rssiColor(a.rssi, WIFI_SCALE) : C_DIM);
    frame.drawString(r, W - 2, y);
  }
  if (nHits == 1) {
    frame.setTextDatum(TL_DATUM);
    frame.setTextColor(C_FAINT);
    frame.drawString("only one AP heard", 2, 52);
  }

  ledForRssi(best.rssi, fresh, WIFI_SCALE);
}

void drawBleTrack() {
  const int i = ble.find(trackAddr);

  char hdr[24];
  if (trackName[0]) strlcpy(hdr, trackName, sizeof hdr);
  else snprintf(hdr, sizeof hdr, "%02X:%02X:%02X", trackAddr[3], trackAddr[4], trackAddr[5]);
  char right[12];
  snprintf(right, sizeof right, "%s %s", i >= 0 ? ble.devs[i].tag : "", spinner());
  drawHeader(hdr, right);

  if (i < 0) { drawNotInRange(); return; }

  const BleDev& d = ble.devs[i];
  const uint32_t now = millis();
  const uint32_t age = now - d.lastSeenMs;
  const bool fresh = (age < BLE_FRESH_MS);
  const uint16_t c = rssiColor(d.rssi, BLE_SCALE);

  char r[6];
  snprintf(r, sizeof r, "%d", (int)d.rssi);
  frame.setTextDatum(TL_DATUM);
  frame.setTextColor(fresh ? c : C_DIM);
  frame.drawString(r, 2, 12, 4);

  frame.setTextColor(C_DIM);
  frame.drawString("dBm", 50, 14, 1);
  char kind[12];
  snprintf(kind, sizeof kind, "%s %s", d.tag, d.addrType == BLE_ADDR_PUBLIC ? "pub" : "rnd");
  frame.drawString(kind, 50, 24, 1);
  char addr[18];
  snprintf(addr, sizeof addr, "%02X:%02X:%02X:%02X:%02X:%02X",
           d.addr[0], d.addr[1], d.addr[2], d.addr[3], d.addr[4], d.addr[5]);
  frame.drawString(addr, 50, 34, 1);

  frame.setTextDatum(TR_DATUM);
  frame.setTextColor(fresh ? c : C_DIM);
  frame.drawString(rssiWord(d.rssi, BLE_SCALE), W - 2, 16, 2);

  drawGauge(d.rssi, fresh ? c : C_DIM, -100, -40, BLE_SCALE);

  // How alive the device is: time since the last advertisement and its rate.
  const uint32_t alive = std::max<uint32_t>(now - d.firstSeenMs, 1);
  char line[32];
  snprintf(line, sizeof line, "heard %lu.%lu s ago", (unsigned long)(age / 1000), (unsigned long)(age % 1000 / 100));
  frame.setTextDatum(TL_DATUM);
  frame.setTextColor(C_DIM);
  frame.drawString(line, 2, 52, 1);
  snprintf(line, sizeof line, "%lu adv, %lu.%lu/s", (unsigned long)d.adverts,
           (unsigned long)(d.adverts * 1000 / alive), (unsigned long)(d.adverts * 10000 / alive % 10));
  frame.drawString(line, 2, 62, 1);

  ledForRssi(d.rssi, fresh, BLE_SCALE);
}

void drawChannels() {
  const WifiTable::ChannelLoad load = wifi.channelLoad();

  char hdr[32];
  snprintf(hdr, sizeof hdr, "Channel load  %d APs", wifi.count);
  drawHeader(hdr, spinner());

  const int baseY = 66, maxH = 44;
  frame.setTextFont(1);
  for (int ch = 1; ch <= 13; ++ch) {
    const int x = 2 + (ch - 1) * 12;
    const bool primary = (ch == 1 || ch == 6 || ch == 11);
    frame.drawFastHLine(x, baseY, 10, primary ? C_DIM : C_FAINT);
    if (load.count[ch]) {
      const int h = std::max(2, load.count[ch] * maxH / load.maxCount);
      // Bar colour: how loud the strongest AP on that channel is (louder = more contention).
      frame.fillRect(x, baseY - h, 10, h, rssiColor(load.strongest[ch], WIFI_SCALE));
      char n[4];
      snprintf(n, sizeof n, "%d", load.count[ch]);
      frame.setTextDatum(BC_DATUM);
      frame.setTextColor(C_TEXT);
      frame.drawString(n, x + 5, baseY - h - 1);
    }
    char lbl[4];
    snprintf(lbl, sizeof lbl, "%d", ch);
    frame.setTextDatum(TC_DATUM);
    frame.setTextColor(primary ? C_TEXT : C_DIM);
    frame.drawString(lbl, x + 5, 69);
  }
  ledForRssi(0, false, WIFI_SCALE);
}

void render() {
  frame.fillSprite(C_BG);
  switch (nav.view) {
    case View::List:     drawList(); break;
    case View::Details:  if (radio == Radio::WiFi) drawDetails(); else drawBleDetails(); break;
    case View::Track:    if (radio == Radio::WiFi) drawTrack();   else drawBleTrack();   break;
    case View::Channels: drawChannels(); break;
  }
  frame.pushSprite(0, 0);
}

// ---------------------------------------------------------------- service ---
void serviceWifi() {
  if (scanning) {
    const int n = WiFi.scanComplete();   // >= 0 only once the SCAN_DONE event has fired
    if (n >= 0) {
      ingestScan(n);
      WiFi.scanDelete();
      scanning = false;
      nextScanMs = millis() + SCAN_GAP_MS;
      nav.resync(items());
      dirty = true;
    } else if (millis() - scanStartedMs > SCAN_WATCHDOG_MS) {
      Serial.println("{\"event\":\"scan_timeout\"}");
      esp_wifi_scan_stop();
      scanning = false;
      nextScanMs = millis() + SCAN_GAP_MS;
    }
  }
  if (!scanning && (int32_t)(millis() - nextScanMs) >= 0) startScan();
}

void serviceBle() {
  static uint32_t lastTidy = 0, lastLog = 0, lastRestart = 0;

  BleAdv a;
  for (int budget = 64; budget > 0 && xQueueReceive(bleQueue, &a, 0) == pdTRUE; --budget)
    ble.ingest(a, millis());

  const uint32_t now = millis();
  if (now - lastTidy >= BLE_TIDY_MS) {
    lastTidy = now;
    ble.tidy(now);
    nav.resync(items());
    dirty = true;
  }
  if (now - lastLog >= BLE_LOG_MS) {
    lastLog = now;
    logBle();
  }
  // The host should scan until told to stop; if it ever gives up, kick it.
  if (!bleScan->isScanning() && now - lastRestart >= 1000) {
    lastRestart = now;
    Serial.println("{\"event\":\"ble_restart\"}");
    bleScan->start(0, false, true);
  }
}

}  // namespace

// ---------------------------------------------------------------- Arduino ---
void setup() {
  // A sweep logs ~3 KB in one burst; give the USB CDC driver room so nothing is
  // dropped if the host is slow to drain it.
  Serial.setTxBufferSize(8192);
  Serial.setTxTimeoutMs(500);
  Serial.begin(115200);

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_DI_PIN, OUTPUT);
  pinMode(LED_CI_PIN, OUTPUT);
  digitalWrite(LED_CI_PIN, LOW);
  ledSet(0, 0, 0);

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(C_BG);
  frame.setColorDepth(16);
  frame.createSprite(W, H);

  frame.fillSprite(C_BG);
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(C_TEXT);
  frame.drawString("Wi-Fi / BLE survey", W / 2, 22, 2);
  frame.setTextColor(C_DIM);
  frame.drawString("2x: Wi-Fi <> BLE", W / 2, 46, 1);
  frame.drawString("press: details  hold: next", W / 2, 58, 1);
  frame.pushSprite(0, 0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    scanDoneStatus = info.wifi_scan_done.status;
    ++scanDoneEvents;
  }, ARDUINO_EVENT_WIFI_SCAN_DONE);

  // Bring the BLE host up now so switching radios later is instant. It idles
  // (no scan running) while Wi-Fi is active.
  bleQueue = xQueueCreate(64, sizeof(BleAdv));
  NimBLEDevice::init("");
  bleScan = NimBLEDevice::getScan();
  bleScan->setScanCallbacks(new AdvSink(), /*wantDuplicates=*/true);
  bleScan->setActiveScan(true);        // ask for scan responses, which carry names
  bleScan->setDuplicateFilter(0);      // every advertisement, so RSSI keeps updating
  bleScan->setMaxResults(0);           // we keep our own table; do not buffer results
  bleScan->setInterval(100);
  bleScan->setWindow(100);             // listen continuously

  delay(100);
  startScan();
}

void loop() {
  switch (pollButton()) {
    case Gesture::Press:       onCycleView();  break;
    case Gesture::DoublePress: toggleRadio();  break;
    case Gesture::Hold:        onNext();       break;
    case Gesture::None:        break;
  }
  pollSerial();

  if (radio == Radio::WiFi) serviceWifi();
  else                      serviceBle();

  static uint32_t lastDraw = 0;
  const uint32_t now = millis();
  if (dirty || now - lastDraw >= REDRAW_MS) {
    render();
    lastDraw = now;
    dirty = false;
  }
  delay(5);
}
