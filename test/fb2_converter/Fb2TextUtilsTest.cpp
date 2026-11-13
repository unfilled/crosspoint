#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Fb2Passes.h"

namespace {

namespace fp = fb2passes;

TEST(Fb2TextUtils, Utf8SafeLenKeepsAsciiIntact) {
  EXPECT_EQ(fp::utf8SafeLen("hello", 5), 5u);
  EXPECT_EQ(fp::utf8SafeLen("hello", 3), 3u);
  EXPECT_EQ(fp::utf8SafeLen("", 0), 0u);
}

TEST(Fb2TextUtils, Utf8SafeLenBacksOffSplitCodepoint) {
  // "Привет" = 6 Cyrillic letters, 2 bytes each.
  const char* s = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
  EXPECT_EQ(fp::utf8SafeLen(s, 12), 12u);
  EXPECT_EQ(fp::utf8SafeLen(s, 11), 10u);  // 11 cuts "т" in half -> drop it
  EXPECT_EQ(fp::utf8SafeLen(s, 1), 0u);    // single byte of a 2-byte sequence -> nothing
}

TEST(Fb2TextUtils, Utf8SafeLenThreeByteSequence) {
  const char* s = "a\xE2\x80\x94b";  // "a" + em-dash + "b" = 5 bytes
  EXPECT_EQ(fp::utf8SafeLen(s, 5), 5u);
  EXPECT_EQ(fp::utf8SafeLen(s, 4), 4u);  // exactly after the dash
  EXPECT_EQ(fp::utf8SafeLen(s, 3), 1u);  // mid-dash -> keep only "a"
  EXPECT_EQ(fp::utf8SafeLen(s, 2), 1u);
}

TEST(Fb2TextUtils, TextCaptureTrimsToCodepointBoundary) {
  fp::TextCapture cap;
  cap.start();
  // Fill to the cap with ASCII, then append a 2-byte Cyrillic letter that
  // splits exactly at the cap: 159 'a' + first byte of 0xD0 xx.
  std::string bulk(159, 'a');
  cap.feed(bulk.data(), (int)bulk.size());
  const char partial[2] = {(char)0xD0, (char)0x90};
  cap.feed(partial, 2);  // only the first byte fits (cap is 160)
  cap.stop(true);
  ASSERT_TRUE(cap.taken);
  EXPECT_EQ(strlen(cap.text()), 159u);  // dangling lead byte trimmed
}

TEST(Fb2TextUtils, DetectDialogDashPrefixPositive) {
  EXPECT_EQ(fp::detectDialogDashPrefix("\xE2\x80\x94 \xD0\x9F", 5), 1);
  EXPECT_EQ(fp::detectDialogDashPrefix("  \xE2\x80\x94 x", 7), 1);  // leading spaces allowed
  EXPECT_EQ(fp::detectDialogDashPrefix("\t\xE2\x80\x94 ", 5), 1);
}

TEST(Fb2TextUtils, DetectDialogDashPrefixNegative) {
  EXPECT_EQ(fp::detectDialogDashPrefix("\xD0\x9F\xD1\x80\xD0\xB8", 4), 0);  // Cyrillic word
  EXPECT_EQ(fp::detectDialogDashPrefix("\xE2\x80\x94, x", 5), 0);           // dash but no space
  EXPECT_EQ(fp::detectDialogDashPrefix("abc", 3), 0);
}

TEST(Fb2TextUtils, DetectDialogDashPrefixUndecidedOnSplitDash) {
  EXPECT_EQ(fp::detectDialogDashPrefix("", 0), -1);
  EXPECT_EQ(fp::detectDialogDashPrefix("  ", 2), -1);
  EXPECT_EQ(fp::detectDialogDashPrefix("\xE2", 1), -1);
  EXPECT_EQ(fp::detectDialogDashPrefix("\xE2\x80", 2), -1);
  EXPECT_EQ(fp::detectDialogDashPrefix("\xE2\x80\x94", 3), -1);  // dash, space not seen yet
}

}  // namespace
