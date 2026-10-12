// BLE: advertisement decoding (maker, iBeacon, Eddystone, services), the
// device table, and log lines.
#include <unity.h>
#include <survey/ble_table.h>
#include <cstring>

using namespace survey;

namespace {

BleAdv adv(uint8_t last, int8_t rssi) {
  BleAdv a = {};
  a.addr[0] = 0x5C; a.addr[1] = 0x1B; a.addr[2] = 0xF4; a.addr[3] = 0xAA; a.addr[4] = 0xBB; a.addr[5] = last;
  a.addrType = 1;   // random
  a.rssi = rssi;
  return a;
}

// A real iBeacon payload: Apple (4C 00), subtype 02, length 15, UUID, major 0x0102, minor 0x0304, tx -59.
const uint8_t IBEACON[25] = {0x4C, 0x00, 0x02, 0x15,
                             0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2, 0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
                             0x01, 0x02, 0x03, 0x04, 0xC5};

}  // namespace

void setUp() {}
void tearDown() {}

void test_maker_tags() {
  TEST_ASSERT_EQUAL_STRING("Apple", makerTag(0x004C));
  TEST_ASSERT_EQUAL_STRING("MSFT",  makerTag(0x0006));
  TEST_ASSERT_EQUAL_STRING("Sonos", makerTag(0x05A7));
  TEST_ASSERT_EQUAL_STRING("Qualc", makerTag(0x000A));
  TEST_ASSERT_EQUAL_STRING("Qualc", makerTag(0x001D));
  TEST_ASSERT_EQUAL_STRING("",      makerTag(0xFFFF));
  for (uint16_t c = 0; c < 0x1000; ++c) TEST_ASSERT_TRUE(strlen(makerTag(c)) <= 5);   // fits the column
}

void test_ibeacon_is_decoded() {
  BleAdv a = {};
  parseManufacturerData(IBEACON, sizeof IBEACON, a);
  TEST_ASSERT_TRUE(a.haveMfr);
  TEST_ASSERT_EQUAL_HEX16(0x004C, a.company);
  TEST_ASSERT_EQUAL_INT(25, a.mfrLen);
  TEST_ASSERT_TRUE(a.ibeacon);
  TEST_ASSERT_EQUAL_HEX16(0x0102, a.major);
  TEST_ASSERT_EQUAL_HEX16(0x0304, a.minor);
  TEST_ASSERT_EQUAL_STRING("iBcn", a.tag);
}

void test_apple_but_not_ibeacon() {
  const uint8_t m[] = {0x4C, 0x00, 0x10, 0x05, 0x01, 0x18, 0x3A, 0x5E, 0x9F};   // Apple "nearby info"
  BleAdv a = {};
  parseManufacturerData(m, sizeof m, a);
  TEST_ASSERT_TRUE(a.haveMfr);
  TEST_ASSERT_FALSE(a.ibeacon);
  TEST_ASSERT_EQUAL_STRING("Apple", a.tag);
  TEST_ASSERT_EQUAL_INT(9, a.mfrLen);

  // Apple, subtype 02, but truncated: not an iBeacon.
  BleAdv b = {};
  parseManufacturerData(IBEACON, 20, b);
  TEST_ASSERT_FALSE(b.ibeacon);
  TEST_ASSERT_EQUAL_STRING("Apple", b.tag);
}

void test_unknown_and_short_manufacturer_data() {
  const uint8_t m[] = {0x34, 0x12, 0x00};
  BleAdv a = {};
  parseManufacturerData(m, sizeof m, a);
  TEST_ASSERT_TRUE(a.haveMfr);
  TEST_ASSERT_EQUAL_HEX16(0x1234, a.company);   // little-endian
  TEST_ASSERT_EQUAL_STRING("", a.tag);

  BleAdv b = {};
  parseManufacturerData(m, 1, b);
  TEST_ASSERT_FALSE(b.haveMfr);
  TEST_ASSERT_EQUAL_STRING("", b.tag);
}

