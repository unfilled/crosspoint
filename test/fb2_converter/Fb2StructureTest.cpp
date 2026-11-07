#include <gtest/gtest.h>

#include <expat.h>

#include <algorithm>
#include <string>

#include "Fb2Passes.h"

namespace {

namespace fp = fb2passes;

// Mirrors Fb2Converter::convert pass 2: expat with the early stop at the
// first <binary>, then the raw tail scan for the remaining registry entries.
fp::StructurePass runStructure(const std::string& doc) {
  XML_Parser p = XML_ParserCreate(nullptr);
  EXPECT_NE(p, nullptr);
  fp::StructurePass s;
  s.parser = p;
  XML_SetElementHandler(p, fp::structStart, fp::structEnd);
  XML_SetUserData(p, &s);
  if (XML_Parse(p, doc.data(), static_cast<int>(doc.size()), 1) == XML_STATUS_ERROR) {
    EXPECT_EQ(XML_GetErrorCode(p), XML_ERROR_ABORTED) << XML_ErrorString(XML_GetErrorCode(p));
  }
  XML_ParserFree(p);
  if (s.binaryScanRequested) {
    const size_t start = s.binaryStartByte > 16 ? static_cast<size_t>(s.binaryStartByte) - 16 : 0;
    fp::RawBinaryScanner scanner;
    scanner.feed(doc.data() + start, doc.size() - start, s);
    scanner.finish(s);
  }
  return s;
}

// Same, but feeding the tail in tiny chunks to exercise carry boundaries.
fp::StructurePass runStructureChunked(const std::string& doc, size_t chunk) {
  XML_Parser p = XML_ParserCreate(nullptr);
  EXPECT_NE(p, nullptr);
  fp::StructurePass s;
  s.parser = p;
  XML_SetElementHandler(p, fp::structStart, fp::structEnd);
  XML_SetUserData(p, &s);
  if (XML_Parse(p, doc.data(), static_cast<int>(doc.size()), 1) == XML_STATUS_ERROR) {
    EXPECT_EQ(XML_GetErrorCode(p), XML_ERROR_ABORTED) << XML_ErrorString(XML_GetErrorCode(p));
  }
  XML_ParserFree(p);
  if (s.binaryScanRequested) {
    const size_t start = s.binaryStartByte > 16 ? static_cast<size_t>(s.binaryStartByte) - 16 : 0;
    fp::RawBinaryScanner scanner;
    for (size_t off = start; off < doc.size(); off += chunk) {
      scanner.feed(doc.data() + off, std::min(chunk, doc.size() - off), s);
    }
    scanner.finish(s);
  }
  return s;
}

}  // namespace

TEST(Fb2Structure, ChaptersForBodiesAndNotes) {
  const auto s = runStructure(
      "<FictionBook>"
      "<description/>"
      "<body><title/><section id=\"s1\"/><section id=\"s2\"/></body>"       // ch 0 (front), 1, 2
      "<body name=\"notes\"><title/><section id=\"n1\"/><section id=\"n2\"/>"
      "<section id=\"n3\"/></body>"                                          // ch 3 (notes head), 4, 5, 6
      "</FictionBook>");
  // front-matter slot (0) + 2 sections; notes head slot (3) + 3 sections.
  EXPECT_EQ(s.chapterCursor, 6);
  EXPECT_EQ(s.bodyCount, 2);
  EXPECT_EQ(s.idTargets.size(), 5u);
  EXPECT_EQ(s.findIdChapter("s1"), 1);
  EXPECT_EQ(s.findIdChapter("s2"), 2);
  EXPECT_EQ(s.findIdChapter("n1"), 4);
  EXPECT_EQ(s.findIdChapter("n3"), 6);
}

TEST(Fb2Structure, NestedSectionsStayInTheTopLevelChapter) {
  const auto s = runStructure(
      "<FictionBook><body>"
      "<section id=\"part1\"><section id=\"ch1a\"/><section id=\"ch1b\"/></section>"
      "<section id=\"part2\"/>"
      "</body></FictionBook>");
  EXPECT_EQ(s.chapterCursor, 2);  // front slot + part1 + part2
  EXPECT_EQ(s.findIdChapter("part1"), 1);
  EXPECT_EQ(s.findIdChapter("ch1a"), 1);  // nested ids resolve to their top-level chapter
  EXPECT_EQ(s.findIdChapter("ch1b"), 1);
  EXPECT_EQ(s.findIdChapter("part2"), 2);
}

TEST(Fb2Structure, BinaryRegistryByContentType) {
  const auto s = runStructure(
      "<FictionBook><body><section/></body>"
      "<binary id=\"cover.jpg\" content-type=\"image/jpeg\">Zm9v</binary>"
      "<binary id=\"pic\" content-type=\"image/png\">YmFy</binary>"
      "<binary id=\"clip\" content-type=\"video/mp4\">eA==</binary>"
      "</FictionBook>");
  ASSERT_EQ(s.binaries.size(), 3u);
  EXPECT_EQ(s.findBinary("cover.jpg"), 0);
  EXPECT_STREQ(s.binaries[0].ext, ".jpg");
  EXPECT_STREQ(s.binaries[1].ext, ".png");
  EXPECT_STREQ(s.binaries[2].ext, "");  // unsupported -> empty ext, emission skips it
}

