// String helpers: safe copies, JSON escaping, MAC and PHY text, security names.
#include <unity.h>
#include <survey/text.h>
#include <cstring>

using namespace survey;

void setUp() {}
void tearDown() {}

void test_strcopy_truncates_and_terminates() {
  char b[5];
  memset(b, 'x', sizeof b);
  strCopy(b, sizeof b, "abcdefgh");
  TEST_ASSERT_EQUAL_STRING("abcd", b);
  strCopy(b, sizeof b, "");
  TEST_ASSERT_EQUAL_STRING("", b);
  strCopy(b, 1, "abc");
  TEST_ASSERT_EQUAL_STRING("", b);
  strCopy(b, 0, "abc");                 // must not touch the buffer
  TEST_ASSERT_EQUAL_STRING("", b);
}

void test_ascii_only_replaces_control_and_high_bytes() {
  char s[] = "Caf\xC3\xA9 \x01ok\x7F";
  asciiOnly(s);
  TEST_ASSERT_EQUAL_STRING("Caf?? ?ok?", s);
}

void test_json_append_escapes_and_respects_capacity() {
  char b[64];
  size_t n = jsonAppend(b, sizeof b, 0, "say \"hi\" \\ there");
  TEST_ASSERT_EQUAL_STRING("say \\\"hi\\\" \\\\ there", b);
  TEST_ASSERT_EQUAL_UINT(strlen(b), n);

  n = jsonAppend(b, sizeof b, 0, "tab\there\nnew");
  TEST_ASSERT_EQUAL_STRING("tab here new", b);   // control chars become spaces, never raw

  // Appending continues from pos and never overruns.
  char small[8];
  n = jsonAppend(small, sizeof small, 0, "abc");
  n = jsonAppend(small, sizeof small, n, "defghijkl");
  TEST_ASSERT_TRUE(n < sizeof small);
  TEST_ASSERT_EQUAL_UINT(strlen(small), n);
  TEST_ASSERT_EQUAL_STRING("abcdef", small);     // always leaves room for a 2-byte escape + terminator

  // An escape is written whole or not at all.
  char tiny[4];
  n = jsonAppend(tiny, sizeof tiny, 0, "a\"b");
  TEST_ASSERT_EQUAL_STRING("a\\\"", tiny);           // a\" fits exactly, b does not
  char tinier[3];
  n = jsonAppend(tinier, sizeof tinier, 0, "a\"b");
  TEST_ASSERT_EQUAL_STRING("a", tinier);         // no room for both escape bytes: dropped
}

void test_mac_text() {
  const uint8_t mac[6] = {0xA4, 0x2B, 0xB0, 0x01, 0xAB, 0xFF};
  char out[18];
  macText(mac, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("A4:2B:B0:01:AB:FF", out);
}

void test_phy_text() {
  char out[12];
  phyText(PHY_B | PHY_G | PHY_N, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("11b/g/n", out);
  phyText(PHY_N, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("11n", out);
  phyText(PHY_B | PHY_G | PHY_N | PHY_LR, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("11b/g/n/lr", out);
  phyText(0, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("11?", out);
}

void test_security_names() {
  TEST_ASSERT_EQUAL_STRING("open",      authName(AUTH_OPEN));
  TEST_ASSERT_EQUAL_STRING("WPA2",      authName(AUTH_WPA2));
  TEST_ASSERT_EQUAL_STRING("WPA2/WPA3", authName(AUTH_WPA2_WPA3));
  TEST_ASSERT_EQUAL_STRING("?",         authName(200));
  TEST_ASSERT_EQUAL_STRING("CCMP",      cipherName(CIPHER_CCMP));
  TEST_ASSERT_EQUAL_STRING("TKIP+CCMP", cipherName(CIPHER_TKIP_CCMP));
  TEST_ASSERT_EQUAL_STRING("?",         cipherName(CIPHER_SMS4));   // deliberately unnamed
  TEST_ASSERT_EQUAL_STRING("?",         cipherName(200));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_strcopy_truncates_and_terminates);
  RUN_TEST(test_ascii_only_replaces_control_and_high_bytes);
  RUN_TEST(test_json_append_escapes_and_respects_capacity);
  RUN_TEST(test_mac_text);
  RUN_TEST(test_phy_text);
  RUN_TEST(test_security_names);
  return UNITY_END();
}
