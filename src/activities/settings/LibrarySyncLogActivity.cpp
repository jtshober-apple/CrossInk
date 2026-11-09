#include "LibrarySyncLogActivity.h"

#include <GfxRenderer.h>

#include <cstdio>
#include <cstdlib>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "network/LibrarySync.h"
#include "util/LibrarySyncCore.h"

namespace fui = freeink::ui;

LibrarySyncLogActivity::LibrarySyncLogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("LibrarySyncLog", renderer, mappedInput) {}

void LibrarySyncLogActivity::onEnter() {
  UiListActivity::onEnter();

  // Newest line first. Each line: stamp \t reason \t file \t attempt \t size (see LibrarySyncCore::formatErrorLine).
  const std::string log = LibrarySync::readErrorLog();
  std::vector<std::string_view> lines;
  size_t pos = 0;
  while (pos < log.size()) {
    size_t end = log.find('\n', pos);
    if (end == std::string::npos) end = log.size();
    if (end > pos) lines.emplace_back(std::string_view(log).substr(pos, end - pos));
    pos = end + 1;
  }
  entries_.clear();
  entries_.reserve(lines.size());
  for (size_t i = lines.size(); i-- > 0;) {
    std::string_view f[5];
    if (LibrarySyncCore::splitTabs(lines[i], f, 5) < 3) continue;
    Entry e;
    e.stamp = std::string(f[0]);
    e.reason = std::string(f[1]);
    e.name = std::string(f[2]);
    if (f[3].size()) e.attempts = static_cast<unsigned>(LibrarySyncCore::parseU64(f[3]));
    if (f[4].size()) e.size = static_cast<unsigned long>(LibrarySyncCore::parseU64(f[4]));
    entries_.push_back(std::move(e));
  }

  subtitles_.clear();
  subtitles_.reserve(entries_.size());
  rowItems_.clear();
  rowItems_.reserve(entries_.empty() ? 1 : entries_.size());
  if (entries_.empty()) {
    fui::ListItem none;
    none.label = "No problem books recorded";
    none.actionValue = -1;
    rowItems_.push_back(none);
  }
  for (size_t i = 0; i < entries_.size(); ++i) {
    subtitles_.push_back(entries_[i].stamp + "  " + entries_[i].reason);
    fui::ListItem item;
    item.label = entries_[i].name.c_str();
    item.subtitle = subtitles_.back().c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
  nav.reset(0);

  LibrarySync::markErrorsRead();
}

int LibrarySyncLogActivity::listCount() const { return static_cast<int>(rowItems_.size()); }

const char* LibrarySyncLogActivity::headerTitle() const { return "Library Sync problems"; }

void LibrarySyncLogActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index < 0 || index >= static_cast<int>(entries_.size())) return;
  const Entry& e = entries_[static_cast<size_t>(index)];
  char size[40] = "";
  if (e.size > 0) std::snprintf(size, sizeof(size), "%.1f MB", static_cast<double>(e.size) / 1e6);
  std::string body = e.name + "\n" + e.stamp;
  if (e.attempts > 0) body += "\nAttempt " + std::to_string(e.attempts);
  if (size[0]) body += "\n" + std::string(size);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, e.reason, body, "Close", "OK"),
                         [](const ActivityResult&) {});
}

void LibrarySyncLogActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
