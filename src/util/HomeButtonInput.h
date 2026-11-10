#pragma once

#include <cstdint>

// Persisted indices: append actions without reordering existing values.
enum class HomeButtonAction : uint8_t {
  Home,
  Ignore,
  NextPage,
  Refresh,
  Footnotes,
  Confirm,
  Sync,
  Bookmark,
  Dictionary,
  ReaderMenu,
  ToggleFrontlight,
  CreateClipping,
  // Global actions (handled in main.cpp wherever you are):
  Library,
  Settings,
  Sleep,
  SyncLibrary,
  SyncDashboard,
  FileTransfer,
  JoinNetwork,
  CreateHotspot,
  Count
};

// Tap counting for the Home key (and the Power button): one, two or three taps in quick succession, or a long press.
// A gesture with nothing assigned to the longer gestures fires the moment it is complete, so the common single tap
// costs no extra delay unless you actually assigned a double or triple tap.
class HomeButtonInput {
 public:
  static constexpr uint32_t DOUBLE_TAP_MS = 350;

  HomeButtonAction update(uint32_t now, bool tapped, bool held, bool swiped, bool pressed, HomeButtonAction tap,
                          HomeButtonAction doubleTap, HomeButtonAction tripleTap, HomeButtonAction longPress) {
    if (swiped) {
      reset();
      return HomeButtonAction::Ignore;
    }
    if (held) {
      reset();
      return longPress;
    }
    const HomeButtonAction actions[3] = {tap, doubleTap, tripleTap};
    HomeButtonAction out = HomeButtonAction::Ignore;
    if (count > 0 && !secondContact &&
        now - tappedAt > DOUBLE_TAP_MS) {  // the window closed: the taps so far are the gesture
      out = actions[count - 1];
      reset();
    } else if (count > 0 && pressed) {
      secondContact = true;  // the next tap has started: do not time out under it
    }
    if (!tapped) return out;
    if (out != HomeButtonAction::Ignore) {  // a late tap right behind a timed-out gesture starts a new one
      count = 1;
      tappedAt = now;
      secondContact = false;
      return out;
    }
    ++count;
    secondContact = false;
    tappedAt = now;
    const bool longerAssigned =
        (count == 1 && (doubleTap != HomeButtonAction::Ignore || tripleTap != HomeButtonAction::Ignore)) ||
        (count == 2 && tripleTap != HomeButtonAction::Ignore);
    if (longerAssigned) return HomeButtonAction::Ignore;  // wait: it may become a double or triple
    const HomeButtonAction result = actions[count - 1];
    reset();
    return result;
  }

  void reset() {
    count = 0;
    secondContact = false;
  }

 private:
  uint32_t tappedAt = 0;
  uint8_t count = 0;
  bool secondContact = false;
};
