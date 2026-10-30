#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

/**
 * HTTP client for the Tesserae dashboard REST API (/api/v1/device/*).
 *
 * All methods are synchronous and blocking; call them from the activity loop,
 * never from an ISR or the render task.
 *
 * Tesserae protocol summary:
 *  POST /api/v1/device/discover        — announce {device_id, mac, kind, panel_w, panel_h};
 *                                        registered:false until the admin clicks Register,
 *                                        then device_token + device_id
 *  GET  /api/v1/device/<id>/frame.bmp  — rendered indexed BMP; 404 = no frame yet
 *  POST /api/v1/device/<id>/status     — heartbeat; response carries next_poll_s
 *
 * Auth header on every device request: X-Tesserae-Token: <tessDeviceToken>
 * The cached frame is written to TESSERAE_CACHE_PATH on the SD card by fetchFrame().
 */
class TesseraeClient {
 public:
  enum Error {
    OK = 0,
    NOT_CONFIGURED,     // tessServerUrl empty
    NOT_PAIRED,         // tessDeviceToken empty — need to discover first
    AWAITING_APPROVAL,  // discover returned registered:false — admin must click Register in Tesserae
    NO_FRAME,           // GET frame returned 404 — no dashboard rendered yet
    FRAME_UNCHANGED,    // reserved; the server does not currently answer 304 for frame.bmp
    NETWORK_ERROR,
    SERVER_ERROR,
    LOW_MEMORY,
    INVALID_RESPONSE,
  };

  static constexpr const char* TESSERAE_CACHE_PATH = "/.crosspoint/tesserae_frame.bmp";

  /**
   * True when the dashboard on screen is more than 24 hours old: the last successful frame fetch (stamped from the
   * RTC) is over a day ago or was never recorded. False when the RTC is unavailable, so a missing clock never
   * raises a false alarm.
   */
  static bool frameIsStale();

  /** Blocking join of the saved Wi-Fi network (10 s timeout). Pair with leaveWifi(). */
  static bool joinSavedWifi();
  static void leaveWifi();

  /** Convert an error code to a short English string for logging. */
  static const char* errorString(Error error);

  /**
   * POST /api/v1/device/discover with device MAC, kind, and panel size. Once the
   * server has the device registered, the token and device ID are written to SETTINGS
   * and saved via SETTINGS.saveToFile(). Until then returns AWAITING_APPROVAL.
   *
   * Call this when tessDeviceToken is empty.
   */
  static Error discover();

  /**
   * GET /api/v1/device/<id>/frame.bmp.
   * On 200: streams the BMP to a temp file and moves it over TESSERAE_CACHE_PATH
   * only once the download is complete.
   * On 404: returns NO_FRAME. On 401/403 clears the pairing and returns NOT_PAIRED.
   *
   * Prerequisite: tessDeviceToken must be non-empty (call discover() first).
   */
  static Error fetchFrame();

  /**
   * POST /api/v1/device/<id>/status heartbeat.
   * Sends battery percentage, RSSI, IP address, and firmware version.
   * On success writes the returned next_poll_s into *nextPollSeconds (if non-null).
   */
  static Error sendHeartbeat(uint16_t* nextPollSeconds);

  /**
   * Best-effort refresh run right before the sleep screen is drawn. Joins the last-used saved WiFi
   * network if not already connected, registers with the server if not yet paired (succeeds once
   * the admin has clicked Register), fetches the latest frame, sends a heartbeat, and turns WiFi
   * back off. Never blocks longer than the WiFi connect timeout plus the HTTP calls; on any
   * failure the previously cached frame stays in place.
   */
  static Error refreshBeforeSleep();

  /** Make the next refreshBeforeSleep() a no-op: a timed wake has just fetched and cached the frame. */
  static void markRefreshed();

 private:
  TesseraeClient() = delete;

  // Frames stream to the SD card, so this is only a sanity cap. The server's own cap is 1 MiB;
  // a 480x800 4bpp BMP is ~192 KB.
  static constexpr size_t MAX_FRAME_BYTES = 1024 * 1024;
  // Max JSON response size for discover/status responses (they carry the device config).
  static constexpr size_t MAX_JSON_BYTES = 4096;
};
