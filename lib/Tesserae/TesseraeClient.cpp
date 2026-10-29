#include "TesseraeClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <HalStorage.h>
#include <WiFi.h>

#ifdef SIMULATOR
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#else
#include <SecureHttpClient.h>
#endif

#include <cstdio>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "Memory.h"

namespace {
constexpr const char* TAG = "TESS";

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

// Returns "<serverUrl>/api/v1/device/<id>/<endpoint>"
std::string makeDeviceUrl(const char* serverUrl, const char* deviceId, const char* endpoint) {
  std::string url = serverUrl;
  if (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/v1/device/";
  url += deviceId;
  url += '/';
  url += endpoint;
  return url;
}
}  // namespace

const char* TesseraeClient::errorString(Error error) {
  switch (error) {
    case OK:                return "OK";
    case NOT_CONFIGURED:    return "NOT_CONFIGURED";
    case NOT_PAIRED:        return "NOT_PAIRED";
    case AWAITING_APPROVAL: return "AWAITING_APPROVAL";
    case NO_FRAME:          return "NO_FRAME";
    case FRAME_UNCHANGED:   return "FRAME_UNCHANGED";
    case NETWORK_ERROR:     return "NETWORK_ERROR";
    case SERVER_ERROR:      return "SERVER_ERROR";
    case LOW_MEMORY:        return "LOW_MEMORY";
    case INVALID_RESPONSE:  return "INVALID_RESPONSE";
    default:                return "UNKNOWN";
  }
}

// ── discover ──────────────────────────────────────────────────────────────────

TesseraeClient::Error TesseraeClient::discover() {
  if (SETTINGS.tessServerUrl[0] == '\0') {
    LOG_ERR(TAG, "discover: no server URL configured");
    return NOT_CONFIGURED;
  }

  std::string url = SETTINGS.tessServerUrl;
  if (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/v1/device/discover";

  // Build JSON body: { mac, firmware_version, width, height, color_depth }
  JsonDocument bodyDoc;
  char macBuf[18];
  const uint8_t mac[] = {0, 0, 0, 0, 0, 0};
  uint8_t macArr[6];
#ifndef SIMULATOR
  WiFi.macAddress(macArr);
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           macArr[0], macArr[1], macArr[2], macArr[3], macArr[4], macArr[5]);
#else
  snprintf(macBuf, sizeof(macBuf), "AA:BB:CC:DD:EE:FF");
#endif
  bodyDoc["mac"] = macBuf;
  bodyDoc["firmware_version"] = "crossink-1.x";
  bodyDoc["width"] = 800;
  bodyDoc["height"] = 480;
  bodyDoc["color_depth"] = 1;

  char bodyBuf[256];
  const size_t bodyLen = serializeJson(bodyDoc, bodyBuf, sizeof(bodyBuf));
  (void)bodyLen;

  LOG_DBG(TAG, "discover: POST %s", url.c_str());

#ifdef SIMULATOR
  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  if (isHttpsUrl(url)) {
    secureClient.setInsecure();
    http.begin(secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.POST(bodyBuf);
  if (httpCode == 202) { http.end(); return AWAITING_APPROVAL; }
  if (httpCode != 200) {
    LOG_ERR(TAG, "discover: HTTP %d", httpCode);
    http.end();
    return (httpCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
  }
  const String resp = http.getString();
  http.end();
  JsonDocument respDoc;
  if (deserializeJson(respDoc, resp) != DeserializationError::Ok) return INVALID_RESPONSE;
  const char* token = respDoc["token"] | "";
  const char* devId = respDoc["device_id"] | "";
  if (token[0] == '\0' || devId[0] == '\0') return INVALID_RESPONSE;
  snprintf(SETTINGS.tessDeviceToken, sizeof(SETTINGS.tessDeviceToken), "%s", token);
  snprintf(SETTINGS.tessDeviceId, sizeof(SETTINGS.tessDeviceId), "%s", devId);
  SETTINGS.saveToFile();
  LOG_INF(TAG, "discover: paired as device %s", devId);
  return OK;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR(TAG, "discover: bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  http.addHeader("Content-Type", "application/json");

  auto respBuf = makeUniqueNoThrow<char[]>(MAX_JSON_BYTES + 1);
  if (!respBuf) { http.end(); return LOW_MEMORY; }
  size_t respLen = 0;
  bool tooLarge = false;

  const int httpCode = http.POST(bodyBuf, [&](const uint8_t* data, size_t len) {
    if (http.getStatus() != 200) return true;
    if (respLen + len > MAX_JSON_BYTES) { tooLarge = true; return false; }
    std::memcpy(respBuf.get() + respLen, data, len);
    respLen += len;
    respBuf[respLen] = '\0';
    return true;
  });

  http.end();

  if (httpCode == 202) return AWAITING_APPROVAL;
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode != 200) { LOG_ERR(TAG, "discover: HTTP %d", httpCode); return SERVER_ERROR; }
  if (tooLarge) return INVALID_RESPONSE;

  JsonDocument respDoc;
  if (deserializeJson(respDoc, respBuf.get(), respLen) != DeserializationError::Ok) return INVALID_RESPONSE;
  const char* token = respDoc["token"] | "";
  const char* devId = respDoc["device_id"] | "";
  if (token[0] == '\0' || devId[0] == '\0') return INVALID_RESPONSE;
  snprintf(SETTINGS.tessDeviceToken, sizeof(SETTINGS.tessDeviceToken), "%s", token);
  snprintf(SETTINGS.tessDeviceId, sizeof(SETTINGS.tessDeviceId), "%s", devId);
  SETTINGS.saveToFile();
  LOG_INF(TAG, "discover: paired as device %s", devId);
  return OK;
#endif
}

// ── fetchFrame ────────────────────────────────────────────────────────────────

TesseraeClient::Error TesseraeClient::fetchFrame() {
  if (SETTINGS.tessServerUrl[0] == '\0') return NOT_CONFIGURED;
  if (SETTINGS.tessDeviceToken[0] == '\0') return NOT_PAIRED;

  const std::string url = makeDeviceUrl(SETTINGS.tessServerUrl, SETTINGS.tessDeviceId, "frame.bmp");
  LOG_DBG(TAG, "fetchFrame: GET %s", url.c_str());

#ifdef SIMULATOR
  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  if (isHttpsUrl(url)) {
    secureClient.setInsecure();
    http.begin(secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  if (SETTINGS.tessEtag[0] != '\0') http.addHeader("If-None-Match", SETTINGS.tessEtag);
  http.addHeader("X-Tesserae-Token", SETTINGS.tessDeviceToken);
  const int httpCode = http.GET();
  if (httpCode == 304) { http.end(); return FRAME_UNCHANGED; }
  if (httpCode == 204) { http.end(); return NO_FRAME; }
  if (httpCode != 200) { http.end(); return (httpCode < 0) ? NETWORK_ERROR : SERVER_ERROR; }
  // Save to SD
  FsFile file;
  if (!Storage.openFileForWrite("TESS", TesseraeClient::TESSERAE_CACHE_PATH, file)) {
    LOG_ERR(TAG, "fetchFrame: failed to open cache file for write");
    http.end();
    return SERVER_ERROR;
  }
  const String body = http.getString();
  file.write(reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
  file.close();
  // Capture ETag
  const String etag = http.header("ETag");
  if (etag.length() > 0 && etag.length() < sizeof(SETTINGS.tessEtag)) {
    snprintf(SETTINGS.tessEtag, sizeof(SETTINGS.tessEtag), "%s", etag.c_str());
    SETTINGS.saveToFile();
  }
  http.end();
  return OK;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) { LOG_ERR(TAG, "fetchFrame: bad URL: %s", url.c_str()); return NETWORK_ERROR; }
  if (SETTINGS.tessEtag[0] != '\0') http.addHeader("If-None-Match", std::string(SETTINGS.tessEtag));
  http.addHeader("X-Tesserae-Token", std::string(SETTINGS.tessDeviceToken));

  // Stream BMP bytes directly into a heap buffer, then write to SD.
  auto frameBuf = makeUniqueNoThrow<uint8_t[]>(MAX_FRAME_BYTES);
  if (!frameBuf) { http.end(); return LOW_MEMORY; }
  size_t frameLen = 0;
  bool tooLarge = false;
  std::string etag;

  const int httpCode = http.GET([&](const uint8_t* data, size_t len) {
    if (http.getStatus() == 304 || http.getStatus() == 204) return true;
    if (http.getStatus() != 200) return true;
    if (frameLen + len > MAX_FRAME_BYTES) { tooLarge = true; return false; }
    std::memcpy(frameBuf.get() + frameLen, data, len);
    frameLen += len;
    return true;
  });

  // Capture ETag before end().
  const std::string etagVal = http.getResponseHeader("ETag");
  http.end();

  if (httpCode == 304) return FRAME_UNCHANGED;
  if (httpCode == 204) return NO_FRAME;
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode != 200) { LOG_ERR(TAG, "fetchFrame: HTTP %d", httpCode); return SERVER_ERROR; }
  if (tooLarge) { LOG_ERR(TAG, "fetchFrame: frame too large (>%zu bytes)", MAX_FRAME_BYTES); return SERVER_ERROR; }
  if (frameLen == 0) return INVALID_RESPONSE;

  // Write BMP to SD cache path.
  FsFile file;
  if (!Storage.openFileForWrite("TESS", TesseraeClient::TESSERAE_CACHE_PATH, file)) {
    LOG_ERR(TAG, "fetchFrame: failed to open cache file for write: %s", TesseraeClient::TESSERAE_CACHE_PATH);
    return SERVER_ERROR;
  }
  file.write(frameBuf.get(), frameLen);
  file.close();

  // Update cached ETag.
  if (!etagVal.empty() && etagVal.size() < sizeof(SETTINGS.tessEtag)) {
    snprintf(SETTINGS.tessEtag, sizeof(SETTINGS.tessEtag), "%s", etagVal.c_str());
    SETTINGS.saveToFile();
  }

  LOG_INF(TAG, "fetchFrame: saved %zu bytes to %s", frameLen, TesseraeClient::TESSERAE_CACHE_PATH);
  return OK;
#endif
}

// ── sendHeartbeat ─────────────────────────────────────────────────────────────

TesseraeClient::Error TesseraeClient::sendHeartbeat(uint16_t* nextPollSeconds) {
  if (SETTINGS.tessServerUrl[0] == '\0') return NOT_CONFIGURED;
  if (SETTINGS.tessDeviceToken[0] == '\0') return NOT_PAIRED;

  const std::string url = makeDeviceUrl(SETTINGS.tessServerUrl, SETTINGS.tessDeviceId, "status");
  LOG_DBG(TAG, "heartbeat: POST %s", url.c_str());

  // Battery and RSSI may be unavailable; fall back to safe values.
  const int rssi = WiFi.RSSI();
  // Battery percentage: HAL may provide a getter; use 0 as a safe fallback.
  constexpr uint8_t batteryPct = 0;

  char ipBuf[16] = "0.0.0.0";
#ifndef SIMULATOR
  const auto ip = WiFi.localIP();
  snprintf(ipBuf, sizeof(ipBuf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
#endif

  JsonDocument bodyDoc;
  bodyDoc["battery_pct"] = batteryPct;
  bodyDoc["rssi"] = rssi;
  bodyDoc["ip"] = ipBuf;
  bodyDoc["firmware_version"] = "crossink-1.x";
  char bodyBuf[256];
  serializeJson(bodyDoc, bodyBuf, sizeof(bodyBuf));

#ifdef SIMULATOR
  HTTPClient http;
  WiFiClient plainClient;
  http.begin(plainClient, url.c_str());
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Tesserae-Token", SETTINGS.tessDeviceToken);
  const int httpCode = http.POST(bodyBuf);
  if (httpCode != 200) { http.end(); return (httpCode < 0) ? NETWORK_ERROR : SERVER_ERROR; }
  if (nextPollSeconds) {
    const String resp = http.getString();
    JsonDocument respDoc;
    if (deserializeJson(respDoc, resp) == DeserializationError::Ok) {
      *nextPollSeconds = respDoc["next_poll_s"] | static_cast<uint16_t>(300);
    }
  }
  http.end();
  return OK;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) return NETWORK_ERROR;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Tesserae-Token", std::string(SETTINGS.tessDeviceToken));

  auto respBuf = makeUniqueNoThrow<char[]>(MAX_JSON_BYTES + 1);
  if (!respBuf) { http.end(); return LOW_MEMORY; }
  size_t respLen = 0;

  const int httpCode = http.POST(bodyBuf, [&](const uint8_t* data, size_t len) {
    if (http.getStatus() != 200) return true;
    if (respLen + len > MAX_JSON_BYTES) return false;
    std::memcpy(respBuf.get() + respLen, data, len);
    respLen += len;
    respBuf[respLen] = '\0';
    return true;
  });
  http.end();

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode != 200) { LOG_ERR(TAG, "heartbeat: HTTP %d", httpCode); return SERVER_ERROR; }

  if (nextPollSeconds && respLen > 0) {
    JsonDocument respDoc;
    if (deserializeJson(respDoc, respBuf.get(), respLen) == DeserializationError::Ok) {
      *nextPollSeconds = respDoc["next_poll_s"] | static_cast<uint16_t>(300);
    }
  }
  return OK;
#endif
}
