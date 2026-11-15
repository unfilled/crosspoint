#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Read-only reading-statistics screen: summary rows (books/sessions/time,
// today, week), per-day rows for the last 14 local days, and the top books
// by total time. Data comes from ReadingStatsStore; rows are materialized
// once on enter (bounded: 5 summary + 14 days + 10 books), so the row
// provider just points into the vector.
class ReadingStatsActivity final : public UiListActivity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;

 private:
  struct Row {
    std::string label;
    std::string value;
    std::string heading;  // section heading carried by the section's first row
  };

  std::vector<Row> rows;

  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}  // informational rows only
  const char* headerTitle() const override;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
};
