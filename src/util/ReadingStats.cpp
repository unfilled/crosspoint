#include "ReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace {

constexpr char STATS_PATH[] = "/.crosspoint/reading_stats.bin";
constexpr char STATS_TMP_PATH[] = "/.crosspoint/reading_stats.tmp";
constexpr uint32_t FORMAT_VERSION = 1;
constexpr size_t HEADER_BYTES = 8;       // "RSB1" + u32 version
constexpr size_t RECORD_FIXED = 20;      // fields below, little-endian
constexpr uint16_t MAX_PATH_BYTES = 512;

void putU16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

void putU64(uint8_t* p, const uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t getU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

void writeHeader(HalFile& out) {
  uint8_t header[HEADER_BYTES] = {'R', 'S', 'B', '1', 0, 0, 0, 0};
  putU32(header + 4, FORMAT_VERSION);
  out.write(header, sizeof(header));
}

// Serializes one record; returns false when the path is over the on-disk cap.
bool writeRecord(HalFile& out, const ReadingSessionRecord& rec) {
  if (rec.bookPath.size() > MAX_PATH_BYTES) return false;
  uint8_t fixed[RECORD_FIXED];
  putU64(fixed, static_cast<uint64_t>(rec.startEpoch));
  putU32(fixed + 8, rec.durationSec);
  putU16(fixed + 12, rec.startProgressBp);
  putU16(fixed + 14, rec.endProgressBp);
  putU16(fixed + 16, static_cast<uint16_t>(rec.bookPath.size()));
  putU16(fixed + 18, 0);
  if (out.write(fixed, sizeof(fixed)) != sizeof(fixed)) return false;
  return out.write(rec.bookPath.data(), rec.bookPath.size()) == rec.bookPath.size();
}

// Reads one record; false at EOF or on a torn tail (the caller treats both as
// end-of-data, so a half-written append is simply dropped).
bool readRecord(HalFile& in, ReadingSessionRecord& rec) {
  uint8_t fixed[RECORD_FIXED];
  if (in.read(fixed, sizeof(fixed)) != static_cast<int>(sizeof(fixed))) return false;
  const uint16_t pathLen = getU16(fixed + 16);
  if (pathLen == 0 || pathLen > MAX_PATH_BYTES) return false;
  char path[MAX_PATH_BYTES + 1];
  if (in.read(path, pathLen) != pathLen) return false;
  path[pathLen] = '\0';
  rec.startEpoch = static_cast<int64_t>(getU64(fixed));
  rec.durationSec = getU32(fixed + 8);
  rec.startProgressBp = getU16(fixed + 12);
  rec.endProgressBp = getU16(fixed + 14);
  rec.bookPath.assign(path, pathLen);
  return true;
}

bool headerLooksValid(HalFile& in) {
  uint8_t header[HEADER_BYTES];
  if (in.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  return std::memcmp(header, "RSB1", 4) == 0 && getU32(header + 4) == FORMAT_VERSION;
}

// The HAL has no append-mode open, so every mutation is a stream-rewrite
// through a temp file (one 20 B + path buffer live at a time). Flushes are
// rare (reader exit / sleep), so the copy cost is irrelevant. `append` is
// written after the copied records; renameFrom/renameTo re-point a book path.
bool rewriteStore(const ReadingSessionRecord* append, const char* renameFrom, const char* renameTo) {
  HalFile in;
  const bool haveOld = Storage.openFileForRead("STATS", STATS_PATH, in);
  HalFile out;
  if (!Storage.openFileForWrite("STATS", STATS_TMP_PATH, out)) {
    LOG_ERR("STATS", "cannot open temp store");
    if (in) in.close();
    return false;
  }
  bool ok = true;
  writeHeader(out);
  if (haveOld) {
    if (!headerLooksValid(in)) {
      LOG_ERR("STATS", "unknown store format, starting fresh");
    } else {
      ReadingSessionRecord rec;
      while (readRecord(in, rec)) {
        if (renameFrom && rec.bookPath == renameFrom) rec.bookPath = renameTo;
        if (!writeRecord(out, rec)) {
          ok = false;
          break;
        }
      }
    }
    in.close();
  }
  if (ok && append && !writeRecord(out, *append)) ok = false;
  out.close();
  if (!ok) {
    Storage.remove(STATS_TMP_PATH);
    return false;
  }
  Storage.remove(STATS_PATH);
  if (!Storage.rename(STATS_TMP_PATH, STATS_PATH)) {
    LOG_ERR("STATS", "cannot replace store");
    Storage.remove(STATS_TMP_PATH);
    return false;
  }
  return true;
}

int32_t localDayIndex(const int64_t epoch, const int32_t tzOffsetSec) {
  return static_cast<int32_t>((epoch + tzOffsetSec) / 86400);
}

}  // namespace

ReadingStatsSummary aggregateReadingStats(const std::vector<ReadingSessionRecord>& records, const int64_t nowEpoch,
                                          const int32_t tzOffsetSec) {
  ReadingStatsSummary summary;
  if (nowEpoch <= 0) return summary;

  const int32_t today = localDayIndex(nowEpoch, tzOffsetSec);
  constexpr int DAY_COUNT = 14;
  summary.days.resize(DAY_COUNT);
  for (int i = 0; i < DAY_COUNT; ++i) summary.days[i].dayIndex = today - (DAY_COUNT - 1 - i);

  std::map<std::string, ReadingStatsBook> byBook;
  for (const auto& rec : records) {
    ++summary.sessions;
    summary.totalSeconds += rec.durationSec;
    auto& book = byBook[rec.bookPath];
    if (book.path.empty()) book.path = rec.bookPath;
    book.seconds += rec.durationSec;
    ++book.sessions;

    const int32_t day = localDayIndex(rec.startEpoch, tzOffsetSec);
    const int age = today - day;
    if (age < 0 || age >= DAY_COUNT) continue;
    summary.days[DAY_COUNT - 1 - age].seconds += rec.durationSec;
    if (age == 0) summary.todaySeconds += rec.durationSec;
    if (age < 7) summary.weekSeconds += rec.durationSec;
  }

  summary.distinctBooks = static_cast<uint32_t>(byBook.size());
  summary.topBooks.reserve(byBook.size() < 10 ? byBook.size() : 10);
  for (auto& [path, book] : byBook) summary.topBooks.push_back(std::move(book));
  auto& top = summary.topBooks;
  std::sort(top.begin(), top.end(), [](const ReadingStatsBook& a, const ReadingStatsBook& b) {
    if (a.seconds != b.seconds) return a.seconds > b.seconds;
    return a.path < b.path;
  });
  if (top.size() > 10) top.resize(10);
  return summary;
}

bool ReadingStatsStore::addSession(const std::string& bookPath, const int64_t startEpoch, const uint32_t durationSec,
                                   const uint16_t startProgressBp, const uint16_t endProgressBp) {
  if (bookPath.empty() || startEpoch <= 0 || durationSec == 0) return false;
  const ReadingSessionRecord rec{startEpoch, durationSec, startProgressBp, endProgressBp, bookPath};
  return rewriteStore(&rec, nullptr, nullptr);
}

bool ReadingStatsStore::updatePath(const std::string& oldPath, const std::string& newPath) {
  if (oldPath.empty() || newPath.empty()) return false;
  if (!Storage.exists(STATS_PATH)) return true;  // nothing to re-point
  return rewriteStore(nullptr, oldPath.c_str(), newPath.c_str());
}

bool ReadingStatsStore::loadSummary(const int64_t nowEpoch, const int32_t tzOffsetSec, ReadingStatsSummary& out) {
  out = ReadingStatsSummary{};
  HalFile in;
  if (!Storage.openFileForRead("STATS", STATS_PATH, in)) return false;
  std::vector<ReadingSessionRecord> records;
  if (headerLooksValid(in)) {
    ReadingSessionRecord rec;
    while (readRecord(in, rec)) records.push_back(std::move(rec));
  } else {
    LOG_ERR("STATS", "unknown store format, ignoring");
  }
  in.close();
  out = aggregateReadingStats(records, nowEpoch, tzOffsetSec);
  return true;
}
