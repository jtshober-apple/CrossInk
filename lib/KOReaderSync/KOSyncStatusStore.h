#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <unordered_map>

/**
 * Lightweight store that records the epoch timestamp of the last successful
 * KOReader auto-push for each book path.  Written to SD as a small JSON file
 * so the indicator survives reboots.
 *
 * Loaded lazily on first access; write is fire-and-forget (SD errors are
 * logged but never propagate — the indicator is a best-effort UI aid, not
 * authoritative sync state).
 */
class KOSyncStatusStore {
 public:
  static KOSyncStatusStore& getInstance() {
    static KOSyncStatusStore instance;
    return instance;
  }

  // Record that epubPath was successfully pushed right now.
  void markSynced(const std::string& epubPath);

  // True if epubPath has ever been successfully pushed on this device.
  bool isSynced(const std::string& epubPath);

  static const char* getFilePath() { return "/.crosspoint/ko_sync_status.json"; }

 private:
  KOSyncStatusStore() = default;
  ~KOSyncStatusStore() = default;

  void ensureLoaded();
  void saveToFile() const;

  bool loaded_ = false;
  // path → epoch-seconds of last successful push
  std::unordered_map<std::string, uint32_t> syncedAt_;
};

#define KO_SYNC_STATUS KOSyncStatusStore::getInstance()
