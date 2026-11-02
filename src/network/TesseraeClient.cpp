#include "network/TesseraeClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "network/KoSyncQuiet.h"
#include "network/WifiUtils.h"
#include "util/RtcMinutes.h"

namespace {
constexpr const char* TAG = "TESS";

// The crosspoint_gray device kind on the Tesserae side (devices/crosspoint_gray/device.json).
constexpr const char* DEVICE_KIND = "crosspoint_gray";
constexpr int PANEL_W = 480;
constexpr int PANEL_H = 800;
constexpr const char* FIRMWARE_ID = "crossink";
constexpr const char* FRAME_TMP_PATH = "/.crosspoint/tesserae_frame.tmp";
constexpr uint32_t SLEEP_WIFI_TIMEOUT_MS = 7000;
// Same order of magnitude as the KOSync TLS gate; skip the refresh rather than risk a failed handshake.
constexpr uint32_t SLEEP_MIN_FREE_HEAP = 40000;
constexpr uint32_t SLEEP_MIN_MAX_ALLOC_HEAP = 24000;
// Pressing power must reach the sleep screen in a predictable time: stages that would start too late are skipped
// and the cached frame is shown instead. Typical total is ~10-15 s; this only bounds the bad days.
constexpr uint32_t SLEEP_SYNC_LATEST_START_MS = 6000;    // KoSync only starts if Wi-Fi was up by now
constexpr uint32_t SLEEP_FETCH_LATEST_START_MS = 18000;  // the frame download only starts before this
// After a push the server re-renders from the new position; check again soon instead of making the user wait.
constexpr uint32_t QUICK_RECHECK_SECONDS = 75;
constexpr uint32_t DEFERRED_REFRESH_SECONDS = 15;

// Blocking join of the last-used (else first) saved network. Returns true once connected.
bool connectSavedWifi() {
  std::optional<WifiCredential> cred;
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty()) cred = WIFI_STORE.findCredential(last);
  if (!cred) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) {
    LOG_DBG(TAG, "sleep refresh: no saved WiFi network");
    return false;
  }

  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) return false;
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  const uint32_t start = millis();
  while (millis() - start < SLEEP_WIFI_TIMEOUT_MS) {
    if (hasActiveStationWifiConnection()) return true;
    delay(100);
  }
  LOG_ERR(TAG, "sleep refresh: WiFi connect timed out (%s)", cred->ssid.c_str());
  return false;
}

void wifiOff() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

std::string baseUrl() {
  std::string url = SETTINGS.tessServerUrl;
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

// Returns "<serverUrl>/api/v1/device/<id>/<endpoint>"
std::string makeDeviceUrl(const char* deviceId, const char* endpoint) {
  std::string url = baseUrl();
  url += "/api/v1/device/";
  url += deviceId;
  url += '/';
  url += endpoint;
  return url;
}

void readMac(uint8_t (&mac)[6]) {
  std::memset(mac, 0, sizeof(mac));
#ifndef SIMULATOR
  WiFi.macAddress(mac);
#else
  const uint8_t simMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  std::memcpy(mac, simMac, sizeof(mac));
#endif
}

// Sends a JSON POST and collects up to `maxBytes` of the response body. Returns the HTTP status
// (<= 0 on transport failure). `withToken` adds the per-device auth header.
int postJson(const std::string& url, const std::string& body, bool withToken, std::string& response, size_t maxBytes) {
  response.clear();
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR(TAG, "bad URL: %s", url.c_str());
    return -1;
  }
  http.addHeader("Content-Type", "application/json");
  if (withToken) http.addHeader("X-Tesserae-Token", std::string(SETTINGS.tessDeviceToken));

  const int code = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                    [&](const uint8_t* data, size_t len) {
                                      if (http.getStatus() != 200) return true;
                                      if (response.size() + len > maxBytes) return false;
                                      response.append(reinterpret_cast<const char*>(data), len);
                                      return true;
                                    });
  http.end();
  return code;
}

