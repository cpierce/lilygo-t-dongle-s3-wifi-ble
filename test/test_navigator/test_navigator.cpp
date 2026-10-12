// View cycling and the highlight that follows an item through re-sorts.
#include <unity.h>
#include <survey/navigator.h>
#include <cstring>
#include <vector>

using namespace survey;

namespace {

// A list of items identified by one byte, standing in for a table.
struct FakeItems : ItemSource {
  std::vector<uint8_t> ids;
  mutable uint8_t keyBuf[6];

  explicit FakeItems(std::initializer_list<uint8_t> l) : ids(l) {}
  int count() const override { return (int)ids.size(); }
  const uint8_t* key(int i) const override {
    memset(keyBuf, 0, 6); keyBuf[5] = ids[(size_t)i]; return keyBuf;
  }
  int find(const uint8_t* k) const override {
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == k[5]) return (int)i;
    return -1;
  }
};

int v(View x) { return (int)x; }

}  // namespace

void setUp() {}
void tearDown() {}

void test_wifi_press_cycle() {
  Navigator n;
  FakeItems items{1, 2, 3};
  TEST_ASSERT_TRUE(n.cycle(items, true));              // List -> Details: target it
  TEST_ASSERT_EQUAL_INT(v(View::Details), v(n.view));
  TEST_ASSERT_EQUAL_INT(0, n.cursor);
  TEST_ASSERT_TRUE(n.haveSel);
  TEST_ASSERT_EQUAL_INT(1, n.selKey[5]);
  TEST_ASSERT_FALSE(n.cycle(items, true));
  TEST_ASSERT_EQUAL_INT(v(View::Track), v(n.view));
  TEST_ASSERT_FALSE(n.cycle(items, true));
  TEST_ASSERT_EQUAL_INT(v(View::Channels), v(n.view));
  TEST_ASSERT_FALSE(n.cycle(items, true));
  TEST_ASSERT_EQUAL_INT(v(View::List), v(n.view));
}

void test_ble_press_cycle_has_no_channels() {
  Navigator n;
  FakeItems items{1};
  n.cycle(items, false);
  n.cycle(items, false);
  TEST_ASSERT_EQUAL_INT(v(View::Track), v(n.view));
  n.cycle(items, false);
  TEST_ASSERT_EQUAL_INT(v(View::List), v(n.view));
}

void test_empty_list() {
  Navigator n;
  FakeItems none{};
  TEST_ASSERT_FALSE(n.cycle(none, false));              // BLE: nothing to show, stay
  TEST_ASSERT_EQUAL_INT(v(View::List), v(n.view));
  TEST_ASSERT_FALSE(n.cycle(none, true));               // Wi-Fi: channel view is still useful
  TEST_ASSERT_EQUAL_INT(v(View::Channels), v(n.view));
  TEST_ASSERT_FALSE(n.next(none));
  TEST_ASSERT_EQUAL_INT(0, n.cursor);
  TEST_ASSERT_FALSE(n.haveSel);
}

void test_hold_moves_and_wraps() {
  Navigator n;
  FakeItems items{1, 2, 3};
  TEST_ASSERT_FALSE(n.next(items));                     // in the list: no re-target
  TEST_ASSERT_EQUAL_INT(1, n.cursor);
  TEST_ASSERT_EQUAL_INT(2, n.selKey[5]);
  n.next(items);
  TEST_ASSERT_EQUAL_INT(2, n.cursor);
  n.next(items);
  TEST_ASSERT_EQUAL_INT(0, n.cursor);                   // wrapped
  TEST_ASSERT_EQUAL_INT(1, n.selKey[5]);
}

void test_hold_in_details_retargets() {
  Navigator n;
  FakeItems items{1, 2};
  n.cycle(items, true);
  TEST_ASSERT_TRUE(n.next(items));
  TEST_ASSERT_EQUAL_INT(v(View::Details), v(n.view));   // stays on the page
  TEST_ASSERT_EQUAL_INT(2, n.selKey[5]);
  n.cycle(items, true);                                 // Track
  TEST_ASSERT_TRUE(n.next(items));
  TEST_ASSERT_EQUAL_INT(1, n.selKey[5]);
  n.cycle(items, true);                                 // Channels
  TEST_ASSERT_FALSE(n.next(items));                     // nothing to step through
  TEST_ASSERT_EQUAL_INT(1, n.selKey[5]);
}

void test_highlight_follows_item_through_resort() {
  Navigator n;
  FakeItems items{1, 2, 3};
  n.next(items);                                        // on item 2, row 1
  items.ids = {3, 1, 2};                                // list re-sorted
  n.resync(items);
  TEST_ASSERT_EQUAL_INT(2, n.cursor);
  TEST_ASSERT_EQUAL_INT(2, n.selKey[5]);
  TEST_ASSERT_FALSE(n.next(items));                     // next from there wraps to row 0
  TEST_ASSERT_EQUAL_INT(0, n.cursor);
  TEST_ASSERT_EQUAL_INT(3, n.selKey[5]);
}

void test_highlight_stays_put_when_item_vanishes() {
  Navigator n;
  FakeItems items{1, 2, 3};
  n.next(items); n.next(items);                         // item 3, row 2
  items.ids = {1, 2};                                   // it dropped off the end
  n.resync(items);
  TEST_ASSERT_EQUAL_INT(1, n.cursor);                   // clamped to the last row
  TEST_ASSERT_EQUAL_INT(2, n.selKey[5]);                // and now following that one
  items.ids = {9, 1, 2};
  n.resync(items);
  TEST_ASSERT_EQUAL_INT(2, n.cursor);
}

void test_reset_clears_selection() {
  Navigator n;
  FakeItems items{1, 2};
  n.next(items);
  n.cycle(items, true);
  n.reset();
  TEST_ASSERT_EQUAL_INT(v(View::List), v(n.view));
  TEST_ASSERT_EQUAL_INT(0, n.cursor);
  TEST_ASSERT_FALSE(n.haveSel);
}

void test_view_names_match_the_log_format() {
  TEST_ASSERT_EQUAL_STRING("list",     viewName(View::List));
  TEST_ASSERT_EQUAL_STRING("details",  viewName(View::Details));
  TEST_ASSERT_EQUAL_STRING("track",    viewName(View::Track));
  TEST_ASSERT_EQUAL_STRING("channels", viewName(View::Channels));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_wifi_press_cycle);
  RUN_TEST(test_ble_press_cycle_has_no_channels);
  RUN_TEST(test_empty_list);
  RUN_TEST(test_hold_moves_and_wraps);
  RUN_TEST(test_hold_in_details_retargets);
  RUN_TEST(test_highlight_follows_item_through_resort);
  RUN_TEST(test_highlight_stays_put_when_item_vanishes);
  RUN_TEST(test_reset_clears_selection);
  RUN_TEST(test_view_names_match_the_log_format);
  return UNITY_END();
}
