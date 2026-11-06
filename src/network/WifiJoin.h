#pragma once

#include <cstdint>
#include <functional>
#include <string>

// One shared, non-interactive Wi-Fi join used by the timed-wake dashboard refresh, the sleep refresh and Library Sync.
// Self-contained: needs only WIFI_STORE (the saved networks) and the Arduino WiFi stack.
namespace WifiJoin {

struct Options {
  uint32_t totalMs = 25000;     // hard cap for the whole join, scan included
  bool scan = true;             // false: legacy single try of the last-connected (else first) saved network
  std::string onlySsid;         // when set, only this network may be joined
  std::string preferSsid;       // when set and visible, tried first
  std::function<bool()> abort;  // polled while waiting; true cancels the join
  const char* tag = "WIFI";     // log tag
};

// Scans once, then tries the saved networks last-connected-first (when visible) and the others strongest-first, each
// with a short timeout, never exceeding totalMs. Returns true once connected (the radio is left on).
bool joinBest(const Options& options);

}  // namespace WifiJoin
