#include "LibrarySyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#ifndef SIMULATOR
#include <WiFi.h>
#endif

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiJoin.h"
#include "util/RtcMinutes.h"

namespace {
constexpr const char* TAG = "LIBSYNC_ACT";

enum Row : int {
  ROW_SYNC_NOW = 0,
  ROW_SCHEDULE,
  ROW_WIFI,
  ROW_BATTERY,
  ROW_UPDATES,
  ROW_SERVER,
  ROW_COUNT,
};

constexpr uint8_t BATTERY_STEPS[] = {20, 30, 40, 50, 60};

// Menu row geometry, shared by the renderer and the touch hit-test so a tap always lands on the row that is drawn.
struct MenuGeometry {
  int left, width, rowH, top, step;
};
MenuGeometry menuGeometry(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  MenuGeometry g{};
  g.left = metrics.contentSidePadding;
  g.width = renderer.getScreenWidth() - g.left * 2;
  g.rowH = renderer.getLineHeight(UI_10_FONT_ID) * 2 + 14;
  g.top = metrics.topPadding + metrics.headerHeight + 10;
  g.step = g.rowH + 4;
  return g;
}

// On-screen Cancel button for touch boards (they draw no button hints).
Rect cancelButtonRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  return Rect{metrics.contentSidePadding, renderer.getScreenHeight() - 110, w, 64};
}

bool inRect(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

const char* scheduleLabel(const LibrarySync::Schedule schedule) {
  switch (schedule) {
    case LibrarySync::Schedule::MANUAL:
      return "Off (manual only)";
    case LibrarySync::Schedule::EVERY_WAKE:
      return "Every background wake";
    case LibrarySync::Schedule::TWICE_DAILY:
      return "Twice daily";
    case LibrarySync::Schedule::DAILY:
      return "Daily";
    case LibrarySync::Schedule::CHARGING:
      return "Only while charging";
  }
  return "";
}

void cycleBattery(uint8_t& floor) {
  for (size_t i = 0; i < sizeof(BATTERY_STEPS); ++i) {
    if (BATTERY_STEPS[i] > floor) {
      floor = BATTERY_STEPS[i];
      return;
    }
  }
  floor = BATTERY_STEPS[0];
}
}  // namespace

LibrarySyncActivity::LibrarySyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool runNow)
    : Activity("LibrarySync", renderer, mappedInput), runNow_(runNow) {}

void LibrarySyncActivity::onEnter() {
  Activity::onEnter();
  LibrarySync::loadConfig();
  selected_ = 0;
  cancelRequested_ = false;
  progress_ = LibrarySync::Progress();
  if (!runNow_) {
    setState(State::MENU);
    return;
  }
  beginRun();
}

void LibrarySyncActivity::beginRun() {
  setState(State::CONNECTING);
#ifndef SIMULATOR
  if (WiFi.status() == WL_CONNECTED) {
    startRun();
    return;
  }
  {
    // Try the saved networks without asking (scan once, strongest first); only fall back to the picker if none joins.
    WifiJoin::Options join;
    join.tag = "LIBSYNC";
    join.totalMs = 25000;
    if (WifiJoin::joinBest(join)) {
      startRun();
      return;
    }
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             exitViaBack();
                             return;
                           }
                           startRun();
                         });
#else
  startRun();
#endif
}

void LibrarySyncActivity::onExit() {
  Activity::onExit();
#ifndef SIMULATOR
  // Hand the radio back when a sync ran from here.
  if (ranSync_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
#endif
}

void LibrarySyncActivity::setState(const State state) {
  state_ = state;
  drawn_ = false;
  requestUpdate();
}

void LibrarySyncActivity::exitViaBack() { finish(); }

void LibrarySyncActivity::startRun() {
  if (!OPDS_STORE.hasServers()) {
    result_ = LibrarySync::Result::NO_SERVER;
    setState(State::DONE);
    return;
  }
  setState(State::RUNNING);
}

void LibrarySyncActivity::loop() {
  using MIB = MappedInputManager::Button;
  switch (state_) {
    case State::MENU:
      loopMenu();
      break;
    case State::CONNECTING:
      break;  // WifiSelectionActivity is running
    case State::RUNNING:
      if (!drawn_) break;  // let the first progress screen reach the panel before the blocking work
      runSync();
      break;
    case State::DONE:
      if (mappedInput.wasPressed(MIB::Back) || mappedInput.wasPressed(MIB::Confirm)) {
        exitViaBack();
      } else if (mappedInput.hasTouch()) {
        int tx = 0;
        int ty = 0;
        if (mappedInput.wasScreenTapped(tx, ty) || mappedInput.wasBackGesture()) exitViaBack();
      }
      break;
  }
}

void LibrarySyncActivity::loopMenu() {
  using MIB = MappedInputManager::Button;
  if (mappedInput.wasPressed(MIB::Back)) {
    exitViaBack();
    return;
  }
  if (mappedInput.wasReleased(MIB::Confirm)) {
    activateRow();
    return;
  }
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      exitViaBack();
      return;
    }
    const auto g = menuGeometry(renderer);
    int row = 0;
    const auto touch = mappedInput.rowTouch(row, g.top, g.step, ROW_COUNT, 0, INT32_MAX, g.rowH);
    if (touch == MappedInputManager::RowTouch::Down && row != selected_) {
      selected_ = row;
      requestUpdate();
    } else if (touch == MappedInputManager::RowTouch::Tap) {
      selected_ = row;
      activateRow();
      return;
    }
  }
  buttonNavigator_.onNext([this] {
    selected_ = ButtonNavigator::nextIndex(selected_, ROW_COUNT);
    requestUpdate();
  });
  buttonNavigator_.onPrevious([this] {
    selected_ = ButtonNavigator::previousIndex(selected_, ROW_COUNT);
    requestUpdate();
  });
}