constexpr const char* FETCH_STAMP_PATH = "/.crosspoint/tesserae_fetched";
// Exists once the sleep screen was drawn with the stale-dashboard mark; cleared by the next good fetch.
constexpr const char* STALE_DRAWN_PATH = "/.crosspoint/tesserae_stale_drawn";
constexpr int64_t STALE_AFTER_MINUTES = 24 * 60;
constexpr int64_t RETRY_STALE_AFTER_MINUTES = 60;

void stampFetchTime() {
  Storage.remove(STALE_DRAWN_PATH);
  int64_t now = 0;
  if (!rtcMinutesSinceEpoch(now)) return;
  FsFile file;
  if (!Storage.openFileForWrite("TESS", FETCH_STAMP_PATH, file)) return;
  char buf[24];
  const int n = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(now));
  file.write(reinterpret_cast<const uint8_t*>(buf), n);
  file.close();
}
}  // namespace

const char* TesseraeClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "OK";
    case NOT_CONFIGURED:
      return "NOT_CONFIGURED";
    case NOT_PAIRED:
      return "NOT_PAIRED";
    case AWAITING_APPROVAL:
      return "AWAITING_APPROVAL";
    case NO_FRAME:
      return "NO_FRAME";
    case FRAME_UNCHANGED:
      return "FRAME_UNCHANGED";
    case NETWORK_ERROR:
      return "NETWORK_ERROR";
    case SERVER_ERROR:
      return "SERVER_ERROR";
    case LOW_MEMORY:
      return "LOW_MEMORY";
    case INVALID_RESPONSE:
      return "INVALID_RESPONSE";
    default:
      return "UNKNOWN";
  }
}

// ── discover ──────────────────────────────────────────────────────────────────

TesseraeClient::Error TesseraeClient::discover() {
  if (SETTINGS.tessServerUrl[0] == '\0') {
    LOG_ERR(TAG, "discover: no server URL configured");
    return NOT_CONFIGURED;
  }

  uint8_t mac[6];
  readMac(mac);
  char macBuf[18];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  // Stable per-device id announced to the server; the server hands back the stored id on a MAC match.
  char idBuf[24];
  snprintf(idBuf, sizeof(idBuf), "crossink-%02x%02x%02x", mac[3], mac[4], mac[5]);

  JsonDocument bodyDoc;
  bodyDoc["device_id"] = idBuf;
  bodyDoc["mac"] = macBuf;
  bodyDoc["kind"] = DEVICE_KIND;
  bodyDoc["panel_w"] = PANEL_W;
  bodyDoc["panel_h"] = PANEL_H;
  bodyDoc["fw_version"] = FIRMWARE_ID;
  std::string body;
  serializeJson(bodyDoc, body);

  const std::string url = baseUrl() + "/api/v1/device/discover";
  LOG_DBG(TAG, "discover: POST %s", url.c_str());

  std::string response;
  const int httpCode = postJson(url, body, false, response, MAX_JSON_BYTES);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode != 200) {
    LOG_ERR(TAG, "discover: HTTP %d", httpCode);
    return SERVER_ERROR;
  }

  JsonDocument respDoc;
  if (deserializeJson(respDoc, response) != DeserializationError::Ok) return INVALID_RESPONSE;

  // Not registered yet: the admin has to click Register on this device in Settings -> Devices.
  if (!(respDoc["registered"] | false)) return AWAITING_APPROVAL;

  const char* token = respDoc["device_token"] | "";
  const char* devId = respDoc["device_id"] | "";
  if (token[0] == '\0' || devId[0] == '\0') return INVALID_RESPONSE;
  if (std::strlen(token) >= sizeof(SETTINGS.tessDeviceToken) || std::strlen(devId) >= sizeof(SETTINGS.tessDeviceId)) {
    LOG_ERR(TAG, "discover: token or device id too long for settings");
    return INVALID_RESPONSE;
  }
  snprintf(SETTINGS.tessDeviceToken, sizeof(SETTINGS.tessDeviceToken), "%s", token);
  snprintf(SETTINGS.tessDeviceId, sizeof(SETTINGS.tessDeviceId), "%s", devId);
  SETTINGS.tessEtag[0] = '\0';
  SETTINGS.saveToFile();
  LOG_INF(TAG, "discover: paired as device %s", devId);
  return OK;
}

