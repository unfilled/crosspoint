#include <gtest/gtest.h>

#include <HalStorage.h>

#include "ReadingStats.h"

namespace {

constexpr int64_t NOW = 1800000000;              // arbitrary fixed "now"
constexpr int32_t TZ = 6 * 3600;                 // UTC+6
constexpr int64_t DAY = 86400;

ReadingSessionRecord rec(const char* path, const int64_t startEpoch, const uint32_t durationSec) {
  ReadingSessionRecord r;
  r.startEpoch = startEpoch;
  r.durationSec = durationSec;
  r.startProgressBp = 100;
  r.endProgressBp = 200;
  r.bookPath = path;
  return r;
}

class StoreTest : public ::testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
};

}  // namespace

// ---------------------------------------------------------------- pure core ---

TEST(ReadingStatsAggregate, EmptyRecordsGiveZeroedSummary) {
  const auto s = aggregateReadingStats({}, NOW, TZ);
  EXPECT_EQ(s.sessions, 0u);
  EXPECT_EQ(s.totalSeconds, 0u);
  EXPECT_EQ(s.distinctBooks, 0u);
  EXPECT_EQ(s.days.size(), 14u);
  EXPECT_TRUE(s.topBooks.empty());
}

TEST(ReadingStatsAggregate, UntrustedNowYieldsEmptySummary) {
  const auto s = aggregateReadingStats({rec("/a.epub", NOW - 60, 120)}, 0, TZ);
  EXPECT_EQ(s.sessions, 0u);
  EXPECT_TRUE(s.days.empty());
}

TEST(ReadingStatsAggregate, SessionTodayCountsEverywhere) {
  const auto s = aggregateReadingStats({rec("/a.epub", NOW - 60, 120)}, NOW, TZ);
  EXPECT_EQ(s.sessions, 1u);
  EXPECT_EQ(s.totalSeconds, 120u);
  EXPECT_EQ(s.todaySeconds, 120u);
  EXPECT_EQ(s.weekSeconds, 120u);
  EXPECT_EQ(s.distinctBooks, 1u);
  ASSERT_EQ(s.days.size(), 14u);
  EXPECT_EQ(s.days.back().seconds, 120u);  // last bucket is today
  ASSERT_EQ(s.topBooks.size(), 1u);
  EXPECT_EQ(s.topBooks[0].path, "/a.epub");
}

TEST(ReadingStatsAggregate, OldSessionLeavesRecentBuckets) {
  const auto s = aggregateReadingStats({rec("/a.epub", NOW - 20 * DAY, 300)}, NOW, TZ);
  EXPECT_EQ(s.totalSeconds, 300u);
  EXPECT_EQ(s.todaySeconds, 0u);
  EXPECT_EQ(s.weekSeconds, 0u);
  for (const auto& d : s.days) EXPECT_EQ(d.seconds, 0u);
}

TEST(ReadingStatsAggregate, LocalMidnightBoundaryUsesTzOffset) {
  // One hour before local midnight: with UTC+6 that is 18:00 UTC today but
  // 00:00 local tomorrow boundary math puts it in the previous local day.
  const int64_t localMidnightUtc = (NOW + TZ) / DAY * DAY - TZ;  // today's 00:00 local, in UTC
  const auto s = aggregateReadingStats({rec("/a.epub", localMidnightUtc - 3600, 60)}, NOW, TZ);
  ASSERT_EQ(s.days.size(), 14u);
  EXPECT_EQ(s.days.back().seconds, 0u);          // nothing today
  EXPECT_EQ(s.days[s.days.size() - 2].seconds, 60u);  // everything yesterday
  EXPECT_EQ(s.todaySeconds, 0u);
  EXPECT_EQ(s.weekSeconds, 60u);                 // but still inside the week
}

TEST(ReadingStatsAggregate, TopBooksSortedAndCapped) {
  std::vector<ReadingSessionRecord> records;
  for (int i = 0; i < 12; ++i) {
    const std::string path = "/book" + std::to_string(i) + ".epub";
    records.push_back(rec(path.c_str(), NOW - 60, static_cast<uint32_t>((i + 1) * 10)));
  }
  const auto s = aggregateReadingStats(records, NOW, TZ);
  EXPECT_EQ(s.distinctBooks, 12u);
  ASSERT_EQ(s.topBooks.size(), 10u);
  EXPECT_EQ(s.topBooks[0].seconds, 120u);
  for (size_t i = 1; i < s.topBooks.size(); ++i) {
    EXPECT_GE(s.topBooks[i - 1].seconds, s.topBooks[i].seconds);
  }
}

// ------------------------------------------------------------- store (fake FS) ---

TEST_F(StoreTest, AddSessionsRoundTripsThroughTheFile) {
  auto& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.addSession("/a.epub", NOW - 60, 120, 0, 100));
  ASSERT_TRUE(store.addSession("/b.epub", NOW - 30, 60, 100, 200));

  ReadingStatsSummary s;
  ASSERT_TRUE(store.loadSummary(NOW, TZ, s));
  EXPECT_EQ(s.sessions, 2u);
  EXPECT_EQ(s.totalSeconds, 180u);
  EXPECT_EQ(s.distinctBooks, 2u);
}

TEST_F(StoreTest, UpdatePathRepointsExistingRecords) {
  auto& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.addSession("/old.epub", NOW - 60, 120, 0, 100));
  ASSERT_TRUE(store.updatePath("/old.epub", "/new.epub"));

  ReadingStatsSummary s;
  ASSERT_TRUE(store.loadSummary(NOW, TZ, s));
  ASSERT_EQ(s.topBooks.size(), 1u);
  EXPECT_EQ(s.topBooks[0].path, "/new.epub");
}

TEST_F(StoreTest, UpdatePathWithoutStoreIsANoOp) {
  EXPECT_TRUE(ReadingStatsStore::getInstance().updatePath("/old.epub", "/new.epub"));
}

TEST_F(StoreTest, TornTailRecordIsDropped) {
  auto& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.addSession("/a.epub", NOW - 60, 120, 0, 100));
  // Corrupt: chop the file mid-record (power loss mid-append).
  auto& bytes = fake::files["/.crosspoint/reading_stats.bin"]->bytes;
  bytes.resize(bytes.size() - 3);

  ReadingStatsSummary s;
  ASSERT_TRUE(store.loadSummary(NOW, TZ, s));
  EXPECT_EQ(s.sessions, 0u);

  // The store recovers: a new append rewrites from scratch-less state.
  ASSERT_TRUE(store.addSession("/b.epub", NOW - 30, 60, 0, 100));
  ASSERT_TRUE(store.loadSummary(NOW, TZ, s));
  EXPECT_EQ(s.sessions, 1u);
  EXPECT_EQ(s.totalSeconds, 60u);
}

TEST_F(StoreTest, GarbageFileStartsFreshOnAppend) {
  fake::add("/.crosspoint/reading_stats.bin", "not a stats file at all");
  auto& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.addSession("/a.epub", NOW - 60, 120, 0, 100));

  ReadingStatsSummary s;
  ASSERT_TRUE(store.loadSummary(NOW, TZ, s));
  EXPECT_EQ(s.sessions, 1u);
}
