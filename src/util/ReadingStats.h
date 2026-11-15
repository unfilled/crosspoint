#pragma once

// Reading statistics: a small append-only store of completed reading sessions
// plus the aggregation behind the stats screen.
//
// Sessions are recorded from ReaderActivity::flushReaderSession() (the same
// place that feeds the plugin event), so only "emit-worthy" sessions land
// here: wall-clock time was trusted and at least ~10 s of reading time
// accumulated. Page-level granularity stays inside ReaderSession; this store
// keeps one row per flush (reader exit / sleep).
//
// File format (docs/file-formats.md): /.crosspoint/reading_stats.bin
//   header  8 bytes: "RSB1" + u32 version (little-endian)
//   record  20 bytes fixed + path bytes:
//           i64 startEpoch, u32 durationSec, u16 startProgressBp,
//           u16 endProgressBp, u16 pathLen, u16 reserved(0), char path[pathLen]
// A torn tail record (power loss mid-append) is dropped on read.

#include <cstdint>
#include <string>
#include <vector>

struct ReadingSessionRecord {
  int64_t startEpoch = 0;
  uint32_t durationSec = 0;
  uint16_t startProgressBp = 0;
  uint16_t endProgressBp = 0;
  std::string bookPath;
};

struct ReadingStatsDay {
  int32_t dayIndex = 0;  // local days since the Unix epoch
  uint32_t seconds = 0;
};

struct ReadingStatsBook {
  std::string path;
  uint32_t seconds = 0;
  uint32_t sessions = 0;
};

struct ReadingStatsSummary {
  uint32_t sessions = 0;
  uint64_t totalSeconds = 0;
  uint32_t todaySeconds = 0;
  uint32_t weekSeconds = 0;  // last 7 local days, including today
  uint32_t distinctBooks = 0;
  std::vector<ReadingStatsDay> days;       // last 14 local days, oldest first, zero-filled
  std::vector<ReadingStatsBook> topBooks;  // up to 10, seconds descending
};

// Pure aggregation (host-testable): bucket sessions into local days using the
// passed UTC offset; `nowEpoch` <= 0 yields an empty summary.
ReadingStatsSummary aggregateReadingStats(const std::vector<ReadingSessionRecord>& records, int64_t nowEpoch,
                                          int32_t tzOffsetSec);

class ReadingStatsStore {
 public:
  static ReadingStatsStore& getInstance() {
    static ReadingStatsStore instance;
    return instance;
  }

  // Appends one session (stream-rewrite via temp file; the store has no
  // append-mode open in the HAL).
  bool addSession(const std::string& bookPath, int64_t startEpoch, uint32_t durationSec, uint16_t startProgressBp,
                  uint16_t endProgressBp);
  // Re-point rows after a book rename (called from the file browser).
  bool updatePath(const std::string& oldPath, const std::string& newPath);
  // Reads the whole store and aggregates it for the screen.
  bool loadSummary(int64_t nowEpoch, int32_t tzOffsetSec, ReadingStatsSummary& out);

 private:
  ReadingStatsStore() = default;
};

#define READING_STATS ReadingStatsStore::getInstance()
