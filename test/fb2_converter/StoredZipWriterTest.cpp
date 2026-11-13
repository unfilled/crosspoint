#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "StoredZipWriter.h"

namespace {

// Independent bitwise CRC-32 (zlib polynomial), deliberately NOT the miniz
// table implementation the writer uses: a shared bug in table generation
// would otherwise cancel out.
uint32_t crc32Reference(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
  }
  return ~crc;
}

std::string tempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint16_t le16(const std::vector<uint8_t>& b, size_t off) { return b[off] | (b[off + 1] << 8); }
uint32_t le32(const std::vector<uint8_t>& b, size_t off) {
  return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

struct ZipEntry {
  std::string name;
  uint32_t crc = 0;
  uint32_t size = 0;
  uint32_t dataOffset = 0;
};

// Validate the full container structure and return the parsed entries via
// `entries`: local headers chained from offset 0, central directory matching
// them one to one, EOCD consistent with the directory. ASSERT_* bounds-checks
// every step so a malformed container fails the test instead of reading out
// of bounds.
void validateZip(const std::vector<uint8_t>& b, const std::string& context, std::vector<ZipEntry>& entries) {
  entries.clear();
  ASSERT_GE(b.size(), 22u) << context;

  // EOCD must be the last 22 bytes (no comment is ever written).
  const size_t eocd = b.size() - 22;
  ASSERT_EQ(le32(b, eocd), 0x06054B50u) << context;
  const uint16_t count = le16(b, eocd + 10);
  const uint32_t cdSize = le32(b, eocd + 12);
  const uint32_t cdOffset = le32(b, eocd + 16);
  ASSERT_EQ(cdOffset + cdSize, eocd) << context << ": central directory not contiguous";

  // Walk local headers from the start.
  size_t off = 0;
  while (off < cdOffset) {
    ASSERT_LE(off + 30, b.size()) << context << ": truncated local header";
    ASSERT_EQ(le32(b, off), 0x04034B50u) << context << ": bad local sig at " << off;
    EXPECT_EQ(le16(b, off + 8), 0) << context << ": method must be stored";
    const uint32_t crc = le32(b, off + 14);
    const uint32_t size = le32(b, off + 18);
    EXPECT_EQ(le32(b, off + 22), size) << context << ": compressed/uncompressed differ";
    const uint16_t nameLen = le16(b, off + 26);
    EXPECT_EQ(le16(b, off + 28), 0) << context << ": unexpected extra field";
    ZipEntry e;
    ASSERT_LE(off + 30 + nameLen + (size_t)size, b.size()) << context << ": truncated entry";
    e.name.assign(reinterpret_cast<const char*>(b.data() + off + 30), nameLen);
    e.crc = crc;
    e.size = size;
    e.dataOffset = off + 30 + nameLen;
    EXPECT_EQ(crc32Reference(b.data() + e.dataOffset, size), crc) << context << ": CRC mismatch for " << e.name;
    entries.push_back(e);
    off = e.dataOffset + size;
  }
  EXPECT_EQ(off, cdOffset) << context << ": local entries do not tile up to the directory";
  EXPECT_EQ(entries.size(), static_cast<size_t>(count)) << context;

  // Central directory mirrors the locals.
  size_t cd = cdOffset;
  for (const ZipEntry& e : entries) {
    ASSERT_LE(cd + 46, b.size()) << context << ": truncated central header";
    ASSERT_EQ(le32(b, cd), 0x02014B50u) << context;
    EXPECT_EQ(le16(b, cd + 10), 0) << context;
    EXPECT_EQ(le32(b, cd + 16), e.crc) << context;
    EXPECT_EQ(le32(b, cd + 20), e.size) << context;
    EXPECT_EQ(le32(b, cd + 24), e.size) << context;
    const uint16_t nameLen = le16(b, cd + 28);
    ASSERT_LE(cd + 46 + nameLen, b.size()) << context << ": truncated central name";
    EXPECT_EQ(le32(b, cd + 42), e.dataOffset - 30 - e.name.size()) << context << ": local header offset";
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data() + cd + 46), nameLen), e.name) << context;
    cd += 46 + nameLen;
  }
  EXPECT_EQ(cd, eocd) << context;
}

}  // namespace

