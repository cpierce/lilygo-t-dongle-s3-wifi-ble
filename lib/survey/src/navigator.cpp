#include "survey/navigator.h"
#include <cstring>

namespace survey {

const char* viewName(View v) {
  switch (v) {
    case View::List:     return "list";
    case View::Details:  return "details";
    case View::Track:    return "track";
    case View::Channels: return "channels";
  }
  return "?";
}

void Navigator::reset() {
  view = View::List;
  cursor = 0;
  haveSel = false;
}

void Navigator::resync(const ItemSource& items) {
  const int n = items.count();
  if (n == 0) { cursor = 0; return; }
  if (haveSel) {
    const int i = items.find(selKey);
    if (i >= 0) cursor = i;
  }
  if (cursor >= n) cursor = n - 1;
  memcpy(selKey, items.key(cursor), 6);
  haveSel = true;
}

bool Navigator::next(const ItemSource& items) {
  const int n = items.count();
  if (n == 0 || view == View::Channels) return false;
  resync(items);
  cursor = (cursor + 1) % n;
  memcpy(selKey, items.key(cursor), 6);
  haveSel = true;
  return view != View::List;
}

bool Navigator::cycle(const ItemSource& items, bool hasChannels) {
  switch (view) {
    case View::List:
      if (items.count() == 0) {
        if (hasChannels) view = View::Channels;
        return false;
      }
      resync(items);
      view = View::Details;
      return true;
    case View::Details:  view = View::Track; return false;
    case View::Track:    view = hasChannels ? View::Channels : View::List; return false;
    case View::Channels: view = View::List; return false;
  }
  return false;
}

}  // namespace survey