void test_services_capped_and_eddystone_tag() {
  BleAdv a = {};
  for (uint16_t u = 0x1800; u < 0x1800 + MAX_SVC + 2; ++u) addService(a, u);
  addLongService(a); addLongService(a);
  TEST_ASSERT_EQUAL_INT(MAX_SVC, a.svcCount);
  TEST_ASSERT_EQUAL_HEX16(0x1800, a.svc[0]);
  TEST_ASSERT_EQUAL_INT(2, a.svcLong);
  finishAdv(a);
  TEST_ASSERT_EQUAL_STRING("", a.tag);

  BleAdv e = {};
  addService(e, SVC_EDDYSTONE);
  finishAdv(e);
  TEST_ASSERT_EQUAL_STRING("Eddy", e.tag);

  // A maker tag wins over the Eddystone fallback.
  BleAdv both = {};
  const uint8_t m[] = {0x59, 0x00, 0x01};
  parseManufacturerData(m, sizeof m, both);
  addService(both, SVC_EDDYSTONE);
  finishAdv(both);
  TEST_ASSERT_EQUAL_STRING("Nordc", both.tag);
}

void test_details_fill_in_over_time() {
  BleTable t;
  BleAdv a = adv(0x01, -60);
  strcpy(a.name, "AirPods");
  TEST_ASSERT_TRUE(t.ingest(a, 1000));
  BleAdv b = adv(0x01, -70);            // scan response: no name, but TX power and a service
  b.haveTx = true; b.txPower = 12; addService(b, 0xFE24);
  TEST_ASSERT_TRUE(t.ingest(b, 1500));
  BleAdv c = adv(0x01, -66);            // bare advert
  c.connectable = true;
  TEST_ASSERT_TRUE(t.ingest(c, 1800));

  TEST_ASSERT_EQUAL_INT(1, t.count);
  const BleDev& d = t.devs[0];
  TEST_ASSERT_EQUAL_STRING("AirPods", d.name);   // kept from the first advert
  TEST_ASSERT_TRUE(d.haveTx);
  TEST_ASSERT_EQUAL_INT(12, d.txPower);
  TEST_ASSERT_EQUAL_INT(1, d.svcCount);          // kept from the second
  TEST_ASSERT_EQUAL_HEX16(0xFE24, d.svc[0]);
  TEST_ASSERT_TRUE(d.connectable);               // always from the latest
  TEST_ASSERT_EQUAL_INT(-66, d.lastRssi);
  TEST_ASSERT_EQUAL_INT(-63, d.rssi);            // -60 -> -62.5 -> -63.4
  TEST_ASSERT_EQUAL_UINT32(3, d.adverts);
  TEST_ASSERT_EQUAL_UINT32(1000, d.firstSeenMs);
  TEST_ASSERT_EQUAL_UINT32(1800, d.lastSeenMs);
  TEST_ASSERT_TRUE(BleTable::fresh(d, 1800 + BLE_FRESH_MS - 1));
  TEST_ASSERT_FALSE(BleTable::fresh(d, 1800 + BLE_FRESH_MS));
}

void test_tidy_sorts_and_forgets() {
  BleTable t;
  t.ingest(adv(0x01, -80), 1000);
  t.ingest(adv(0x02, -50), 1000);
  t.ingest(adv(0x03, -50), 9000);
  t.tidy(9500);
  TEST_ASSERT_EQUAL_INT(3, t.count);
  TEST_ASSERT_EQUAL_INT(0x02, t.devs[0].addr[5]);   // equal RSSI: lower address first
  TEST_ASSERT_EQUAL_INT(0x03, t.devs[1].addr[5]);
  TEST_ASSERT_EQUAL_INT(0x01, t.devs[2].addr[5]);
  t.tidy(1000 + BLE_STALE_MS);                      // 0x01 and 0x02 just went stale
  TEST_ASSERT_EQUAL_INT(1, t.count);
  TEST_ASSERT_EQUAL_INT(0x03, t.devs[0].addr[5]);
}