TEST(StoredZipWriter, Crc32ReferenceKnownVector) {
  // The textbook check value: CRC-32 of "123456789" is 0xCBF43926.
  const char* s = "123456789";
  EXPECT_EQ(crc32Reference(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

TEST(StoredZipWriter, SingleEntryRoundTrip) {
  const std::string path = tempPath("fb2zw_single.zip");
  {
    StoredZipWriter zip;
    ASSERT_TRUE(zip.open(path));
    ASSERT_TRUE(zip.startEntry("a.txt"));
    ASSERT_TRUE(zip.writeStr("hello"));
    ASSERT_TRUE(zip.finishEntry());
    ASSERT_TRUE(zip.finish());
  }
  const auto bytes = readFile(path);
  std::vector<ZipEntry> entries;
  validateZip(bytes, "single", entries);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "a.txt");
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data() + entries[0].dataOffset), entries[0].size),
            "hello");
  std::remove(path.c_str());
}

TEST(StoredZipWriter, MultiEntryWithLargeAndEmptyPayloads) {
  const std::string path = tempPath("fb2zw_multi.zip");
  std::vector<uint8_t> big(200 * 1024);
  for (size_t i = 0; i < big.size(); i++) big[i] = static_cast<uint8_t>(i * 31u + 7u);
  {
    StoredZipWriter zip;
    ASSERT_TRUE(zip.open(path));
    ASSERT_TRUE(zip.startEntry("empty"));
    ASSERT_TRUE(zip.finishEntry());  // zero-length entry
    ASSERT_TRUE(zip.startEntry("big.bin"));
    // Feed in odd-sized pieces to exercise the 4 KB io buffer boundary.
    size_t off = 0;
    while (off < big.size()) {
      const size_t n = std::min<size_t>(1000, big.size() - off);
      ASSERT_TRUE(zip.write(big.data() + off, n));
      off += n;
    }
    ASSERT_TRUE(zip.finishEntry());
    ASSERT_TRUE(zip.startEntry("META-INF/container.xml"));
    ASSERT_TRUE(zip.writeStr("<?xml version=\"1.0\"?><container/>"));
    ASSERT_TRUE(zip.finishEntry());
    ASSERT_TRUE(zip.finish());
    EXPECT_EQ(zip.entryCount(), 3u);
  }
  const auto bytes = readFile(path);
  std::vector<ZipEntry> entries;
  validateZip(bytes, "multi", entries);
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].size, 0u);
  EXPECT_EQ(entries[1].size, big.size());
  EXPECT_TRUE(std::equal(big.begin(), big.end(), bytes.begin() + entries[1].dataOffset));
  std::remove(path.c_str());
}

TEST(StoredZipWriter, AbortedZipHasNoCentralDirectory) {
  const std::string path = tempPath("fb2zw_abort.zip");
  {
    StoredZipWriter zip;
    ASSERT_TRUE(zip.open(path));
    ASSERT_TRUE(zip.startEntry("partial"));
    ASSERT_TRUE(zip.writeStr("some bytes"));
    zip.abort();
    EXPECT_FALSE(zip.ok());
    EXPECT_FALSE(zip.writeStr("more"));
  }
  const auto bytes = readFile(path);
  ASSERT_FALSE(bytes.empty());
  // No EOCD signature anywhere: the file is an incomplete stub.
  for (size_t i = 0; i + 4 <= bytes.size(); i++) {
    EXPECT_NE(le32(bytes, i), 0x06054B50u) << "EOCD found in aborted zip at " << i;
  }
  std::remove(path.c_str());
}
