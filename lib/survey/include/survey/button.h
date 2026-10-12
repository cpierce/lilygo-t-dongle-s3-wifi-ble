// Turns the raw level of the one button into press / double press / hold.
#pragma once
#include <cstdint>

namespace survey {

enum class Gesture { None, Press, DoublePress, Hold };

struct ButtonTiming {
  uint32_t debounceMs   = 30;    // shorter than this is contact bounce, ignored
  uint32_t doubleMs     = 350;   // second press within this = double press
  uint32_t longPressMs  = 600;   // held this long = hold
  uint32_t holdRepeatMs = 350;   // while still held, repeat the hold this often
};

class ButtonDecoder {
 public:
  explicit ButtonDecoder(ButtonTiming t = ButtonTiming{}) : t_(t) {}

  // Call often with the current level and time. Press is reported doubleMs
  // after release once it is clear a second press is not coming. Hold fires
  // while the button is still down, after longPressMs, and again every
  // holdRepeatMs for as long as it stays down.
  Gesture update(bool pressed, uint32_t now);

  const ButtonTiming& timing() const { return t_; }

 private:
  ButtonTiming t_;
  bool     down_ = false;
  bool     holdFired_ = false;
  int      presses_ = 0;
  uint32_t tDown_ = 0;
  uint32_t tUp_ = 0;
  uint32_t tHold_ = 0;
};

}  // namespace survey
