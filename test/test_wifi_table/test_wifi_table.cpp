// Wi-Fi AP table: smoothing, sorting, forgetting, recycling, derived views, log lines.
#include <unity.h>
#include <survey/wifi_table.h>
#include <survey/text.h>
#include <cstring>
#include <initializer_list>

using namespace survey;

namespace {

ApRecord rec(uint8_t last, int rssi, uint8_t ch = 6, const char* ssid = "Office") {
  ApRecord r = {};
  r.bssid[0] = 0xA4; r.bssid[1] = 0x2B; r.bssid[2] = 0xB0; r.bssid[3] = 0x11; r.bssid[4] = 0x22; r.bssid[5] = last;
  r.ssid = ssid; r.rssi = rssi; r.channel = ch; r.second = SECOND_NONE;
  r.auth = AUTH_WPA2; r.cipher = CIPHER_CCMP; r.phy = PHY_B | PHY_G | PHY_N; r.wps = false;
  return r;
}

void sweep(WifiTable& t, std::initializer_list<ApRecord> recs) {
  t.beginSweep();
  for (const ApRecord& r : recs) t.ingest(r);
  t.endSweep();
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_new_ap_is_recorded_verbatim() {
  WifiTable t;
  sweep(t, {rec(0x01, -55)});
  TEST_ASSERT_EQUAL_INT(1, t.count);
  const Ap& a = t.aps[0];
  TEST_ASSERT_EQUAL_STRING("Office", a.ssid);
  TEST_ASSERT_EQUAL_INT(-55, a.rssi);
  TEST_ASSERT_EQUAL_INT(-55, a.lastRssi);
  TEST_ASSERT_EQUAL_INT(6, a.channel);
  TEST_ASSERT_EQUAL_UINT32(1, a.firstSeen);
  TEST_ASSERT_EQUAL_UINT32(1, a.lastSeen);
  TEST_ASSERT_EQUAL_UINT32(1, a.seenCount);
  TEST_ASSERT_TRUE(t.fresh(a));
  TEST_ASSERT_EQUAL_INT(0, t.find(a.bssid));
}

void test_rssi_is_smoothed_and_raw_kept() {
  WifiTable t;
  sweep(t, {rec(0x01, -60)});
  sweep(t, {rec(0x01, -80)});
  TEST_ASSERT_EQUAL_INT(1, t.count);
  TEST_ASSERT_EQUAL_INT(-65, t.aps[0].rssi);       // (-180 - 80) / 4
  TEST_ASSERT_EQUAL_INT(-80, t.aps[0].lastRssi);
  TEST_ASSERT_EQUAL_UINT32(2, t.aps[0].seenCount);
  TEST_ASSERT_EQUAL_UINT32(1, t.aps[0].firstSeen);
}

void test_sorted_strongest_first_with_stable_tiebreak() {
  WifiTable t;
  sweep(t, {rec(0x03, -70), rec(0x01, -50), rec(0x02, -70)});
  TEST_ASSERT_EQUAL_INT(0x01, t.aps[0].bssid[5]);
  TEST_ASSERT_EQUAL_INT(0x02, t.aps[1].bssid[5]);  // equal RSSI: lower BSSID first
  TEST_ASSERT_EQUAL_INT(0x03, t.aps[2].bssid[5]);
}

void test_ap_forgotten_after_three_missed_sweeps() {
  WifiTable t;
  sweep(t, {rec(0x01, -60)});
  sweep(t, {});                                     // miss 1
  TEST_ASSERT_EQUAL_INT(1, t.count);
  TEST_ASSERT_FALSE(t.fresh(t.aps[0]));             // drawn grey
  sweep(t, {});                                     // miss 2
  TEST_ASSERT_EQUAL_INT(1, t.count);
  sweep(t, {});                                     // miss 3
  TEST_ASSERT_EQUAL_INT(0, t.count);
}

void test_hearing_it_again_resets_the_miss_count() {
  WifiTable t;
  sweep(t, {rec(0x01, -60)});
  sweep(t, {});
  sweep(t, {});
  sweep(t, {rec(0x01, -60)});
  sweep(t, {});
  sweep(t, {});
  TEST_ASSERT_EQUAL_INT(1, t.count);
}

void test_full_table_recycles_the_stalest_entry() {
  WifiTable t;
  t.beginSweep();
  for (int i = 0; i < MAX_APS; ++i) t.ingest(rec((uint8_t)i, -50 - (i % 30)));
  t.endSweep();
  TEST_ASSERT_EQUAL_INT(MAX_APS, t.count);

  // Everything was heard this sweep: one more has nowhere to go.
  t.beginSweep();
  for (int i = 0; i < MAX_APS; ++i) t.ingest(rec((uint8_t)i, -50 - (i % 30)));
  TEST_ASSERT_NULL(t.ingest(rec(0xEE, -40)));
  t.endSweep();
  TEST_ASSERT_EQUAL_INT(-1, t.find(rec(0xEE, 0).bssid));

  // Next sweep, 0x05 goes quiet: the newcomer takes its slot.
  t.beginSweep();
  for (int i = 0; i < MAX_APS; ++i) if (i != 5) t.ingest(rec((uint8_t)i, -50 - (i % 30)));
  TEST_ASSERT_NOT_NULL(t.ingest(rec(0xEE, -40)));
  t.endSweep();
  TEST_ASSERT_EQUAL_INT(MAX_APS, t.count);
  TEST_ASSERT_EQUAL_INT(-1, t.find(rec(0x05, 0).bssid));
  TEST_ASSERT_EQUAL_INT(0, t.find(rec(0xEE, 0).bssid));   // and it is the strongest
}

void test_ssid_is_ascii_and_bounded() {
  WifiTable t;
  sweep(t, {rec(0x01, -60, 1, "Caf\xC3\xA9 123456789012345678901234567890")});
  TEST_ASSERT_EQUAL_STRING("Caf?? 12345678901234567890123456", t.aps[0].ssid);   // 32 chars max
  TEST_ASSERT_EQUAL_UINT(32, strlen(t.aps[0].ssid));
}

void test_siblings_and_channel_load() {
  WifiTable t;
  sweep(t, {rec(0x01, -50, 1, "Office"), rec(0x02, -70, 6, "Office"), rec(0x03, -60, 11, "Office"),
            rec(0x04, -65, 6, "Guest"), rec(0x05, -90, 6, "")});
  const int office1 = t.find(rec(0x01, 0).bssid);
  const WifiTable::Siblings s = t.siblings(office1);
  TEST_ASSERT_EQUAL_INT(2, s.more);
  TEST_ASSERT_EQUAL_INT(-60, s.bestRssi);
  TEST_ASSERT_EQUAL_INT(11, s.bestCh);
  TEST_ASSERT_EQUAL_INT(0, t.siblings(t.find(rec(0x04, 0).bssid)).more);
  TEST_ASSERT_EQUAL_INT(0, t.siblings(99).more);     // out of range is harmless

  const WifiTable::ChannelLoad l = t.channelLoad();
  TEST_ASSERT_EQUAL_INT(1, l.count[1]);
  TEST_ASSERT_EQUAL_INT(3, l.count[6]);
  TEST_ASSERT_EQUAL_INT(1, l.count[11]);
  TEST_ASSERT_EQUAL_INT(0, l.count[2]);
  TEST_ASSERT_EQUAL_INT(3, l.maxCount);
  TEST_ASSERT_EQUAL_INT(-65, l.strongest[6]);
  TEST_ASSERT_EQUAL_INT(-127, l.strongest[2]);
}

void test_channel_load_ignores_out_of_band_channels() {
  WifiTable t;
  sweep(t, {rec(0x01, -50, 14), rec(0x02, -50, 0), rec(0x03, -50, 36)});
  const WifiTable::ChannelLoad l = t.channelLoad();
  for (int c = 0; c < 14; ++c) TEST_ASSERT_EQUAL_INT(0, l.count[c]);
  TEST_ASSERT_EQUAL_INT(1, l.maxCount);               // never 0, it is a divisor
}

void test_log_line_format() {
  WifiTable t;
  ApRecord r = rec(0x33, -61, 6, "Off\"ice");
  r.second = SECOND_ABOVE; r.wps = true;
  sweep(t, {r});
  char line[320];
  const size_t n = formatApLine(line, sizeof line, t.aps[0], t.scanNo, 34567);
  TEST_ASSERT_EQUAL_STRING(
    "{\"scan\":1,\"ms\":34567,\"ssid\":\"Off\\\"ice\",\"bssid\":\"A4:2B:B0:11:22:33\",\"ch\":6,"
    "\"rssi\":-61,\"auth\":\"WPA2\",\"bw\":40,\"phy\":\"11b/g/n\",\"cipher\":\"CCMP\",\"wps\":true}\n",
    line);
  TEST_ASSERT_EQUAL_UINT(strlen(line), n);
}

void test_log_line_never_overflows_a_small_buffer() {
  WifiTable t;
  sweep(t, {rec(0x01, -61, 6, "A very long network name here!!!")});
  char line[40];
  const size_t n = formatApLine(line, sizeof line, t.aps[0], t.scanNo, 1);
  TEST_ASSERT_EQUAL_UINT(sizeof line - 1, n);
  TEST_ASSERT_EQUAL_UINT(sizeof line - 1, strlen(line));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_new_ap_is_recorded_verbatim);
  RUN_TEST(test_rssi_is_smoothed_and_raw_kept);
  RUN_TEST(test_sorted_strongest_first_with_stable_tiebreak);
  RUN_TEST(test_ap_forgotten_after_three_missed_sweeps);
  RUN_TEST(test_hearing_it_again_resets_the_miss_count);
  RUN_TEST(test_full_table_recycles_the_stalest_entry);
  RUN_TEST(test_ssid_is_ascii_and_bounded);
  RUN_TEST(test_siblings_and_channel_load);
  RUN_TEST(test_channel_load_ignores_out_of_band_channels);
  RUN_TEST(test_log_line_format);
  RUN_TEST(test_log_line_never_overflows_a_small_buffer);
  return UNITY_END();
}
