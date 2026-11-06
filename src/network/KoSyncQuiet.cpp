#include "network/KoSyncQuiet.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "KOSyncStatusStore.h"
#include "ProgressMapper.h"
#include "activities/reader/EpubReaderUtils.h"
#include "network/TesseraeClient.h"
#include "network/WifiUtils.h"

extern GfxRenderer renderer;  // main.cpp; only used to look up cached section layouts when mapping a pulled position

namespace {
constexpr const char* TAG = "KOSYNCQ";
constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points, as the interactive sync uses
constexpr uint32_t MIN_FREE_HEAP = 40000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP = 24000;

bool wakePullFailed = false;

struct Local {
  std::shared_ptr<Epub> epub;
  std::string hash;
  int spine = 0;
  int page = 0;
  int pageCount = 1;
  SavedProgressPosition ko;
  bool koValid = false;
};

bool heapOk() { return ESP.getFreeHeap() >= MIN_FREE_HEAP && ESP.getMaxAllocHeap() >= MIN_MAX_ALLOC_HEAP; }

bool usable(const std::string& epubPath) {
  return !epubPath.empty() && KOREADER_STORE.hasCredentials() && hasActiveStationWifiConnection() && heapOk();
}

// reader progress file: spine u16, page u16, pageCount u16, optional visible text offset u32 (see EpubReaderUtils)
struct StoredProgress {
  int spine = 0;
  int page = 0;
  int pageCount = 1;
  bool hasOffset = false;
  uint32_t offset = 0;
};

bool readStoredProgress(const Epub& epub, StoredProgress& out) {
  HalFile file;
  if (!Storage.openFileForRead("KOSYNCQ", epub.getCachePath() + "/progress.bin", file)) return false;
  uint8_t data[10] = {};
  const int size = file.read(data, sizeof(data));
  file.close();
  if (size != 4 && size != 6 && size != 10) return false;
  out.spine = data[0] | (data[1] << 8);
  out.page = data[2] | (data[3] << 8);
  if (size >= 6) out.pageCount = std::max(1, data[4] | (data[5] << 8));
  if (size == 10) {
    out.hasOffset = true;
    out.offset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                 (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
  }
  return true;
}

bool loadLocal(const std::string& epubPath, Local& out) {
  const DocumentMatchMethod method = KOREADER_STORE.getMatchMethod();
  out.hash = method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(epubPath)
                                                     : KOReaderDocumentId::calculate(epubPath);
  if (out.hash.empty()) return false;

  // Same metadata-only load the interactive sync uses for progress mapping.
  out.epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  out.epub->setupCacheDir();
  if (!out.epub->load(false, true)) {
    out.epub.reset();
    return false;
  }

  StoredProgress progress;
  if (readStoredProgress(*out.epub, progress)) {
    out.spine = progress.spine;
    out.page = progress.page;
    out.pageCount = progress.pageCount;
  }
  if (out.spine < 0 || out.spine >= out.epub->getSpineItemsCount()) out.spine = 0;

  CrossPointPosition pos = {out.spine, out.page, out.pageCount};
  if (progress.hasOffset) {
    pos.visibleTextOffset = progress.offset;
    pos.hasVisibleTextOffset = true;
  }
  out.ko = ProgressMapper::toSavedProgress(out.epub, pos);
  out.koValid = !out.ko.xpath.empty();
  return out.koValid;
}

// Server timestamp of the last remote record this reader has already reconciled, per book hash. It tells "somebody
// wrote since I last synced" (newest wins, even backwards) apart from "my own copy is just further along".
constexpr char SEEN_TS_PATH[] = "/.crosspoint/kosync_seen_ts.txt";

int64_t loadSeenTs(const std::string& hash) {
  HalFile file;
  if (!Storage.openFileForRead("KOSYNCQ", SEEN_TS_PATH, file)) return 0;
  char buf[96] = {};
  file.read(buf, sizeof(buf) - 1);
  file.close();
  char* space = strchr(buf, ' ');
  if (!space) return 0;
  *space = '\0';
  return hash == buf ? atoll(space + 1) : 0;
}

void saveSeenTs(const std::string& hash, const int64_t ts) {
  if (ts <= 0) return;
  HalFile file;
  if (!Storage.openFileForWrite("KOSYNCQ", SEEN_TS_PATH, file)) return;
  char buf[96];
  const int n = snprintf(buf, sizeof(buf), "%s %lld", hash.c_str(), static_cast<long long>(ts));
  file.write(reinterpret_cast<const uint8_t*>(buf), n);
  file.close();
}

uint32_t positionKey(const Local& l) {
  return (static_cast<uint32_t>(std::max(0, l.spine)) << 16) | static_cast<uint32_t>(std::max(0, l.page));
}

KoSyncQuiet::Result upload(const std::string& epubPath, const Local& l) {
  KOReaderProgress progress;
  progress.document = l.hash;
  progress.progress = l.ko.xpath;
  progress.percentage = l.ko.percentage;
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition pos;
    const float pct = std::min(1.0f, std::max(0.0f, l.ko.percentage));
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(l.spine);
    pos.pageNumber = static_cast<uint16_t>(l.page);
    pos.totalPages = static_cast<uint16_t>(l.pageCount);
    pos.xpath = l.ko.xpath;
    progress.position = std::move(pos);
  }
  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    const auto slash = epubPath.rfind('/');
    meta.filename = slash != std::string::npos ? epubPath.substr(slash + 1) : epubPath;
    meta.title = l.epub->getTitle();
    meta.authors = l.epub->getAuthor();
    progress.metadata = std::move(meta);
  }
  const auto err = KOReaderSyncClient::updateProgress(progress);
  if (err != KOReaderSyncClient::OK) {
    LOG_ERR(TAG, "upload failed: %s", KOReaderSyncClient::errorString(err));
    return KoSyncQuiet::Result::FAILED;
  }
  // Remember the server's stamp for what was just written, so our own record never looks like a newer remote one.
  KOReaderProgress stamped;
  if (KOReaderSyncClient::getProgress(l.hash, stamped) == KOReaderSyncClient::OK) saveSeenTs(l.hash, stamped.timestamp);
  KO_SYNC_STATUS.markSynced(epubPath, positionKey(l));
  return KoSyncQuiet::Result::PUSHED;
}
}  // namespace

