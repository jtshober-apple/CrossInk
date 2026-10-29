#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

/**
 * TesseraeActivity handles the full Tesserae dashboard pairing and sync flow:
 *
 *   WIFI_SELECTION  → user picks a network (WifiSelectionActivity sub-flow)
 *   DISCOVERING     → POST /api/v1/device/discover
 *   AWAITING_APPROVAL → admin must approve the device in the Tesserae web UI
 *   FETCHING_FRAME  → GET /api/v1/device/<id>/frame.bmp
 *   DONE            → BMP saved; activity finishes with a success message
 *   ERROR           → shows a user-facing error and offers retry or back
 *
 * The activity is accessible from Settings → Tesserae Dashboard.
 * On completion the cached frame path is /.crosspoint/tesserae_frame.bmp.
 */
class TesseraeActivity final : public Activity {
 public:
  enum class State {
    WIFI_SELECTION,      // waiting for WifiSelectionActivity to return
    DISCOVERING,         // POSTing /discover
    AWAITING_APPROVAL,   // 202 — waiting for admin; user can retry
    FETCHING_FRAME,      // GETting frame.bmp
    DONE,                // frame saved; show success and finish
    ERROR,               // unrecoverable; user can retry or go back
  };

  explicit TesseraeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override {
    return state_ == State::DISCOVERING || state_ == State::FETCHING_FRAME;
  }

  // Called by ActivityManager when WifiSelectionActivity returns.
  void onActivityResult(int resultCode, void* data) override;

 private:
  State state_ = State::WIFI_SELECTION;
  std::string statusMessage_;
  std::string errorMessage_;
  ScreenTransitionRefresh screenTransitionRefresh_;

  // Timestamps for AWAITING_APPROVAL retry loop (ms).
  uint32_t approvalPollMs_ = 0;
  static constexpr uint32_t APPROVAL_POLL_INTERVAL_MS = 5000;

  void setState(State state);
  void runDiscover();
  void runFetchFrame();
  void exitViaBack();
  void renderStatus(const char* heading, const char* body, bool showBack, bool showRetry) const;
};