void test_full_table_recycles_only_a_quiet_entry() {
  BleTable t;
  for (int i = 0; i < MAX_BLE; ++i) t.ingest(adv((uint8_t)i, -60), 1000);
  TEST_ASSERT_FALSE(t.ingest(adv(0xEE, -40), 1500));        // all heard within BLE_FRESH_MS
  TEST_ASSERT_EQUAL_INT(-1, t.find(adv(0xEE, 0).addr));
  for (int i = 1; i < MAX_BLE; ++i) t.ingest(adv((uint8_t)i, -60), 3500);   // 0x00 goes quiet
  TEST_ASSERT_TRUE(t.ingest(adv(0xEE, -40), 3600));
  TEST_ASSERT_EQUAL_INT(MAX_BLE, t.count);
  TEST_ASSERT_EQUAL_INT(-1, t.find(adv(0x00, 0).addr));
  TEST_ASSERT_TRUE(t.find(adv(0xEE, 0).addr) >= 0);
}

void test_log_line_with_every_optional_field() {
  BleTable t;
  BleAdv a = adv(0xCC, -58);
  strcpy(a.name, "Air\"Pods");
  parseManufacturerData(IBEACON, sizeof IBEACON, a);
  a.haveTx = true; a.txPower = 12; a.connectable = true;
  addService(a, 0xFE24); addService(a, 0x180F);
  t.ingest(a, 46000);
  char line[320];
  const size_t n = formatBleLine(line, sizeof line, t.devs[0], 3, 46120);
  TEST_ASSERT_EQUAL_STRING(
    "{\"scan\":3,\"ms\":46120,\"name\":\"Air\\\"Pods\",\"addr\":\"5C:1B:F4:AA:BB:CC\",\"type\":\"rnd\","
    "\"rssi\":-58,\"tag\":\"iBcn\",\"age\":120,\"adv\":1,\"conn\":true,\"mfr\":\"004C\",\"major\":258,\"minor\":772,"
    "\"tx\":12,\"svc\":[\"FE24\",\"180F\"]}\n",
    line);
  TEST_ASSERT_EQUAL_UINT(strlen(line), n);
}

void test_log_line_minimal_and_bounded() {
  BleTable t;
  BleAdv a = adv(0x01, -90);
  a.addrType = ADDR_PUBLIC;
  t.ingest(a, 100);
  char line[320];
  formatBleLine(line, sizeof line, t.devs[0], 1, 100);
  TEST_ASSERT_EQUAL_STRING(
    "{\"scan\":1,\"ms\":100,\"name\":\"\",\"addr\":\"5C:1B:F4:AA:BB:01\",\"type\":\"pub\","
    "\"rssi\":-90,\"tag\":\"\",\"age\":0,\"adv\":1,\"conn\":false}\n",
    line);

  char small[30];
  const size_t n = formatBleLine(small, sizeof small, t.devs[0], 1, 100);
  TEST_ASSERT_EQUAL_UINT(sizeof small - 1, n);
  TEST_ASSERT_EQUAL_UINT(sizeof small - 1, strlen(small));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_maker_tags);
  RUN_TEST(test_ibeacon_is_decoded);
  RUN_TEST(test_apple_but_not_ibeacon);
  RUN_TEST(test_unknown_and_short_manufacturer_data);
  RUN_TEST(test_services_capped_and_eddystone_tag);
  RUN_TEST(test_details_fill_in_over_time);
  RUN_TEST(test_tidy_sorts_and_forgets);
  RUN_TEST(test_full_table_recycles_only_a_quiet_entry);
  RUN_TEST(test_log_line_with_every_optional_field);
  RUN_TEST(test_log_line_minimal_and_bounded);
  return UNITY_END();
}
