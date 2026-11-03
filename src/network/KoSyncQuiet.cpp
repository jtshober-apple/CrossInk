#include "network/KoSyncQuiet.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
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
#include "activities/home/RecentBookProgress.h"
#include "activities/reader/EpubReaderUtils.h"
#include "network/TesseraeClient.h"
#include "network/WifiUtils.h"

namespace {
constexpr const char* TAG = "KOSYNCQ";
constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points, as the interactive sync uses
constexpr uint32_t MIN_FREE_HEAP = 40000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP = 24000;

bool wakePullFailed = false;

struct Local {
  std::shared_ptr<Epub> epub;
  std::string hash;
  PositionCoordinateSpace space = PositionCoordinateSpace::CurrentDocument;
  int spine = 0;
  int page = 0;
  int pageCount = 1;
  KOReaderPosition ko;
};

bool heapOk() { return ESP.getFreeHeap() >= MIN_FREE_HEAP && ESP.getMaxAllocHeap() >= MIN_MAX_ALLOC_HEAP; }

bool usable(const std::string& epubPath) {
  return !epubPath.empty() && KOREADER_STORE.hasCredentials() && hasActiveStationWifiConnection() && heapOk();
}

bool loadLocal(const std::string& epubPath, Local& out) {
  const DocumentMatchMethod method = KOREADER_STORE.getMatchMethod();
  out.hash = method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(epubPath)
                                                     : KOReaderDocumentId::calculate(epubPath);
  if (out.hash.empty()) return false;
  out.space = method == DocumentMatchMethod::FILENAME ? PositionCoordinateSpace::SourceDocument
                                                      : PositionCoordinateSpace::CurrentDocument;

  // Same metadata-only load the interactive sync uses for progress mapping.
  out.epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  out.epub->setupCacheDir();
  if (!out.epub->load(false, true, Epub::XLocationLoadMode::Immediate, true)) {
    out.epub.reset();
    return false;
  }

  EpubReaderUtils::Progress progress;
  if (EpubReaderUtils::loadProgress(*out.epub, progress, "KOSYNCQ")) {
    out.spine = progress.spineIndex;
    out.page = progress.pageNumber;
    if (progress.hasPageCount) out.pageCount = std::max(1, progress.pageCount);
  }
  if (out.spine < 0 || out.spine >= out.epub->getSpineItemsCount()) out.spine = 0;

  CrossPointPosition pos = {out.spine, out.page, out.pageCount};
  if (progress.hasVisibleTextOffset) {
    pos.visibleTextOffset = progress.visibleTextOffset;
    pos.hasVisibleTextOffset = true;
  }
  out.ko = ProgressMapper::toKOReader(out.epub, pos, out.space);
  return out.ko.valid;
}

// Server timestamp of the last remote record this reader has already reconciled, per book hash. It tells "somebody
// wrote since I last synced" (newest wins, even backwards) apart from "my own copy is just further along".
constexpr char SEEN_TS_PATH[] = "/.crosspoint/kosync_seen_ts.txt";

int64_t loadSeenTs(const std::string& hash) {
  FsFile file;
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
  FsFile file;
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
  progress.device = SETTINGS.getEffectiveDeviceName();
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
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
    LOG_ERR(TAG, "upload failed: %s", KOReaderSyncClient::errorString(err).c_str());
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
  const KOReaderPosition koPos = {remote.progress, remote.percentage};
  const CrossPointPosition target =
      ProgressMapper::toCrossPoint(local.epub, koPos, local.spine, local.pageCount, local.space);
  if (!target.valid) return KoSyncQuiet::Result::FAILED;

  const int pageCount = std::max(target.totalPages, target.pageNumber + 1);
  const std::optional<uint32_t> offset =
      target.hasVisibleTextOffset ? std::optional<uint32_t>(target.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*local.epub, target.spineIndex, target.pageNumber, pageCount, offset)) {
    return KoSyncQuiet::Result::FAILED;
  }
  RecentBookProgress::saveCachedEpubPercent(*local.epub, target.spineIndex, target.pageNumber, pageCount);
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
    LOG_ERR(TAG, "fetch remote failed: %s", KOReaderSyncClient::errorString(got).c_str());
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
