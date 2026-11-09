#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

/**
 * Library Sync problems: one row per line of the error log (newest first), each naming the epub that could not be
 * fetched and why. Opening it counts every line as seen. Tap (or Confirm) a row for the full details.
 */
class LibrarySyncLogActivity final : public UiListActivity {
 public:
  explicit LibrarySyncLogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  struct Entry {
    std::string stamp;
    std::string reason;
    std::string name;
    unsigned attempts = 0;
    unsigned long size = 0;
  };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  std::vector<Entry> entries_;
  std::vector<std::string> subtitles_;
  std::vector<freeink::ui::ListItem> rowItems_;
};
