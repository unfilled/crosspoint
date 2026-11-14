// Repro for: drop-cap paragraphs laid out the lines BELOW the letter as if
// the letter still indented them — hyphens landed where the narrow
// (beside-the-letter) width would break, not at the full width.
// ChapterHtmlSlimParser drives ParsedText exactly this way for a drop-cap
// paragraph (narrow width with maxLines=letter-lines, then the rest at full
// width — see ChapterHtmlSlimParser::layoutDropCapLines), so the two-step
// call is reproduced here directly.
//
// Root cause: computeHyphenatedLineBreaks used to compute breaks for the
// WHOLE paragraph, and hyphenateWordAtIndex materializes every split in the
// word list (baked '-' + inserted remainder word) — including lines beyond
// maxLines that layoutAndExtractLines never consumed. Those narrow-width
// pieces survived into the full-width second call.
//
// Method: the stub renderer is monospaced (8px/char + 4px space) and the
// opt-in hyphenation hook (ParserLinkStubs.cpp) splits any first-on-line
// word every 5 bytes. Words are 40 chars (320px) — wider than any test
// line, so the narrow phase ends mid-word and any leaked narrow split shows
// up as a wrong piece or hyphen flag in the tail.

#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

// Opt-in hyphenation hook owned by ParserLinkStubs.cpp.
extern bool gTestHyphenationFallbackEnabled;

namespace {

const GfxRenderer& gRenderer() {
  static GfxRenderer renderer;
  return renderer;
}

struct LineWords {
  std::vector<std::string> words;
  std::vector<bool> hyphen;
};

template <typename Emit>
void runLayout(ParsedText& text, const int width, const size_t maxLines, Emit&& emit) {
  text.layoutAndExtractLines(
      gRenderer(), 0, static_cast<uint16_t>(width),
      [&](std::unique_ptr<TextBlock> line, uint32_t) {
        LineWords lw;
        for (uint16_t i = 0; i < line->wordCount(); i++) {
          lw.words.push_back(line->wordText(i));
          lw.hyphen.push_back(line->wordHasDiscretionaryHyphen(i));
        }
        emit(std::move(lw));
      },
      true, 0, 100, maxLines, nullptr);
}

struct HyphenationScope {
  HyphenationScope() { gTestHyphenationFallbackEnabled = true; }
  ~HyphenationScope() { gTestHyphenationFallbackEnabled = false; }
};

constexpr size_t kWordChars = 40;  // 320px: wider than any test line

void addWord(ParsedText& text, const char c) { text.addWord(std::string(kWordChars, c), EpdFontFamily::REGULAR); }

void expectSameLines(const std::vector<LineWords>& actual, const std::vector<LineWords>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); i++) {
    EXPECT_EQ(actual[i].words, expected[i].words) << "line " << i;
    EXPECT_EQ(actual[i].hyphen, expected[i].hyphen) << "line " << i;
  }
}

}  // namespace

// Two-phase drop-cap layout: 2 narrow (150px) lines, then the remainder at
// full width (200px). The tail must equal a fresh full-width layout of the
// characters the narrow phase did not consume.
TEST(DropCapHyphenation, TailAfterNarrowPhaseUsesFullWidth) {
  HyphenationScope hyphenation;

  std::vector<LineWords> lines;
  {
    BlockStyle style;
    ParsedText text(true, false, style, 0);
    for (char c = 'a'; c <= 'h'; c++) addWord(text, c);
    runLayout(text, 150, 2, [&](LineWords lw) { lines.push_back(std::move(lw)); });
    ASSERT_EQ(lines.size(), 2u) << "maxLines must bound the narrow phase";
    runLayout(text, 200, SIZE_MAX, [&](LineWords lw) { lines.push_back(std::move(lw)); });
  }

  // Characters the narrow phase consumed (a baked '-' is not source text).
  size_t consumed = 0;
  for (size_t i = 0; i < 2; i++) {
    ASSERT_EQ(lines[i].words.size(), 1u);
    ASSERT_TRUE(lines[i].hyphen[0]) << "narrow test lines must end mid-word";
    consumed += lines[i].words[0].size() - 1;
  }
  ASSERT_LT(consumed, kWordChars) << "the narrow/full boundary must stay inside word 0";

  // Reference: the remaining characters laid out at full width in one call —
  // the partial first word, then the untouched whole words.
  std::vector<LineWords> reference;
  {
    BlockStyle style;
    ParsedText text(true, false, style, 0);
    text.addWord(std::string(kWordChars - consumed, 'a'), EpdFontFamily::REGULAR);
    for (char c = 'b'; c <= 'h'; c++) addWord(text, c);
    runLayout(text, 200, SIZE_MAX, [&](LineWords lw) { reference.push_back(std::move(lw)); });
  }

  const std::vector<LineWords> tail(lines.begin() + 2, lines.end());
  expectSameLines(tail, reference);
}