void LibrarySyncActivity::activateRow() {
  auto& cfg = LibrarySync::config();
  switch (selected_) {
    case ROW_SYNC_NOW:
      if (!OPDS_STORE.hasServers()) {
        result_ = LibrarySync::Result::NO_SERVER;
        setState(State::DONE);
        return;
      }
      beginRun();
      return;
    case ROW_SCHEDULE:
      cfg.schedule = LibrarySyncCore::nextSchedule(cfg.schedule);
      break;
    case ROW_WIFI:
      cfg.homeOnly = !cfg.homeOnly;
      if (!cfg.homeOnly) cfg.homeSsid[0] = '\0';  // the next manual sync records the network again
      break;
    case ROW_BATTERY:
      cycleBattery(cfg.batteryFloor);
      break;
    case ROW_UPDATES:
      cfg.replaceUpdated = !cfg.replaceUpdated;
      break;
    case ROW_SERVER: {
      const size_t count = OPDS_STORE.getCount();
      if (count > 0) cfg.serverIndex = static_cast<uint8_t>((cfg.serverIndex + 1) % count);
      break;
    }
    default:
      break;
  }
  LibrarySync::saveConfig();
  requestUpdate();
}

void LibrarySyncActivity::runSync() {
  ranSync_ = true;
  cancelRequested_ = false;
  const auto abort = [this] {
    if (cancelRequested_) return true;
    mappedInput.update();
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelRequested_ = true;
    }
    if (mappedInput.hasTouch()) {
      int tx = 0;
      int ty = 0;
      if (mappedInput.wasBackGesture() ||
          (mappedInput.wasScreenTapped(tx, ty) && inRect(cancelButtonRect(renderer), tx, ty))) {
        cancelRequested_ = true;
      }
    }
    return cancelRequested_;
  };
  unsigned long lastDraw = 0;
  const auto progress = [this, &lastDraw](const LibrarySync::Progress& p) {
    progress_ = p;
    const unsigned long now = millis();
    if (now - lastDraw >= 1500) {
      lastDraw = now;
      requestUpdate(true);
    }
  };
  result_ = LibrarySync::run(/*manual=*/true, abort, progress, progress_);
  LOG_INF(TAG, "manual sync: %s fetched=%u failed=%u missing=%u", LibrarySync::resultString(result_),
          static_cast<unsigned>(progress_.fetched), static_cast<unsigned>(progress_.failed),
          static_cast<unsigned>(progress_.missing));
  setState(State::DONE);
}

std::string LibrarySyncActivity::lastCheckText() const {
  const auto& cfg = LibrarySync::config();
  if (cfg.lastCheckMin <= 0) return "never checked";
  int64_t now = 0;
  if (!rtcMinutesSinceEpoch(now) || now < cfg.lastCheckMin) return "checked";
  const int64_t ago = now - cfg.lastCheckMin;
  char buf[40];
  if (ago < 90) {
    std::snprintf(buf, sizeof(buf), "checked %lld min ago", static_cast<long long>(ago));
  } else if (ago < 48 * 60) {
    std::snprintf(buf, sizeof(buf), "checked %lld h ago", static_cast<long long>(ago / 60));
  } else {
    std::snprintf(buf, sizeof(buf), "checked %lld days ago", static_cast<long long>(ago / (24 * 60)));
  }
  return buf;
}

// ---- rendering --------------------------------------------------------------------------------------------------

void LibrarySyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LIBRARY_SYNC));
  drawn_ = true;

  switch (state_) {
    case State::MENU:
      renderMenu();
      break;
    case State::CONNECTING:
      break;
    case State::RUNNING:
      renderRunning();
      break;
    case State::DONE:
      renderDone();
      break;
  }
  renderer.displayBuffer();
}

