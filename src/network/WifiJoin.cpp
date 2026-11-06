#include "network/WifiJoin.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include <vector>

#include "WifiCredentialStore.h"
#include "network/WifiUtils.h"
#include "util/WifiJoinCore.h"

namespace WifiJoin {

namespace {

bool waitConnected(const uint32_t timeoutMs, const std::function<bool()>& abort) {
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (hasActiveStationWifiConnection()) return true;
    if (abort && abort()) return false;
    delay(100);
  }
  return false;
}

}  // namespace

bool joinBest(const Options& options) {
  const char* tag = options.tag;
  const uint32_t startedMs = millis();
  const std::string last = WIFI_STORE.getLastConnectedSsid();

  std::vector<std::string> saved;
  for (size_t i = 0; i < WIFI_STORE.getCredentialCount(); ++i) {
    if (const auto ssid = WIFI_STORE.getSsidAt(i)) saved.push_back(*ssid);
  }
  if (saved.empty()) {
    LOG_DBG(tag, "no saved WiFi network");
    return false;
  }

  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) return false;

  std::vector<WifiJoinCore::Candidate> candidates;
  if (options.scan) {
    WiFi.disconnect(false);
    delay(50);
    std::vector<WifiJoinCore::Scan> scans;
    const int found = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false, /*passive=*/false,
                                        /*max_ms_per_chan=*/120);
    for (int i = 0; i < found; ++i) scans.push_back(WifiJoinCore::Scan{WiFi.SSID(i).c_str(), WiFi.RSSI(i)});
    WiFi.scanDelete();
    LOG_INF(tag, "wifi scan: %d networks in %u ms", found, static_cast<unsigned>(millis() - startedMs));
    candidates = WifiJoinCore::order(saved, scans, last, options.onlySsid, options.preferSsid);
  } else {
    // Legacy path: the last-connected (else first) saved network, one try.
    std::string ssid = last;
    if (ssid.empty() || !WIFI_STORE.findCredential(ssid)) ssid = saved.front();
    if (!options.onlySsid.empty() && ssid != options.onlySsid) return false;
    candidates.push_back(WifiJoinCore::Candidate{ssid, -100, false});
  }

  for (size_t i = 0; i < candidates.size(); ++i) {
    if (options.abort && options.abort()) return false;
    const uint32_t spent = millis() - startedMs;
    const uint32_t remaining = spent >= options.totalMs ? 0 : options.totalMs - spent;
    const uint32_t timeout = options.scan ? WifiJoinCore::attemptTimeoutMs(i, candidates.size(), remaining) : remaining;
    if (timeout == 0) break;
    const auto cred = WIFI_STORE.findCredential(candidates[i].ssid);
    if (!cred) continue;
    if (i > 0) {
      WiFi.disconnect(false);
      delay(100);
    }
    LOG_INF(tag, "wifi join: %s (rssi %d%s) up to %u ms", cred->ssid.c_str(), candidates[i].rssi,
            candidates[i].visible ? "" : ", blind", static_cast<unsigned>(timeout));
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    if (waitConnected(timeout, options.abort)) {
      LOG_INF(tag, "wifi joined %s after %u ms", cred->ssid.c_str(), static_cast<unsigned>(millis() - startedMs));
      return true;
    }
    LOG_ERR(tag, "wifi join timed out: %s", cred->ssid.c_str());
    if (options.abort && options.abort()) return false;
  }
  return false;
}

}  // namespace WifiJoin
