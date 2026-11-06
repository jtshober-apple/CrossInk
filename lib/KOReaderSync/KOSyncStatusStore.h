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

  // Record that epubPath was successfully pushed right now. pushedPosition is the reading position
  // (spine << 16 | page) that went up; leave it at kUnknownPosition when it isn't known.
  void markSynced(const std::string& epubPath, uint32_t pushedPosition = kUnknownPosition);

  // The reader reports where it is whenever it saves progress. Memory only (no SD write per page turn).
  void notePosition(const std::string& epubPath, uint32_t position);

  // Write the last reported position to SD. Call just before deep sleep: RAM is lost, and a timed
  // wake redraws the sleep screen from a fresh boot.
  void persistPosition();

  // True once epubPath has been pushed AND, when both positions are known, nothing was read since.
  bool isSynced(const std::string& epubPath);

  static constexpr uint32_t kUnknownPosition = 0xFFFFFFFFu;

  static const char* getFilePath() { return "/.crosspoint/ko_sync_status.json"; }

 private:
  KOSyncStatusStore() = default;
  ~KOSyncStatusStore() = default;

  void ensureLoaded();
  void saveToFile() const;

  bool loaded_ = false;
  // path → epoch-seconds of last successful push
  std::unordered_map<std::string, uint32_t> syncedAt_;
  // path → reading position at the last successful push / at the last persisted position report
  std::unordered_map<std::string, uint32_t> pushedPos_;
  std::unordered_map<std::string, uint32_t> currentPos_;
  std::string curPath_;
  uint32_t curPos_ = kUnknownPosition;
  uint32_t persistedCurPos_ = kUnknownPosition;
};

#define KO_SYNC_STATUS KOSyncStatusStore::getInstance()
