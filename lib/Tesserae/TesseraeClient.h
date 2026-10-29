#pragma once

#include <cstdint>
#include <functional>
#include <memory>

/**
 * HTTP client for the Tesserae dashboard REST API.
 *
 * All methods are synchronous and blocking; call them from a FreeRTOS task or
 * the activity loop, never from an ISR or the render task.
 *
 * Tesserae protocol summary:
 *  POST /api/v1/device/discover        — first-run pairing; returns token + id on 200, 202 = awaiting admin
 *  GET  /api/v1/device/<id>/frame.bmp  — fetch rendered BMP; 304 = unchanged (use ETag)
 *  POST /api/v1/device/<id>/status     — heartbeat; response carries next_poll_s
 *
 * Auth header on every request: X-Tesserae-Token: <tessDeviceToken>
 * The cached frame is written to TESSERAE_CACHE_PATH on the SD card by fetchFrame().
 */
class TesseraeClient {
 public:
  enum Error {
    OK = 0,
    NOT_CONFIGURED,   // tessServerUrl empty
    NOT_PAIRED,       // tessDeviceToken empty — need to discover first
    AWAITING_APPROVAL,// discover returned 202 — admin must approve in Tesserae UI
    NO_FRAME,         // GET frame returned 204 — no dashboard assigned
    FRAME_UNCHANGED,  // GET frame returned 304 — ETag matched, skip repaint
    NETWORK_ERROR,
    SERVER_ERROR,
    LOW_MEMORY,
    INVALID_RESPONSE,
  };

  static constexpr const char* TESSERAE_CACHE_PATH = "/.crosspoint/tesserae_frame.bmp";

  /** Convert an error code to a short English string for logging. */
  static const char* errorString(Error error);

  /**
   * POST /api/v1/device/discover with device MAC, screen dimensions, and
   * firmware version. On 200 the token and device ID are written to SETTINGS
   * and saved via SETTINGS.saveToFile(). On 202 returns AWAITING_APPROVAL.
   *
   * Call this when tessDeviceToken is empty.
   */
  static Error discover();

  /**
   * GET /api/v1/device/<id>/frame.bmp.
   * Sends If-None-Match: <tessEtag> if a cached ETag exists.
   * On 200: saves the BMP to TESSERAE_CACHE_PATH and updates tessEtag in SETTINGS.
   * On 304: returns FRAME_UNCHANGED.
   * On 204: returns NO_FRAME.
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

 private:
  TesseraeClient() = delete;

  // Max BMP size allowed for frame download: slightly more than a full 800×480 1-bit BMP
  // (800*480/8 = 48000 bytes) with a generous header allowance.
  static constexpr size_t MAX_FRAME_BYTES = 60000;
  // Max JSON response size for discover/status responses.
  static constexpr size_t MAX_JSON_BYTES = 512;
};
