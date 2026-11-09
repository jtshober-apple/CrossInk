#pragma once

#include <cstddef>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

/**
 * Shown once per start-up, on top of Home, when Library Sync has logged problem books the user has not seen:
 * "N new problem books" with View log / Dismiss. Either choice counts the lines as seen (they stay in the log).
 */
class LibrarySyncNoticeActivity final : public Activity {
 public:
  LibrarySyncNoticeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t unread);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  size_t unread_;
  OptionPopup popup_;
};
