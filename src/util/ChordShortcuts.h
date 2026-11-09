#pragma once

#include <cstdint>

// Shortcuts for two buttons pressed together (Up+Down, Left+Right, Back+Confirm), assignable in Settings -> Controls.
// Persisted by index: append only.
enum class ChordAction : uint8_t { None, Sleep, Home, Library, Settings, ToggleFrontlight, RefreshScreen, Count };

// Recognises "two buttons pressed together": both are down and their press edges are within TOGETHER_MS of each
// other. One button already held while the other is pressed much later (rolling page turns) is NOT a chord, and a
// chord fires once until both buttons are released again.
class ChordDetector {
 public:
  static constexpr uint32_t TOGETHER_MS = 140;

  // Call every loop pass with the current held state of the pair. Returns true exactly once per chord.
  bool update(const uint32_t now, const bool aDown, const bool bDown) {
    if (!aDown && !bDown) {
      fired_ = false;
      prevA_ = prevB_ = false;
      return false;
    }
    if (aDown && !prevA_) aAt_ = now;
    if (bDown && !prevB_) bAt_ = now;
    prevA_ = aDown;
    prevB_ = bDown;
    if (aDown && bDown && !fired_) {
      fired_ = true;  // decided once per press: a slow roll-over is ignored until both are released
      const uint32_t gap = aAt_ > bAt_ ? aAt_ - bAt_ : bAt_ - aAt_;
      return gap <= TOGETHER_MS;
    }
    return false;
  }

  // True while the pair is held after a chord fired (callers keep swallowing the buttons until release).
  bool active() const { return fired_ && prevA_ && prevB_; }

 private:
  uint32_t aAt_ = 0;
  uint32_t bAt_ = 0;
  bool prevA_ = false;
  bool prevB_ = false;
  bool fired_ = false;
};
