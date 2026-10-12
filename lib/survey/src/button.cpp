#include "survey/button.h"

namespace survey {

Gesture ButtonDecoder::update(bool pressed, uint32_t now) {
  if (pressed && !down_) {
    down_ = true; tDown_ = now; holdFired_ = false;
  } else if (pressed && down_ && !holdFired_ && now - tDown_ >= t_.longPressMs) {
    holdFired_ = true;
    presses_ = 0;
    tHold_ = now;
    return Gesture::Hold;
  } else if (pressed && down_ && holdFired_ && now - tHold_ >= t_.holdRepeatMs) {
    tHold_ = now;
    return Gesture::Hold;
  } else if (!pressed && down_) {
    down_ = false;
    if (!holdFired_ && now - tDown_ >= t_.debounceMs) {
      tUp_ = now;
      if (++presses_ >= 2) { presses_ = 0; return Gesture::DoublePress; }
    }
  }
  if (!down_ && presses_ == 1 && now - tUp_ >= t_.doubleMs) {
    presses_ = 0;
    return Gesture::Press;
  }
  return Gesture::None;
}

}  // namespace survey
