// Small string helpers: safe copies, JSON escaping, and the names the log and
// screen use for Wi-Fi security modes. The numeric codes mirror ESP-IDF's
// wifi_auth_mode_t / wifi_cipher_type_t / wifi_second_chan_t; the firmware
// static_asserts that they still match.
#pragma once
#include <cstddef>
#include <cstdint>

namespace survey {

constexpr uint8_t AUTH_OPEN = 0, AUTH_WEP = 1, AUTH_WPA = 2, AUTH_WPA2 = 3, AUTH_WPA_WPA2 = 4,
                  AUTH_WPA2_ENT = 5, AUTH_WPA3 = 6, AUTH_WPA2_WPA3 = 7;
constexpr uint8_t CIPHER_NONE = 0, CIPHER_WEP40 = 1, CIPHER_WEP104 = 2, CIPHER_TKIP = 3, CIPHER_CCMP = 4,
                  CIPHER_TKIP_CCMP = 5, CIPHER_AES_CMAC128 = 6, CIPHER_SMS4 = 7, CIPHER_GCMP = 8,
                  CIPHER_GCMP256 = 9;
constexpr uint8_t SECOND_NONE = 0, SECOND_ABOVE = 1, SECOND_BELOW = 2;

constexpr uint8_t PHY_B  = 1;
constexpr uint8_t PHY_G  = 2;
constexpr uint8_t PHY_N  = 4;
constexpr uint8_t PHY_LR = 8;   // Espressif long-range mode

// strlcpy without depending on the platform having it. Always terminates.
void strCopy(char* dst, size_t cap, const char* src);

// The 6x8 font is ASCII only.
void asciiOnly(char* s);

// Append s to buf as JSON string content (escaped), never overflowing.
// Returns the new length; buf is always terminated.
size_t jsonAppend(char* buf, size_t cap, size_t pos, const char* s);

// "AA:BB:CC:DD:EE:FF"
void macText(const uint8_t* mac, char* out, size_t n);

// "11b/g/n" style summary of the modes an AP advertises.
void phyText(uint8_t phy, char* out, size_t n);

const char* authName(uint8_t a);
const char* cipherName(uint8_t c);

}  // namespace survey
