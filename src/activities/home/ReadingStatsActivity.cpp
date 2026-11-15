#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "components/UITheme.h"
#include "util/ReadingStats.h"

namespace fui = freeink::ui;

namespace {

// Language-neutral "H:MM:SS" — hours are not re-based per day on purpose, so
// 27:05:33 reads as twenty-seven hours of reading. Seconds matter: without
// them a short session displays as 0:00 and looks like time is not counted.
void formatDuration(const uint64_t seconds, char* buf, const size_t bufSize) {
  snprintf(buf, bufSize, "%u:%02u:%02u", static_cast<unsigned>(seconds / 3600),
           static_cast<unsigned>((seconds % 3600) / 60), static_cast<unsigned>(seconds % 60));
}

// Local day label "dd.mm" from a local day index (days since the epoch).
void formatDay(const int32_t dayIndex, const int32_t tzOffsetSec, char* buf, const size_t bufSize) {
  const time_t dayStartUtc = static_cast<time_t>(dayIndex) * 86400 - tzOffsetSec;
  struct tm tm {};
  gmtime_r(&dayStartUtc, &tm);
  snprintf(buf, bufSize, "%02d.%02d", tm.tm_mday, tm.tm_mon + 1);
}

// UTC offset of the active timezone (TZ is pushed into newlib at boot);
// 0 when wall-clock time is not trusted yet.
int32_t tzOffsetSec(const int64_t nowEpoch) {
  if (nowEpoch <= 0) return 0;
  const time_t now = static_cast<time_t>(nowEpoch);
  struct tm local {};
  struct tm utc {};
  localtime_r(&now, &local);
  gmtime_r(&now, &utc);
  // mktime reads the struct as local time, so utc-as-local lands offset ahead.
  return static_cast<int32_t>(mktime(&utc) - mktime(&local));
}

}  // namespace

const char* ReadingStatsActivity::headerTitle() const { return tr(STR_READING_STATS); }

void ReadingStatsActivity::onEnter() {
  UiListActivity::onEnter();

  const int64_t now = trustedtime::trustedNow();
  ReadingStatsSummary summary;
  READING_STATS.loadSummary(now, tzOffsetSec(now), summary);
  if (summary.sessions == 0) {
    requestUpdate();  // buildScreen shows the empty-state text
    return;
  }
  const int32_t tz = tzOffsetSec(now);

  char value[24];
  const auto addSummaryRow = [this, &value](const char* label) {
    rows.push_back({label, value, {}});
  };

  snprintf(value, sizeof(value), "%u", summary.distinctBooks);
  addSummaryRow(tr(STR_STATS_BOOKS_READ));
  snprintf(value, sizeof(value), "%u", summary.sessions);
  addSummaryRow(tr(STR_STATS_SESSIONS));
  formatDuration(summary.totalSeconds, value, sizeof(value));
  addSummaryRow(tr(STR_STATS_TOTAL_TIME));
  formatDuration(summary.todaySeconds, value, sizeof(value));
  addSummaryRow(tr(STR_STATS_TODAY));
  formatDuration(summary.weekSeconds, value, sizeof(value));
  addSummaryRow(tr(STR_STATS_LAST_7_DAYS));

  bool firstDay = true;
  for (const auto& day : summary.days) {
    char label[8];
    formatDay(day.dayIndex, tz, label, sizeof(label));
    if (day.seconds > 0) {
      formatDuration(day.seconds, value, sizeof(value));
    } else {
      value[0] = '-';
      value[1] = '\0';
    }
    rows.push_back({label, value, firstDay ? tr(STR_STATS_BY_DAY) : ""});
    firstDay = false;
  }

  // Book titles come from the library index when it is already built; the
  // store keeps only paths, and rebuilding the index here would freeze the
  // screen. Books missing from the index (deleted, library never built) fall
  // back to the path basename. One chunked pass over the index resolves all
  // rows at once, so the cost is bounded by the library, not by the top list.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto libReader = makeUniqueNoThrow<LibraryReader>();
  if (libReader && !libReader->index.open(library::libraryIndexPath())) {
    libReader.reset();
  }
  const size_t lookupCount = std::min(summary.topBooks.size(), library::LibraryIndexFile::MAX_IDENTITY_LOOKUPS);
  uint16_t ascRows[library::LibraryIndexFile::MAX_IDENTITY_LOOKUPS] = {};
  if (libReader && lookupCount > 0) {
    library::BookIdentity identities[library::LibraryIndexFile::MAX_IDENTITY_LOOKUPS];
    for (size_t i = 0; i < lookupCount; i++) {
      const std::string& path = summary.topBooks[i].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("STATS", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!libReader->index.recentRowsFor(identities, lookupCount, ascRows)) {
      LOG_ERR("STATS", "library title lookup failed; falling back to file names");
      libReader.reset();
    }
  }

  bool firstBook = true;
  for (size_t i = 0; i < summary.topBooks.size(); i++) {
    const auto& book = summary.topBooks[i];
    std::string name;
    if (libReader && i < lookupCount && ascRows[i] != 0xFFFF) {
      const uint16_t ordinal = libReader->index.ordinalForRow(library::SortOrder::RecentAsc, ascRows[i]);
      if (ordinal != 0xFFFF && libReader->index.readRecord(ordinal, libReader->record) &&
          (!libReader->index.readTitle(libReader->record, name) || name.empty())) {
        libReader->index.readName(libReader->record, name);
      }
    }
    if (name.empty()) {
      // Basename without extension, exactly as the book sits on the card.
      const auto slash = book.path.find_last_of('/');
      name = book.path.substr(slash == std::string::npos ? 0 : slash + 1);
      const auto dot = name.rfind('.');
      if (dot != std::string::npos && dot > 0) name.resize(dot);
    }
    formatDuration(book.seconds, value, sizeof(value));
    rows.push_back({std::move(name), value, firstBook ? tr(STR_STATS_TOP_BOOKS) : ""});
    firstBook = false;
  }

  requestUpdate();
}

void ReadingStatsActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  const auto* self = static_cast<const ReadingStatsActivity*>(ctx);
  if (index >= self->rows.size()) return;
  const Row& row = self->rows[index];
  item.label = row.label.c_str();
  if (!row.value.empty()) item.value = row.value.c_str();
  if (!row.heading.empty()) item.sectionHeading = row.heading.c_str();
}

void ReadingStatsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the header band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (rows.empty()) {
    screen.centeredText(tr(STR_STATS_NO_DATA), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.rowProvider = &ReadingStatsActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(rows.size());
  // Read-only rows: no action registered, so taps do nothing; the default
  // input mask keeps swipe scrolling.
  props.valueInset = 8;
  fui::TextStyle label = screen.theme().smallText;
  label.maxLines = 2;  // book names can wrap; shared list layout sizes the row
  props.labelText = label;
  // Values here are short durations: skip the balanced 60%-band wrap cap.
  props.balanceWrappedLabelWithValue = false;
  syncListViewport(screen, props);
  screen.list(props);
}