// ── fetchFrame ────────────────────────────────────────────────────────────────

TesseraeClient::Error TesseraeClient::fetchFrame() {
  if (SETTINGS.tessServerUrl[0] == '\0') return NOT_CONFIGURED;
  if (SETTINGS.tessDeviceToken[0] == '\0') return NOT_PAIRED;

  const std::string url = makeDeviceUrl(SETTINGS.tessDeviceId, "frame.bmp");
  LOG_DBG(TAG, "fetchFrame: GET %s", url.c_str());

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR(TAG, "fetchFrame: bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  http.addHeader("X-Tesserae-Token", std::string(SETTINGS.tessDeviceToken));

  // Stream into a temp file so a failed or partial download never replaces the live sleep screen.
  Storage.ensureDirectoryExists("/.crosspoint");
  FsFile file;
  bool opened = false;
  bool writeFailed = false;
  bool tooLarge = false;
  size_t total = 0;

  const int httpCode = http.GET([&](const uint8_t* data, size_t len) {
    if (http.getStatus() != 200) return true;
    if (!opened) {
      Storage.remove(FRAME_TMP_PATH);
      if (!Storage.openFileForWrite("TESS", FRAME_TMP_PATH, file)) {
        writeFailed = true;
        return false;
      }
      opened = true;
    }
    if (total + len > MAX_FRAME_BYTES) {
      tooLarge = true;
      return false;
    }
    if (file.write(data, len) != len) {
      writeFailed = true;
      return false;
    }
    total += len;
    return true;
  });
  const bool complete = http.responseComplete();
  http.end();
  if (opened) file.close();

  auto discardTmp = [] { Storage.remove(FRAME_TMP_PATH); };

  if (httpCode == 401 || httpCode == 403) {
    // The server no longer recognises our token (device deleted or token regenerated): re-pair.
    LOG_ERR(TAG, "fetchFrame: token rejected (HTTP %d), clearing pairing", httpCode);
    SETTINGS.tessDeviceToken[0] = '\0';
    SETTINGS.saveToFile();
    discardTmp();
    return NOT_PAIRED;
  }
  if (httpCode == 404 || httpCode == 204) {
    discardTmp();
    return NO_FRAME;
  }
  if (httpCode <= 0) {
    discardTmp();
    return NETWORK_ERROR;
  }
  if (httpCode != 200) {
    LOG_ERR(TAG, "fetchFrame: HTTP %d", httpCode);
    discardTmp();
    return SERVER_ERROR;
  }
  if (tooLarge) {
    LOG_ERR(TAG, "fetchFrame: frame too large (>%zu bytes)", MAX_FRAME_BYTES);
    discardTmp();
    return SERVER_ERROR;
  }
  if (writeFailed) {
    LOG_ERR(TAG, "fetchFrame: SD write failed");
    discardTmp();
    return SERVER_ERROR;
  }
  if (!complete || total == 0) {
    LOG_ERR(TAG, "fetchFrame: incomplete download (%zu bytes)", total);
    discardTmp();
    return INVALID_RESPONSE;
  }

  Storage.remove(TESSERAE_CACHE_PATH);
  if (!Storage.rename(FRAME_TMP_PATH, TESSERAE_CACHE_PATH)) {
    LOG_ERR(TAG, "fetchFrame: failed to move frame into place: %s", TESSERAE_CACHE_PATH);
    discardTmp();
    return SERVER_ERROR;
  }

  LOG_INF(TAG, "fetchFrame: saved %zu bytes to %s", total, TESSERAE_CACHE_PATH);
  stampFetchTime();
  return OK;
}

// ── sendHeartbeat ─────────────────────────────────────────────────────────────

TesseraeClient::Error TesseraeClient::sendHeartbeat(uint16_t* nextPollSeconds) {
  if (SETTINGS.tessServerUrl[0] == '\0') return NOT_CONFIGURED;
  if (SETTINGS.tessDeviceToken[0] == '\0') return NOT_PAIRED;

  const std::string url = makeDeviceUrl(SETTINGS.tessDeviceId, "status");
  LOG_DBG(TAG, "heartbeat: POST %s", url.c_str());

  uint8_t mac[6];
  readMac(mac);
  char macBuf[18];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  char ipBuf[16] = "0.0.0.0";
#ifndef SIMULATOR
  const auto ip = WiFi.localIP();
  snprintf(ipBuf, sizeof(ipBuf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
#endif

  JsonDocument bodyDoc;
  bodyDoc["rssi"] = WiFi.RSSI();
  bodyDoc["battery_pct"] = powerManager.getBatteryPercentage();
  // The open book, so the dashboard footer follows what is being read rather than guessing from server positions.
  {
    const std::string& openPath = APP_STATE.openEpubPath;
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (book.path == openPath && !book.title.empty()) {
        bodyDoc["book_title"] = book.title;
        break;
      }
    }
  }
  bodyDoc["ip"] = ipBuf;
  bodyDoc["mac"] = macBuf;
  bodyDoc["fw_version"] = FIRMWARE_ID;
  bodyDoc["panel_w"] = PANEL_W;
  bodyDoc["panel_h"] = PANEL_H;
  std::string body;
  serializeJson(bodyDoc, body);

  std::string response;
  const int httpCode = postJson(url, body, true, response, MAX_JSON_BYTES);
  if (httpCode == 401 || httpCode == 403) {
    SETTINGS.tessDeviceToken[0] = '\0';
    SETTINGS.saveToFile();
    return NOT_PAIRED;
  }
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode != 200) {
    LOG_ERR(TAG, "heartbeat: HTTP %d", httpCode);
    return SERVER_ERROR;
  }

  if (nextPollSeconds) {
    JsonDocument respDoc;
    if (deserializeJson(respDoc, response) == DeserializationError::Ok) {
      const uint32_t next = respDoc["next_poll_s"] | 300u;
      *nextPollSeconds = next > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(next);
    }
  }
  return OK;
}

// ── refreshBeforeSleep ────────────────────────────────────────────────────────

namespace {
bool refreshedThisBoot = false;
bool timedWakeRender = false;
bool lastAttemptFailed = false;
bool quickRecheck = false;
bool deferredRefresh = false;
std::function<void(const char*)> stepListener;

void notifyStep(const char* text) {
  if (stepListener) stepListener(text);
}
}  // namespace

bool TesseraeClient::joinSavedWifi() { return connectSavedWifi(); }
void TesseraeClient::leaveWifi() { wifiOff(); }

void TesseraeClient::markRefreshed() { refreshedThisBoot = true; }
void TesseraeClient::setTimedWake(const bool v) { timedWakeRender = v; }
bool TesseraeClient::isTimedWake() { return timedWakeRender; }
uint32_t TesseraeClient::takeQuickRecheckSeconds() {
  const bool want = quickRecheck;
  quickRecheck = false;
  return want ? QUICK_RECHECK_SECONDS : 0;
}
void TesseraeClient::deferRefresh() { deferredRefresh = true; }
uint32_t TesseraeClient::takeDeferredRefreshSeconds() {
  const bool want = deferredRefresh;
  deferredRefresh = false;
  return want ? DEFERRED_REFRESH_SECONDS : 0;
}
void TesseraeClient::setStepListener(std::function<void(const char*)> listener) { stepListener = std::move(listener); }

TesseraeClient::Error TesseraeClient::refreshBeforeSleep(const bool timedWake) {
  if (SETTINGS.tessServerUrl[0] == '\0') return NOT_CONFIGURED;
  if (refreshedThisBoot) {
    refreshedThisBoot = false;
    return OK;
  }

  if (ESP.getFreeHeap() < SLEEP_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < SLEEP_MIN_MAX_ALLOC_HEAP) {
    LOG_ERR(TAG, "sleep refresh: not enough heap (free=%u maxAlloc=%u), using cached frame",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return LOW_MEMORY;
  }

  const uint32_t startedMs = millis();
  const bool alreadyConnected = hasActiveStationWifiConnection();
  if (!alreadyConnected) notifyStep(tr(STR_STEP_CONNECTING));
  if (!alreadyConnected && !connectSavedWifi()) {
    wifiOff();
    lastAttemptFailed = true;
    return NETWORK_ERROR;
  }

  // Tell the server which book is open before anything slow, so it can start redrawing the footer for it.
  if (timedWake) {
    uint16_t ignoredPoll = 0;
    sendHeartbeat(&ignoredPoll);
  }

  // Sync the open book first, while the heap is at its largest (TLS needs it). Going to sleep only pushes (the
  // reader is the newest source); a timed wake also pulls, so a book read or listened to elsewhere reaches this
  // reader. Skipped when Wi-Fi was slow: a late sync would hold up the sleep screen.
  // A timed wake has nobody waiting on it: it always pushes and pulls, with no time cap.
  const bool syncing = timedWake || SETTINGS.koSyncPushOnSleep;
  if (syncing && (timedWake || millis() - startedMs <= SLEEP_SYNC_LATEST_START_MS)) {
    notifyStep((timedWake ? tr(STR_STEP_SYNCING) : tr(STR_STEP_UPLOADING)));
    const auto synced =
        KoSyncQuiet::syncBoth(APP_STATE.openEpubPath, timedWake || SETTINGS.koSyncPushOnSleep != 0, timedWake);
    LOG_INF(TAG, "refresh: kosync result=%d (timedWake=%d)", static_cast<int>(synced), timedWake);
    // The relay and the server need a moment to redraw the footer from the pushed position: look again shortly.
    quickRecheck = synced == KoSyncQuiet::Result::PUSHED;
  } else if (syncing) {
    LOG_INF(TAG, "refresh: kosync skipped, Wi-Fi took %u ms", static_cast<unsigned>(millis() - startedMs));
  }

  Error err = OK;
  if (!timedWake && millis() - startedMs > SLEEP_FETCH_LATEST_START_MS) {
    LOG_INF(TAG, "refresh: out of time, showing the cached frame");
    err = NETWORK_ERROR;
  } else {
    notifyStep(tr(STR_STEP_FETCHING));
    if (SETTINGS.tessDeviceToken[0] == '\0') {
      err = discover();
      LOG_INF(TAG, "sleep refresh: discover: %s", errorString(err));
    }
    if (err == OK) {
      err = fetchFrame();
      if (err == NOT_PAIRED) {
        // Server forgot us; try to re-claim once (works if the device is still registered by MAC).
        err = discover();
        if (err == OK) err = fetchFrame();
      }
      LOG_INF(TAG, "sleep refresh: fetchFrame: %s", errorString(err));
      if (err == OK) {
        uint16_t nextPollSeconds = 0;
        sendHeartbeat(&nextPollSeconds);
      }
    }
  }
  lastAttemptFailed = err != OK;

  notifyStep(tr(STR_ENTERING_SLEEP));
  if (!alreadyConnected) wifiOff();
  return err;
}

bool TesseraeClient::staleMarkNeedsRedraw() { return frameIsStale() && !Storage.exists(STALE_DRAWN_PATH); }

void TesseraeClient::markStaleDrawn() {
  FsFile file;
  if (Storage.openFileForWrite("TESS", STALE_DRAWN_PATH, file)) file.close();
}

bool TesseraeClient::frameIsStale() {
  int64_t now = 0;
  if (!rtcMinutesSinceEpoch(now)) return false;
  FsFile file;
  if (!Storage.openFileForRead("TESS", FETCH_STAMP_PATH, file)) return true;
  char buf[24] = {};
  file.read(buf, sizeof(buf) - 1);
  file.close();
  const long long stamp = atoll(buf);
  if (stamp <= 0) return true;
  // A day without a refresh is always stale; after a failed attempt an hour is enough to say so.
  return now - stamp > (lastAttemptFailed ? RETRY_STALE_AFTER_MINUTES : STALE_AFTER_MINUTES);
}