void LibrarySyncActivity::renderMenu() const {
  const auto g = menuGeometry(renderer);
  const int left = g.left;
  const int width = g.width;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowH = g.rowH;
  int y = g.top;

  const auto& cfg = LibrarySync::config();
  const auto server = LibrarySync::selectedServer();

  char summary[80];
  if (cfg.lastTotal > 0) {
    std::snprintf(summary, sizeof(summary), "%u of %u books on this reader, %s", static_cast<unsigned>(cfg.lastSynced),
                  static_cast<unsigned>(cfg.lastTotal), lastCheckText().c_str());
  } else {
    std::snprintf(summary, sizeof(summary), "Not synced yet");
  }

  char battery[16];
  std::snprintf(battery, sizeof(battery), "%u%%", static_cast<unsigned>(cfg.batteryFloor));
  std::string wifi = "Any saved network";
  if (cfg.homeOnly) wifi = cfg.homeSsid[0] ? std::string("Only ") + cfg.homeSsid : "Home network (set by next sync)";

  struct Line {
    const char* label;
    std::string value;
  };
  const Line rows[ROW_COUNT] = {
      {"Sync library now", summary},
      {"Automatic sync", scheduleLabel(cfg.schedule)},
      {"Wi-Fi for automatic sync", wifi},
      {"Skip below battery", battery},
      {"Replace changed books", cfg.replaceUpdated ? "On" : "Off"},
      {"Server", server.found ? server.name : "None saved: add one in OPDS Servers"},
  };
  for (int i = 0; i < ROW_COUNT; ++i) {
    const bool sel = i == selected_;
    if (sel) renderer.fillRect(left - 6, y, width + 12, rowH, true);
    renderer.drawText(UI_10_FONT_ID, left, y + 5, rows[i].label, !sel, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, left, y + 5 + lineH + 2, rows[i].value.c_str(), !sel);
    y += g.step;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Change", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LibrarySyncActivity::renderRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int left = metrics.contentSidePadding;
  const int width = pageWidth - left * 2;
  int y = pageHeight / 2 - 90;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "Syncing library", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 12;

  char line[80];
  if (progress_.total > 0) {
    std::snprintf(line, sizeof(line), "%u of %u books on this reader", static_cast<unsigned>(progress_.synced),
                  static_cast<unsigned>(progress_.total));
  } else {
    std::snprintf(line, sizeof(line), "Reading the catalog...");
  }
  renderer.drawCenteredText(SMALL_FONT_ID, y, line, true, EpdFontFamily::REGULAR);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 14;

  if (!progress_.title.empty()) {
    UITheme::drawCenteredWrappedText(renderer, Rect{left, y, width, renderer.getLineHeight(SMALL_FONT_ID) * 3},
                                     SMALL_FONT_ID, progress_.title.c_str(), 3, true, EpdFontFamily::REGULAR,
                                     UITheme::TextVerticalAlignment::TOP);
    y += renderer.getLineHeight(SMALL_FONT_ID) * 3 + 10;
    std::snprintf(line, sizeof(line), "Fetching %u of %u this batch", static_cast<unsigned>(progress_.position),
                  static_cast<unsigned>(progress_.batch));
    renderer.drawCenteredText(SMALL_FONT_ID, y, line, true, EpdFontFamily::REGULAR);
    y += renderer.getLineHeight(SMALL_FONT_ID) + 10;
    if (progress_.bytesTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{left, y, width, 16}, progress_.bytes, progress_.bytesTotal);
    }
  }

  if (mappedInput.hasTouch()) {
    const Rect b = cancelButtonRect(renderer);
    renderer.drawRect(b.x, b.y, b.width, b.height, 2, true);
    renderer.drawCenteredText(UI_10_FONT_ID, b.y + (b.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                              "Tap to cancel", true, EpdFontFamily::BOLD);
  }
  const auto labels = mappedInput.mapLabels("Cancel", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LibrarySyncActivity::renderDone() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const Rect area{metrics.contentSidePadding, 0, pageWidth - metrics.contentSidePadding * 2, pageHeight};
  int y = pageHeight / 2 - 70;

  renderer.drawCenteredText(UI_10_FONT_ID, y, LibrarySync::resultString(result_), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 14;

  char line[96];
  if (progress_.total > 0) {
    std::snprintf(line, sizeof(line), "%u of %u books on this reader", static_cast<unsigned>(progress_.synced),
                  static_cast<unsigned>(progress_.total));
    renderer.drawCenteredText(SMALL_FONT_ID, y, line, true, EpdFontFamily::REGULAR);
    y += renderer.getLineHeight(SMALL_FONT_ID) + 6;
    std::snprintf(line, sizeof(line), "Fetched %u, failed %u, still to fetch %u",
                  static_cast<unsigned>(progress_.fetched), static_cast<unsigned>(progress_.failed),
                  static_cast<unsigned>(progress_.missing));
    UITheme::drawCenteredWrappedText(renderer, Rect{area.x, y, area.width, renderer.getLineHeight(SMALL_FONT_ID) * 2},
                                     SMALL_FONT_ID, line, 2, true, EpdFontFamily::REGULAR,
                                     UITheme::TextVerticalAlignment::TOP);
  }

  if (mappedInput.hasTouch()) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 90, "Tap anywhere to close", true, EpdFontFamily::REGULAR);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
