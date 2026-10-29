#include "KOSyncStatusStore.h"

#include <Arduino.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <ctime>

void KOSyncStatusStore::ensureLoaded() {
  if (loaded_) return;
  loaded_ = true;

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(getFilePath(), doc)) {
    LOG_DBG("KOSyncStatus", "No status file found (first run or cleared)");
    return;
  }
  if (!doc.is<JsonObject>()) return;
  for (JsonPair kv : doc.as<JsonObject>()) {
    std::string key = kv.key().c_str();
    const uint32_t value = kv.value().as<uint32_t>();
    if (key.size() > 2 && key.compare(key.size() - 2, 2, "|p") == 0) {
      pushedPos_[key.substr(0, key.size() - 2)] = value;
    } else if (key.size() > 2 && key.compare(key.size() - 2, 2, "|c") == 0) {
      currentPos_[key.substr(0, key.size() - 2)] = value;
    } else {
      syncedAt_[key] = value;
    }
  }
  LOG_DBG("KOSyncStatus", "Loaded %u sync records", (unsigned)syncedAt_.size());
}

void KOSyncStatusStore::markSynced(const std::string& epubPath, const uint32_t pushedPosition) {
  ensureLoaded();
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  syncedAt_[epubPath] = now;
  if (pushedPosition != kUnknownPosition) {
    pushedPos_[epubPath] = pushedPosition;
    currentPos_[epubPath] = pushedPosition;
  } else {
    pushedPos_.erase(epubPath);
  }
  saveToFile();
  LOG_INF("KOSyncStatus", "Marked synced: %s at %u", epubPath.c_str(), now);
}

void KOSyncStatusStore::notePosition(const std::string& epubPath, const uint32_t position) {
  curPath_ = epubPath;
  curPos_ = position;
}

void KOSyncStatusStore::persistPosition() {
  if (curPath_.empty() || curPos_ == kUnknownPosition || curPos_ == persistedCurPos_) return;
  ensureLoaded();
  currentPos_[curPath_] = curPos_;
  persistedCurPos_ = curPos_;
  saveToFile();
}

bool KOSyncStatusStore::isSynced(const std::string& epubPath) {
  ensureLoaded();
  if (syncedAt_.count(epubPath) == 0 || syncedAt_.at(epubPath) == 0) return false;
  const auto pushed = pushedPos_.find(epubPath);
  if (pushed == pushedPos_.end()) return true;  // pushed before positions were recorded: old behaviour
  uint32_t current = kUnknownPosition;
  if (curPath_ == epubPath && curPos_ != kUnknownPosition) {
    current = curPos_;
  } else {
    const auto persisted = currentPos_.find(epubPath);
    if (persisted != currentPos_.end()) current = persisted->second;
  }
  return current == kUnknownPosition || current == pushed->second;
}

void KOSyncStatusStore::saveToFile() const {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  for (const auto& kv : syncedAt_) {
    obj[kv.first] = kv.second;
  }
  for (const auto& kv : pushedPos_) {
    obj[kv.first + "|p"] = kv.second;
  }
  for (const auto& kv : currentPos_) {
    obj[kv.first + "|c"] = kv.second;
  }
  if (!PersistableStoreBase::writeDocToFileAtomically(getFilePath(), doc)) {
    LOG_ERR("KOSyncStatus", "Failed to save sync status file");
  }
}
