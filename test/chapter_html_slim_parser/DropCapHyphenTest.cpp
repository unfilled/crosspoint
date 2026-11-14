// Repro for: drop-cap paragraphs lay out hyphenation of the lines BELOW the
// letter as if the letter still indented them — hyphens land where the narrow
// (beside-the-letter) width would break, not the full width.
//
// Method: the stub renderer is monospaced (8px/char + 4px space), so line
// breaks are fully deterministic. The same long-word paragraph is laid out
// twice — with and without a 4-line drop cap — and every line after the
// spanned ones must be IDENTICAL between the two runs (same words, same
// hyphen splits). Any difference means the post-cap lines inherited the
// narrow width.

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// Same suite convention as ChapterHtmlSlimParserTest.cpp: expose the parser's
// private SAX entry points for this translation unit only.
#define class struct
#define private public
#include "ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

struct DropCapHyphenRepro : ::testing::Test {
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  std::vector<std::unique_ptr<Page>> pages;
  std::optional<ChapterHtmlSlimParser> parser;

  void makeParser(const bool hyphenation) {
    parser.emplace(nullptr, "unused.xhtml", renderer, 0, 1.0f, false, 0,
                   static_cast<uint16_t>(renderer.getScreenWidth()),
                   static_cast<uint16_t>(renderer.getScreenHeight()), hyphenation, false,
                   [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
                     pages.push_back(std::move(page));
                   },
                   true, "", "", 0, std::vector<std::string>{}, nullptr, &cssParser);
    parser->beginParse();
  }

  static void open(ChapterHtmlSlimParser& p, const char* tag, const char* style) {
    const XML_Char* attributes[] = {"style", style, nullptr};
    ChapterHtmlSlimParser::startElement(&p, tag, style ? attributes : nullptr);
  }
  static void text(ChapterHtmlSlimParser& p, const std::string& value) {
    ChapterHtmlSlimParser::characterData(&p, value.c_str(), static_cast<int>(value.size()));
  }
  static void close(ChapterHtmlSlimParser& p, const char* tag) { ChapterHtmlSlimParser::endElement(&p, tag); }

  // Long Russian words so the ru hyphenation trie actually splits them at
  // deterministic points in the monospaced stub metrics.
  static std::string paragraph() {
    std::string s;
    for (int i = 0; i < 14; i++) {
      s += "представительство окончательности ";
    }
    return s;
  }

  struct LineWords {
    int xPos = 0;
    std::vector<std::string> words;
    std::vector<bool> hyphen;  // word carries a discretionary hyphen
  };

  static std::vector<LineWords> collect(const std::vector<std::unique_ptr<Page>>& pages) {
    std::vector<LineWords> out;
    for (const auto& page : pages) {
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const auto* line = static_cast<const PageLine*>(el.get());
        const TextBlock& b = *line->getBlock();
        LineWords lw;
        lw.xPos = line->xPos;
        for (uint16_t i = 0; i < b.wordCount(); i++) {
          lw.words.push_back(b.wordText(i));
          lw.hyphen.push_back(b.wordHasDiscretionaryHyphen(i));
        }
        out.push_back(std::move(lw));
      }
    }
    return out;
  }
};

TEST_F(DropCapHyphenRepro, LinesBelowTheLetterMatchFullWidthLayout) {
  // Reference run: same paragraph, no drop cap, hyphenation on.
  makeParser(true);
  open(*parser, "p", nullptr);
  text(*parser, paragraph());
  close(*parser, "p");
  const auto reference = collect(pages);

  // Drop-cap run: initial-letter:4 spans four lines.
  pages.clear();
  makeParser(true);
  open(*parser, "p", nullptr);
  open(*parser, "span", "float: left; initial-letter: 4");
  text(*parser, "О");
  close(*parser, "span");
  text(*parser, "тContextHolder " + paragraph());
  close(*parser, "p");
  const auto withCap = collect(pages);

  ASSERT_FALSE(reference.empty());
  ASSERT_GT(withCap.size(), 5u) << "expected beside-lines plus full-width tail";

  // First four lines sit beside the letter (indented); from line 5 on the
  // layout must match the no-cap reference EXACTLY — same words, same
  // hyphenation points. Offset by one word: the cap run starts with the
  // paragraph text after the span, so compare the cap run's tail against the
  // reference tail (drop the first word of the first reference line only if
  // the cap run's text differs by a leading word).
  const size_t spanned = 4;
  ASSERT_GT(withCap.size(), spanned) << "paragraph did not fill the letter's lines";
  for (size_t i = 0; i < spanned; i++) EXPECT_GT(withCap[i].xPos, 0) << "line " << i << " should be indented";
  EXPECT_EQ(withCap[spanned].xPos, 0) << "first line below the letter should be at the margin";

  // Compare tails: everything from the first at-margin line must equal the
  // reference run's corresponding lines. Align by first word content.
  size_t refStart = 0;
  const std::string firstTailWord = withCap[spanned].words.empty() ? "" : withCap[spanned].words[0];
  for (size_t r = 0; r < reference.size(); r++) {
    if (!reference[r].words.empty() && reference[r].words[0] == firstTailWord) {
      refStart = r;
      break;
    }
  }
  const size_t tailLen = std::min(reference.size() - refStart, withCap.size() - spanned);
  ASSERT_GT(tailLen, 1u);
  for (size_t i = 0; i < tailLen; i++) {
    EXPECT_EQ(withCap[spanned + i].words, reference[refStart + i].words)
        << "tail line " << i << " words differ (narrow-width hyphenation leaked below the letter)";
    EXPECT_EQ(withCap[spanned + i].hyphen, reference[refStart + i].hyphen)
        << "tail line " << i << " hyphen marks differ";
  }
}

}  // namespace