TEST(Fb2Structure, DuplicateIdFirstWins) {
  const auto s = runStructure(
      "<FictionBook><body><section id=\"dup\"/><section id=\"dup\"/></body></FictionBook>");
  ASSERT_EQ(s.idTargets.size(), 1u);
  EXPECT_EQ(s.findIdChapter("dup"), 1);
}

TEST(Fb2Structure, EmptyBodyStillCounts) {
  const auto s = runStructure("<FictionBook><body/></FictionBook>");
  EXPECT_EQ(s.chapterCursor, 0);  // the front-matter slot, no sections
}

TEST(Fb2Structure, BinaryTailScanStopsParseAndFindsAll) {
  const auto s = runStructure(
      "<FictionBook><body><section id=\"s1\"/></body>"
      "<binary id=\"a\" content-type=\"image/jpeg\">Zm9v</binary>"
      "<binary id=\"b\" content-type=\"image/png\">YmFy</binary>"
      "<binary id=\"c\" content-type=\"image/gif\">eA==</binary>"
      "</FictionBook>");
  EXPECT_TRUE(s.binaryScanRequested);  // expat stopped at the first binary
  EXPECT_EQ(s.findIdChapter("s1"), 1);
  ASSERT_EQ(s.binaries.size(), 3u);
  EXPECT_EQ(s.findBinary("a"), 0);
  EXPECT_EQ(s.findBinary("b"), 1);
  EXPECT_EQ(s.findBinary("c"), 2);
  EXPECT_STREQ(s.binaries[2].ext, ".gif");
}

TEST(Fb2Structure, BinaryTailScanAcrossChunkBoundaries) {
  const std::string doc =
      "<FictionBook><body><section/></body>"
      "<binary id=\"first\" content-type=\"image/jpeg\">Zm9v</binary>"
      "<binary id=\"second\" content-type=\"image/png\">YmFy</binary>"
      "<binary id='third' content-type='image/gif'>eA==</binary>"  // single quotes
      "</FictionBook>";
  for (const size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(64)}) {
    const auto s = runStructureChunked(doc, chunk);
    ASSERT_EQ(s.binaries.size(), 3u) << "chunk=" << chunk;
    EXPECT_EQ(s.findBinary("first"), 0);
    EXPECT_EQ(s.findBinary("second"), 1);
    EXPECT_EQ(s.findBinary("third"), 2);
    EXPECT_STREQ(s.binaries[1].ext, ".png");
    EXPECT_STREQ(s.binaries[2].ext, ".gif");
  }
}

TEST(Fb2Structure, BinaryTailScanSkipsLookalikes) {
  const auto s = runStructure(
      "<FictionBook><body><section/></body>"
      "<binary id=\"real\" content-type=\"image/png\">Zm9v</binary>"
      "<binaryfoo id=\"nope\" content-type=\"image/jpeg\"/>"   // longer element name
      "<xbinary id=\"nope2\" content-type=\"image/jpeg\"/>"    // prefix before '<'
      "<binary content-type=\"image/jpeg\"/>"                  // no id -> useless, skipped
      "</FictionBook>");
  ASSERT_EQ(s.binaries.size(), 1u);
  EXPECT_EQ(s.findBinary("real"), 0);
}

TEST(Fb2Structure, BinaryTailScanDedupesRepeatedIds) {
  const auto s = runStructure(
      "<FictionBook><body><section/></body>"
      "<binary id=\"same\" content-type=\"image/png\">Zm9v</binary>"
      "<binary id=\"same\" content-type=\"image/jpeg\">YmFy</binary>"
      "</FictionBook>");
  ASSERT_EQ(s.binaries.size(), 1u);
  EXPECT_STREQ(s.binaries[0].ext, ".png");  // first definition wins
}

TEST(Fb2Structure, BinaryInsideBodyDoesNotStopTheParse) {
  // Pathological, but the early stop must not fire mid-body: chapters after
  // it would otherwise be lost.
  const auto s = runStructure(
      "<FictionBook><body><section id=\"a\"/><binary id=\"mid\" content-type=\"image/png\">eA==</binary>"
      "<section id=\"b\"/></body>"
      "<binary id=\"tail\" content-type=\"image/jpeg\">Zm9v</binary>"
      "</FictionBook>");
  EXPECT_EQ(s.findIdChapter("b"), 2);
  EXPECT_TRUE(s.binaryScanRequested);  // stopped only at the real tail binary
  ASSERT_EQ(s.binaries.size(), 1u);    // mid-body binary is not registered (same as before)
  EXPECT_EQ(s.findBinary("tail"), 0);
}

TEST(Fb2Structure, BinarySignatureCutAtWindowEnd) {
  // "<binary" split right after '<' / mid-signature at every possible window
  // boundary must neither crash nor lose the entry (regression: the scanner
  // used to read one byte past the window when the tag started flush at end).
  const std::string tail = "<binary id=\"edge\" content-type=\"image/png\">Zm9v</binary>";
  for (size_t cut = 1; cut < tail.size(); cut++) {
    fp::StructurePass s;
    fp::RawBinaryScanner scanner;
    scanner.feed(tail.data(), cut, s);
    scanner.feed(tail.data() + cut, tail.size() - cut, s);
    scanner.finish(s);
    ASSERT_EQ(s.binaries.size(), 1u) << "cut=" << cut;
    EXPECT_EQ(s.findBinary("edge"), 0) << "cut=" << cut;
  }
}
