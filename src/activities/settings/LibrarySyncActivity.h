#pragma once

#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "network/LibrarySync.h"
#include "util/ButtonNavigator.h"

/**
 * Settings → Library Sync. In menu mode it shows the sync options and the state of the library on this reader.
 * "Sync library now" restarts into a clean network boot (like the OPDS browser, so the heap is fresh for TLS) and
 * comes back here in run mode, which joins Wi-Fi, runs the whole sync with a progress screen, and returns home.
 */
class LibrarySyncActivity final : public Activity {
 public:
  explicit LibrarySyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool runNow = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return state_ == State::RUNNING || state_ == State::CONNECTING; }
  bool skipLoopDelay() override { return state_ == State::RUNNING; }

 private:
  enum class State { MENU, CONNECTING, RUNNING, DONE };

  const bool runNow_;
  State state_ = State::MENU;
  volatile bool drawn_ = false;
  int selected_ = 0;
  ButtonNavigator buttonNavigator_;
  ScreenTransitionRefresh screenTransitionRefresh_;

  LibrarySync::Progress progress_;
  LibrarySync::Result result_ = LibrarySync::Result::OK;
  bool cancelRequested_ = false;

  void setState(State state);
  void loopMenu();
  void activateRow();
  void startRun();
  void runSync();
  void renderMenu() const;
  void renderRunning() const;
  void renderDone() const;
  void exitViaBack();
  std::string lastCheckText() const;
};
