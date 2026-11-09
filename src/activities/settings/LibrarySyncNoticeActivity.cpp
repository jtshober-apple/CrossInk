#include "LibrarySyncNoticeActivity.h"

#include <GfxRenderer.h>

#include <memory>
#include <string>

#include "HalDisplay.h"
#include "activities/ActivityManager.h"
#include "activities/settings/LibrarySyncLogActivity.h"
#include "network/LibrarySync.h"

LibrarySyncNoticeActivity::LibrarySyncNoticeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const size_t unread)
    : Activity("LibrarySyncNotice", renderer, mappedInput), unread_(unread) {}

void LibrarySyncNoticeActivity::onEnter() {
  Activity::onEnter();
  const std::string headline = std::to_string(unread_) + (unread_ == 1 ? " new problem book" : " new problem books");
  const char* options[] = {"View log", "Dismiss"};
  popup_.show("Library Sync", headline.c_str(), options, 2, 0, [this](const int index) {
    LibrarySync::markErrorsRead();  // announced once; the lines stay in the log
    if (index == 0) {
      // The log is a full screen; Back from it returns Home.
      activityManager.replaceActivity(std::make_unique<LibrarySyncLogActivity>(renderer, mappedInput));
    } else {
      finish();
    }
  });
  requestUpdate(true);
}

void LibrarySyncNoticeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (popup_.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void LibrarySyncNoticeActivity::loop() {
  if (popup_.handleInput(mappedInput, [this] {
        if (popup_.isActive()) requestUpdate();
      })) {
    return;
  }
  // Closed without choosing (Back, or a tap outside): same as Dismiss.
  LibrarySync::markErrorsRead();
  finish();
}
