#include <gtest/gtest.h>

#include "util/WifiJoinCore.h"

using namespace WifiJoinCore;

namespace {
std::vector<std::string> names(const std::vector<Candidate>& list) {
  std::vector<std::string> out;
  for (const auto& c : list) out.push_back(c.ssid);
  return out;
}
}  // namespace

TEST(WifiJoinCore, LastConnectedFirstWhenVisibleThenStrongest) {
  const std::vector<std::string> saved{"Home", "Phone", "Work"};
  const std::vector<Scan> scans{{"Work", -40}, {"Phone", -55}, {"Home", -70}, {"Stranger", -30}};
  EXPECT_EQ(names(order(saved, scans, "Home")), (std::vector<std::string>{"Home", "Work", "Phone"}));
  EXPECT_EQ(names(order(saved, scans, "Phone")), (std::vector<std::string>{"Phone", "Work", "Home"}));
  EXPECT_EQ(names(order(saved, scans, "")), (std::vector<std::string>{"Work", "Phone", "Home"}));  // strongest first
}

TEST(WifiJoinCore, UnseenSavedNetworksAreSkipped) {
  const std::vector<std::string> saved{"Home", "Phone", "Work"};
  const std::vector<Scan> scans{{"Phone", -55}};
  const auto result = order(saved, scans, "Home");  // last-connected is out of range: do not waste time on it
  EXPECT_EQ(names(result), (std::vector<std::string>{"Phone"}));
  EXPECT_TRUE(result[0].visible);
}

TEST(WifiJoinCore, BlindFallbackWhenNothingIsSeen) {
  const std::vector<std::string> saved{"Home", "Hidden"};
  const std::vector<Scan> none;
  const auto last = order(saved, none, "Hidden");
  ASSERT_EQ(last.size(), 1u);
  EXPECT_EQ(last[0].ssid, "Hidden");
  EXPECT_FALSE(last[0].visible);
  EXPECT_EQ(order(saved, none, "")[0].ssid, "Home");  // no last-connected: the first saved network
  EXPECT_TRUE(order({}, none, "").empty());
}

TEST(WifiJoinCore, DuplicateScanEntriesUseTheStrongest) {
  const std::vector<std::string> saved{"Mesh", "Other"};
  const std::vector<Scan> scans{{"Mesh", -80}, {"Other", -60}, {"Mesh", -45}};
  EXPECT_EQ(names(order(saved, scans, "")), (std::vector<std::string>{"Mesh", "Other"}));
}

TEST(WifiJoinCore, OnlyAndPreferredNetworks) {
  const std::vector<std::string> saved{"Home", "Phone"};
  const std::vector<Scan> scans{{"Phone", -40}, {"Home", -75}};
  // "home network only": nothing but the marked network may be tried
  EXPECT_EQ(names(order(saved, scans, "Phone", "Home")), (std::vector<std::string>{"Home"}));
  EXPECT_TRUE(order(saved, std::vector<Scan>{{"Phone", -40}}, "Phone", "Home").size() == 1u);  // blind try of Home only
  EXPECT_EQ(order(saved, std::vector<Scan>{{"Phone", -40}}, "Phone", "Home")[0].ssid, "Home");
  // a preferred network goes ahead of the last-connected one, but only when it is in range
  EXPECT_EQ(names(order(saved, scans, "Phone", "", "Home")), (std::vector<std::string>{"Home", "Phone"}));
  EXPECT_EQ(names(order(saved, std::vector<Scan>{{"Phone", -40}}, "Phone", "", "Home")),
            (std::vector<std::string>{"Phone"}));
}

TEST(WifiJoinCore, AttemptTimeoutsStayInsideTheBudget) {
  // 25 s budget, three networks in range: 12 + 6 + 6 never exceeds what is left
  uint32_t left = 25000 - 2000;  // after the scan
  uint32_t spent = 0;
  for (size_t i = 0; i < 3; ++i) {
    const uint32_t t = attemptTimeoutMs(i, 3, left);
    EXPECT_GT(t, 0u);
    spent += t;
    left -= t;
  }
  EXPECT_LE(spent + 2000, 25000u);
  EXPECT_EQ(attemptTimeoutMs(0, 1, 25000), 18000u);  // a single candidate gets longer
  EXPECT_EQ(attemptTimeoutMs(0, 3, 5000), 5000u);    // capped by what remains
  EXPECT_EQ(attemptTimeoutMs(2, 3, 2000), 0u);       // too little left: skip
}
