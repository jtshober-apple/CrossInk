#pragma once

// Pure logic behind WifiJoin (no Arduino, no radio): which saved networks to try, in what order, and for how long.
// Kept separate so it can be unit-tested on the host.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace WifiJoinCore {

struct Scan {
  std::string ssid;
  int rssi = -100;
};

struct Candidate {
  std::string ssid;
  int rssi = -100;
  bool visible = false;  // seen in the scan; false only for the single blind fallback attempt
};

// Order the saved networks for a join attempt.
//  - `onlySsid` (when set) restricts the join to that one network.
//  - `preferSsid` (when set and visible) goes first, then the last-connected network (when visible), then the rest of
//    the visible saved networks strongest-first.
//  - Saved networks that were not seen are skipped (they only waste time), except when none was seen at all: then the
//    last-connected (else first saved) network is tried once blind, because a hidden SSID or a failed scan must not
//    make the join worse than it was before scanning existed.
inline std::vector<Candidate> order(const std::vector<std::string>& saved, const std::vector<Scan>& scans,
                                    const std::string& last, const std::string& onlySsid = "",
                                    const std::string& preferSsid = "") {
  std::vector<Candidate> visible;
  for (const auto& s : saved) {
    if (!onlySsid.empty() && s != onlySsid) continue;
    int best = -1000;
    bool seen = false;
    for (const auto& sc : scans) {
      if (sc.ssid == s) {
        seen = true;
        if (sc.rssi > best) best = sc.rssi;
      }
    }
    if (seen) visible.push_back(Candidate{s, best, true});
  }
  // strongest first (stable for equal strength)
  for (size_t i = 1; i < visible.size(); ++i) {
    for (size_t j = i; j > 0 && visible[j].rssi > visible[j - 1].rssi; --j) std::swap(visible[j], visible[j - 1]);
  }
  auto moveFront = [&visible](const std::string& ssid) {
    if (ssid.empty()) return;
    for (size_t i = 0; i < visible.size(); ++i) {
      if (visible[i].ssid == ssid) {
        Candidate c = visible[i];
        visible.erase(visible.begin() + static_cast<std::ptrdiff_t>(i));
        visible.insert(visible.begin(), c);
        return;
      }
    }
  };
  moveFront(last);
  moveFront(preferSsid);  // applied last so it ends up in front of the last-connected network
  if (!visible.empty()) return visible;

  std::string blind;
  for (const auto& s : saved) {
    if (!onlySsid.empty() && s != onlySsid) continue;
    if (blind.empty()) blind = s;  // first allowed saved network
    if (!last.empty() && s == last) {
      blind = s;
      break;
    }
  }
  if (blind.empty()) return {};
  return {Candidate{blind, -100, false}};
}

constexpr uint32_t MIN_ATTEMPT_MS = 2500;  // not worth starting an attempt with less time than this
constexpr uint32_t FIRST_OF_MANY_ATTEMPT_MS = 12000;
constexpr uint32_t LATER_ATTEMPT_MS = 6000;
constexpr uint32_t ONLY_ATTEMPT_MS = 18000;

// How long to wait for network `index` of `count`, given the time left in the overall budget (0 = skip it).
inline uint32_t attemptTimeoutMs(const size_t index, const size_t count, const uint32_t remainingMs) {
  if (remainingMs < MIN_ATTEMPT_MS) return 0;
  const uint32_t want = count <= 1 ? ONLY_ATTEMPT_MS : (index == 0 ? FIRST_OF_MANY_ATTEMPT_MS : LATER_ATTEMPT_MS);
  return want < remainingMs ? want : remainingMs;
}

}  // namespace WifiJoinCore
