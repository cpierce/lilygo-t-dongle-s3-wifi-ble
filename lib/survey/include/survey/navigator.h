// Which view is on screen and which item the highlight is on. The highlight
// follows an item by its key (BSSID or address) rather than by row, so it
// stays put when the list re-sorts under it.
#pragma once
#include <cstdint>

namespace survey {

enum class View { List, Details, Track, Channels };

const char* viewName(View v);

// The list the navigator moves through: the Wi-Fi or the BLE table.
struct ItemSource {
  virtual ~ItemSource() = default;
  virtual int            count() const = 0;
  virtual const uint8_t* key(int i) const = 0;   // 6-byte BSSID or address
  virtual int            find(const uint8_t* key) const = 0;
};

class Navigator {
 public:
  View    view = View::List;
  int     cursor = 0;
  uint8_t selKey[6] = {0};   // key of the item under the highlight
  bool    haveSel = false;

  // Back to the top of an empty list (radio switch).
  void reset();

  // Keep the highlight on the same item after the list changed.
  void resync(const ItemSource& items);

  // Hold: move the highlight to the next item (wrapping). Returns true when
  // the caller should re-target Details / Track on the new item.
  bool next(const ItemSource& items);

  // Press: List -> Details -> Track -> Channels -> List. `hasChannels` is
  // false for BLE, where Track goes straight back to the List. Returns true
  // when the caller should target Details on the highlighted item.
  bool cycle(const ItemSource& items, bool hasChannels);
};

}  // namespace survey
