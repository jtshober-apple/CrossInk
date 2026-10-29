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
    syncedAt_[kv.key().c_str()] = kv.value().as<uint32_t>();
  }
  LOG_DBG("KOSyncStatus", "Loaded %u sync records", (unsigned)syncedAt_.size());
}

void KOSyncStatusStore::markSynced(const std::string& epubPath) {
  ensureLoaded();
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  syncedAt_[epubPath] = now;
  saveToFile();
  LOG_INF("KOSyncStatus", "Marked synced: %s at %u", epubPath.c_str(), now);
}

bool KOSyncStatusStore::isSynced(const std::string& epubPath) {
  ensureLoaded();
  return syncedAt_.count(epubPath) > 0 && syncedAt_.at(epubPath) > 0;
}

void KOSyncStatusStore::saveToFile() const {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  for (const auto& kv : syncedAt_) {
    obj[kv.first] = kv.second;
  }
  if (!PersistableStoreBase::writeDocToFileAtomically(getFilePath(), doc)) {
    LOG_ERR("KOSyncStatus", "Failed to save sync status file");
  }
}