namespace {
// Write the server's position into the book's progress file and record it as both the current and pushed position.
KoSyncQuiet::Result applyRemote(const std::string& epubPath, const Local& local, const KOReaderProgress& remote) {
  const SavedProgressPosition koPos = {remote.progress, remote.percentage};
  // Headless: no screen is drawn on this path, so the renderer only serves cached-section lookups.
  const CrossPointPosition target =
      ProgressMapper::toCrossPoint(local.epub, koPos, renderer, local.spine, local.pageCount);
  if (target.totalPages <= 0) return KoSyncQuiet::Result::FAILED;  // book unreadable / empty

  const int pageCount = std::max(target.totalPages, target.pageNumber + 1);
  const std::optional<uint32_t> offset =
      target.hasVisibleTextOffset ? std::optional<uint32_t>(target.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*local.epub, target.spineIndex, target.pageNumber, pageCount, offset)) {
    return KoSyncQuiet::Result::FAILED;
  }
  const uint32_t key = (static_cast<uint32_t>(std::max(0, target.spineIndex)) << 16) |
                       static_cast<uint32_t>(std::max(0, target.pageNumber));
  // A redrawn sleep screen reads the persisted position, so the pulled page must be on SD, not just in memory.
  KO_SYNC_STATUS.notePosition(epubPath, key);
  KO_SYNC_STATUS.persistPosition();
  KO_SYNC_STATUS.markSynced(epubPath, key);
  saveSeenTs(local.hash, remote.timestamp);
  LOG_INF(TAG, "pull: moved to spine %d page %d (%.1f%%)", target.spineIndex, target.pageNumber,
          remote.percentage * 100.0f);
  return KoSyncQuiet::Result::PULLED;
}
}  // namespace

namespace KoSyncQuiet {

Result syncBoth(const std::string& epubPath, const bool allowPush, const bool allowPull) {
  if (!usable(epubPath)) return Result::SKIPPED;
  Local local;
  if (!loadLocal(epubPath, local)) return Result::FAILED;

  KOReaderProgress remote;
  const auto got = KOReaderSyncClient::getProgress(local.hash, remote);
  if (got == KOReaderSyncClient::NOT_FOUND) return allowPush ? upload(epubPath, local) : Result::IN_SYNC;
  if (got != KOReaderSyncClient::OK) {
    LOG_ERR(TAG, "fetch remote failed: %s", KOReaderSyncClient::errorString(got));
    return Result::FAILED;
  }

  const float delta = local.ko.percentage - remote.percentage;
  // Newest write wins, in either direction: if only the server changed since the last sync, follow it even when
  // that means going back (re-listening to a chapter); if only this reader changed, push even when behind.
  const int64_t seenTs = loadSeenTs(local.hash);
  const bool remoteNewer = seenTs > 0 && remote.timestamp > seenTs;
  const bool localChanged = !KO_SYNC_STATUS.isSynced(epubPath);
  if (std::fabs(delta) > SAME_PROGRESS_EPSILON) {
    if (remoteNewer && !localChanged) return allowPull ? applyRemote(epubPath, local, remote) : Result::IN_SYNC;
    if (!remoteNewer && localChanged && seenTs > 0) return allowPush ? upload(epubPath, local) : Result::IN_SYNC;
  }
  if (delta > SAME_PROGRESS_EPSILON) return allowPush ? upload(epubPath, local) : Result::IN_SYNC;
  if (delta < -SAME_PROGRESS_EPSILON) return allowPull ? applyRemote(epubPath, local, remote) : Result::IN_SYNC;
  // Equal: what the server holds is what is on the reader.
  saveSeenTs(local.hash, remote.timestamp);
  KO_SYNC_STATUS.markSynced(epubPath, positionKey(local));
  return Result::IN_SYNC;
}

Result pushIfAhead(const std::string& epubPath) { return syncBoth(epubPath, true, false); }

Result pullIfBehind(const std::string& epubPath) { return syncBoth(epubPath, false, true); }

void pullOnWake() {
  if (!SETTINGS.koSyncPullOnWake || !KOREADER_STORE.hasCredentials()) return;
  const std::string& path = APP_STATE.openEpubPath;
  if (path.empty() || !FsHelpers::hasEpubExtension(path) || !Storage.exists(path.c_str())) return;

  const bool alreadyConnected = hasActiveStationWifiConnection();
  if (!alreadyConnected && !TesseraeClient::joinSavedWifi()) {
    TesseraeClient::leaveWifi();
    wakePullFailed = true;
    return;
  }
  const Result r = pullIfBehind(path);
  if (!alreadyConnected) TesseraeClient::leaveWifi();
  if (r == Result::FAILED) wakePullFailed = true;
}

bool consumeWakePullFailure() {
  const bool failed = wakePullFailed;
  wakePullFailed = false;
  return failed;
}

}  // namespace KoSyncQuiet
