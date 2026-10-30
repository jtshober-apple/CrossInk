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

void syncClockForTls() {
#ifndef SIMULATOR
  halClock.syncSystemTimeFromNTP();
#endif
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
  KO_SYNC_STATUS.markSynced(epubPath, positionKey(l));
  return KoSyncQuiet::Result::PUSHED;
}
}  // namespace

namespace KoSyncQuiet {

Result pushIfAhead(const std::string& epubPath) {
  if (!usable(epubPath)) return Result::SKIPPED;
  syncClockForTls();
  Local local;
  if (!loadLocal(epubPath, local)) return Result::FAILED;

  KOReaderProgress remote;
  const auto got = KOReaderSyncClient::getProgress(local.hash, remote);
  if (got == KOReaderSyncClient::NOT_FOUND) return upload(epubPath, local);
  if (got != KOReaderSyncClient::OK) {
    LOG_ERR(TAG, "push: fetch remote failed: %s", KOReaderSyncClient::errorString(got).c_str());
    return Result::FAILED;
  }
  if (local.ko.percentage - remote.percentage > SAME_PROGRESS_EPSILON) return upload(epubPath, local);
  // Equal, or the server is ahead (a pull on wake handles that): the position the server holds counts as synced.
  if (std::fabs(local.ko.percentage - remote.percentage) <= SAME_PROGRESS_EPSILON) {
    KO_SYNC_STATUS.markSynced(epubPath, positionKey(local));
  }
  return Result::IN_SYNC;
}

Result pullIfBehind(const std::string& epubPath) {
  if (!usable(epubPath)) return Result::SKIPPED;
  syncClockForTls();
  Local local;
  if (!loadLocal(epubPath, local)) return Result::FAILED;

  KOReaderProgress remote;
  const auto got = KOReaderSyncClient::getProgress(local.hash, remote);
  if (got == KOReaderSyncClient::NOT_FOUND) return Result::IN_SYNC;
  if (got != KOReaderSyncClient::OK) {
    LOG_ERR(TAG, "pull: fetch remote failed: %s", KOReaderSyncClient::errorString(got).c_str());
    return Result::FAILED;
  }
  if (remote.percentage - local.ko.percentage <= SAME_PROGRESS_EPSILON) return Result::IN_SYNC;

  const KOReaderPosition koPos = {remote.progress, remote.percentage};
  const CrossPointPosition target =
      ProgressMapper::toCrossPoint(local.epub, koPos, local.spine, local.pageCount, local.space);
  if (!target.valid) return Result::FAILED;

  const int pageCount = std::max(target.totalPages, target.pageNumber + 1);
  const std::optional<uint32_t> offset =
      target.hasVisibleTextOffset ? std::optional<uint32_t>(target.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*local.epub, target.spineIndex, target.pageNumber, pageCount, offset)) {
    return Result::FAILED;
  }
  RecentBookProgress::saveCachedEpubPercent(*local.epub, target.spineIndex, target.pageNumber, pageCount);
  KO_SYNC_STATUS.markSynced(epubPath, (static_cast<uint32_t>(std::max(0, target.spineIndex)) << 16) |
                                          static_cast<uint32_t>(std::max(0, target.pageNumber)));
  LOG_INF(TAG, "pull: moved to spine %d page %d (%.1f%%)", target.spineIndex, target.pageNumber,
          remote.percentage * 100.0f);
  return Result::PULLED;
}

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
