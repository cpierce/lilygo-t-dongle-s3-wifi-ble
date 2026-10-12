// RSSI bands: the colour legend in the README, as code.
#include <unity.h>
#include <survey/rssi.h>

using namespace survey;

void setUp() {}
void tearDown() {}

void test_wifi_bands_match_readme() {
  TEST_ASSERT_EQUAL_INT((int)Band::Great, (int)rssiBand(-60, WIFI_SCALE));   // -60 or better
  TEST_ASSERT_EQUAL_INT((int)Band::Great, (int)rssiBand(-30, WIFI_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Good,  (int)rssiBand(-61, WIFI_SCALE));   // -61 to -67
  TEST_ASSERT_EQUAL_INT((int)Band::Good,  (int)rssiBand(-67, WIFI_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Fair,  (int)rssiBand(-68, WIFI_SCALE));   // -68 to -75
  TEST_ASSERT_EQUAL_INT((int)Band::Fair,  (int)rssiBand(-75, WIFI_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Poor,  (int)rssiBand(-76, WIFI_SCALE));   // below -75
  TEST_ASSERT_EQUAL_INT((int)Band::Poor,  (int)rssiBand(-85, WIFI_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Edge,  (int)rssiBand(-86, WIFI_SCALE));
}

void test_ble_bands_sit_lower() {
  TEST_ASSERT_EQUAL_INT((int)Band::Great, (int)rssiBand(-65, BLE_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Good,  (int)rssiBand(-66, BLE_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Good,  (int)rssiBand(-75, BLE_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Fair,  (int)rssiBand(-76, BLE_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Fair,  (int)rssiBand(-85, BLE_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Poor,  (int)rssiBand(-86, BLE_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Poor,  (int)rssiBand(-92, BLE_SCALE));
  TEST_ASSERT_EQUAL_INT((int)Band::Edge,  (int)rssiBand(-93, BLE_SCALE));
}

void test_bars_and_words() {
  TEST_ASSERT_EQUAL_INT(4, rssiBars(-50, WIFI_SCALE));
  TEST_ASSERT_EQUAL_INT(3, rssiBars(-65, WIFI_SCALE));
  TEST_ASSERT_EQUAL_INT(2, rssiBars(-70, WIFI_SCALE));
  TEST_ASSERT_EQUAL_INT(1, rssiBars(-80, WIFI_SCALE));
  TEST_ASSERT_EQUAL_INT(0, rssiBars(-90, WIFI_SCALE));
  TEST_ASSERT_EQUAL_STRING("GREAT", rssiWord(-50, WIFI_SCALE));
  TEST_ASSERT_EQUAL_STRING("GOOD",  rssiWord(-65, WIFI_SCALE));
  TEST_ASSERT_EQUAL_STRING("FAIR",  rssiWord(-70, WIFI_SCALE));
  TEST_ASSERT_EQUAL_STRING("POOR",  rssiWord(-80, WIFI_SCALE));
  TEST_ASSERT_EQUAL_STRING("EDGE",  rssiWord(-90, WIFI_SCALE));
}

void test_smoothing_is_three_to_one() {
  int acc = rssiAccFrom(-60);
  TEST_ASSERT_EQUAL_INT(-60, rssiFromAcc(acc));
  acc = smoothRssiAcc(acc, -60);
  TEST_ASSERT_EQUAL_INT(-60, rssiFromAcc(acc));
  acc = smoothRssiAcc(rssiAccFrom(-60), -70);         // a quarter of the way: -62.5
  TEST_ASSERT_EQUAL_INT(-63, rssiFromAcc(acc));
  acc = smoothRssiAcc(rssiAccFrom(-70), -60);         // -67.5, half rounds away from zero
  TEST_ASSERT_EQUAL_INT(-68, rssiFromAcc(acc));
}

// The old (old * 3 + fresh) / 4 form got stuck 3 dB above a weaker steady
// signal because integer division truncates toward zero. The average must
// settle on the real level from either direction.
void test_smoothing_settles_on_the_true_level() {
  int acc = rssiAccFrom(-60);
  for (int i = 0; i < 40; ++i) acc = smoothRssiAcc(acc, -70);
  TEST_ASSERT_EQUAL_INT(-70, rssiFromAcc(acc));       // fading: not -67
  for (int i = 0; i < 40; ++i) acc = smoothRssiAcc(acc, -90);
  TEST_ASSERT_EQUAL_INT(-90, rssiFromAcc(acc));
  for (int i = 0; i < 40; ++i) acc = smoothRssiAcc(acc, -55);
  TEST_ASSERT_EQUAL_INT(-55, rssiFromAcc(acc));       // strengthening
  // Halfway there after a few readings, settled after a dozen or so.
  acc = rssiAccFrom(-60);
  acc = smoothRssiAcc(acc, -80); acc = smoothRssiAcc(acc, -80); acc = smoothRssiAcc(acc, -80);
  TEST_ASSERT_EQUAL_INT(-72, rssiFromAcc(acc));       // -60 -> -65 -> -68.75 -> -71.6
  for (int i = 0; i < 12; ++i) acc = smoothRssiAcc(acc, -80);
  TEST_ASSERT_EQUAL_INT(-80, rssiFromAcc(acc));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_wifi_bands_match_readme);
  RUN_TEST(test_ble_bands_sit_lower);
  RUN_TEST(test_bars_and_words);
  RUN_TEST(test_smoothing_is_three_to_one);
  RUN_TEST(test_smoothing_settles_on_the_true_level);
  return UNITY_END();
}
