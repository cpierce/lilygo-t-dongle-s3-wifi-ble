// Button gesture decoder: press, double press, hold and hold auto-repeat.
#include <unity.h>
#include <survey/button.h>

using namespace survey;

namespace {

const ButtonTiming T = {};   // the firmware's defaults

// Feed the decoder a level for a span of time at 1 ms steps, collecting what it reports.
struct Sim {
  ButtonDecoder dec;
  uint32_t now = 1000;
  int presses = 0, doubles = 0, holds = 0;

  void run(bool pressed, uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
      switch (dec.update(pressed, now++)) {
        case Gesture::Press:       ++presses; break;
        case Gesture::DoublePress: ++doubles; break;
        case Gesture::Hold:        ++holds;   break;
        case Gesture::None:        break;
      }
    }
  }
  void tap(uint32_t downMs = 80) { run(true, downMs); run(false, 1); }
  void idle(uint32_t ms) { run(false, ms); }
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_single_press_reported_after_double_window() {
  Sim s;
  s.tap();
  s.idle(T.doubleMs - 5);
  TEST_ASSERT_EQUAL_INT(0, s.presses);     // still waiting for a possible second press
  s.idle(10);
  TEST_ASSERT_EQUAL_INT(1, s.presses);
  TEST_ASSERT_EQUAL_INT(0, s.doubles);
  TEST_ASSERT_EQUAL_INT(0, s.holds);
  s.idle(2000);
  TEST_ASSERT_EQUAL_INT(1, s.presses);     // reported once only
}

void test_double_press_reported_on_second_release() {
  Sim s;
  s.tap();
  s.idle(100);
  s.tap();
  TEST_ASSERT_EQUAL_INT(1, s.doubles);
  s.idle(2000);
  TEST_ASSERT_EQUAL_INT(1, s.doubles);
  TEST_ASSERT_EQUAL_INT(0, s.presses);     // not also a single press
  TEST_ASSERT_EQUAL_INT(0, s.holds);
}

void test_two_slow_presses_are_two_single_presses() {
  Sim s;
  s.tap();
  s.idle(T.doubleMs + 50);
  s.tap();
  s.idle(T.doubleMs + 50);
  TEST_ASSERT_EQUAL_INT(2, s.presses);
  TEST_ASSERT_EQUAL_INT(0, s.doubles);
}

void test_hold_fires_while_down_and_not_as_press() {
  Sim s;
  s.run(true, T.longPressMs - 1);
  TEST_ASSERT_EQUAL_INT(0, s.holds);
  s.run(true, 2);
  TEST_ASSERT_EQUAL_INT(1, s.holds);
  s.run(false, 1);
  s.idle(2000);
  TEST_ASSERT_EQUAL_INT(1, s.holds);
  TEST_ASSERT_EQUAL_INT(0, s.presses);     // releasing a hold is not a press
  TEST_ASSERT_EQUAL_INT(0, s.doubles);
}

void test_hold_repeats_while_held() {
  Sim s;
  s.run(true, T.longPressMs + 1);
  TEST_ASSERT_EQUAL_INT(1, s.holds);
  s.run(true, T.holdRepeatMs * 3);
  TEST_ASSERT_EQUAL_INT(4, s.holds);       // first + three repeats
  s.run(false, 1);
  s.idle(2000);
  TEST_ASSERT_EQUAL_INT(4, s.holds);
  TEST_ASSERT_EQUAL_INT(0, s.presses);
}

void test_press_then_hold_does_not_make_double() {
  Sim s;
  s.tap();
  s.idle(100);
  s.run(true, T.longPressMs + 1);
  s.run(false, 1);
  s.idle(2000);
  TEST_ASSERT_EQUAL_INT(1, s.holds);
  TEST_ASSERT_EQUAL_INT(0, s.doubles);
  TEST_ASSERT_EQUAL_INT(0, s.presses);     // the pending press was absorbed by the hold
}

void test_contact_bounce_is_ignored() {
  Sim s;
  s.run(true, T.debounceMs - 10);
  s.run(false, 1);
  s.idle(2000);
  TEST_ASSERT_EQUAL_INT(0, s.presses);
  TEST_ASSERT_EQUAL_INT(0, s.doubles);
}

void test_survives_millis_wraparound() {
  Sim s;
  s.now = 0xFFFFFFFFu - 40;                // press straddles the 32-bit rollover
  s.tap();
  s.idle(T.doubleMs + 10);
  TEST_ASSERT_EQUAL_INT(1, s.presses);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_press_reported_after_double_window);
  RUN_TEST(test_double_press_reported_on_second_release);
  RUN_TEST(test_two_slow_presses_are_two_single_presses);
  RUN_TEST(test_hold_fires_while_down_and_not_as_press);
  RUN_TEST(test_hold_repeats_while_held);
  RUN_TEST(test_press_then_hold_does_not_make_double);
  RUN_TEST(test_contact_bounce_is_ignored);
  RUN_TEST(test_survives_millis_wraparound);
  return UNITY_END();
}
