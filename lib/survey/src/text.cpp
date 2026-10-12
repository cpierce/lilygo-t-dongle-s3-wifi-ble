#include "survey/text.h"
#include <cstdio>
#include <cstring>

namespace survey {

void strCopy(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  size_t i = 0;
  for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

void asciiOnly(char* s) {
  for (; *s; ++s)
    if ((uint8_t)*s < 0x20 || (uint8_t)*s >= 0x7F) *s = '?';
}

size_t jsonAppend(char* buf, size_t cap, size_t pos, const char* s) {
  if (cap == 0) return 0;
  if (pos >= cap) pos = cap - 1;
  for (; *s && pos + 2 < cap; ++s) {
    if (*s == '"' || *s == '\\') buf[pos++] = '\\';
    buf[pos++] = ((uint8_t)*s < 0x20) ? ' ' : *s;
  }
  buf[pos] = '\0';
  return pos;
}

void macText(const uint8_t* mac, char* out, size_t n) {
  snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void phyText(uint8_t phy, char* out, size_t n) {
  size_t pos = (size_t)snprintf(out, n, "11");
  const char* names[] = {"b", "g", "n", "lr"};
  bool first = true;
  for (int i = 0; i < 4 && pos < n; ++i) {
    if (!(phy & (1 << i))) continue;
    pos += (size_t)snprintf(out + pos, n - pos, "%s%s", first ? "" : "/", names[i]);
    first = false;
  }
  if (first) strCopy(out, n, "11?");
}

const char* authName(uint8_t a) {
  switch (a) {
    case AUTH_OPEN:      return "open";
    case AUTH_WEP:       return "WEP";
    case AUTH_WPA:       return "WPA";
    case AUTH_WPA2:      return "WPA2";
    case AUTH_WPA_WPA2:  return "WPA/WPA2";
    case AUTH_WPA2_ENT:  return "WPA2-ENT";
    case AUTH_WPA3:      return "WPA3";
    case AUTH_WPA2_WPA3: return "WPA2/WPA3";
    default:             return "?";
  }
}

const char* cipherName(uint8_t c) {
  switch (c) {
    case CIPHER_NONE:        return "none";
    case CIPHER_WEP40:       return "WEP40";
    case CIPHER_WEP104:      return "WEP104";
    case CIPHER_TKIP:        return "TKIP";
    case CIPHER_CCMP:        return "CCMP";
    case CIPHER_TKIP_CCMP:   return "TKIP+CCMP";
    case CIPHER_AES_CMAC128: return "CMAC";
    case CIPHER_GCMP:        return "GCMP";
    case CIPHER_GCMP256:     return "GCMP256";
    default:                 return "?";
  }
}

}  // namespace survey
