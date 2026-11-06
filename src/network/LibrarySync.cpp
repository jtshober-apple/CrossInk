#define LIBRARY_SYNC_NO_CRAWL 1  // 2.0.0 has no OpdsParser/OpdsParserStream: manifest-only catalog
#include "network/LibrarySync.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#ifndef LIBRARY_SYNC_NO_CRAWL
#endif
#include <ZipFile.h>

#ifndef SIMULATOR
#include <WiFi.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/RtcMinutes.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace LibrarySync {

using namespace LibrarySyncCore;

namespace {
constexpr const char* TAG = "LIBSYNC";
constexpr const char* CONFIG_PATH = "/.crosspoint/library_sync.cfg";
constexpr const char* STATE_PATH = "/.crosspoint/library_sync.tsv";
constexpr const char* MANIFEST_NAME = "manifest.tsv";
constexpr size_t BUFFER_SIZE = 2048;
constexpr size_t MAX_LINE = 2048;
constexpr size_t MAX_LOCAL_BOOKS = 3000;
constexpr size_t MAX_QUEUE = 40;
constexpr size_t MAX_FEEDS = 40;

Config cfg;
bool cfgLoaded = false;

// What the card holds, as order-independent name fingerprints (see nameKey).
std::vector<uint32_t> localKeys;
// Books this device already fetched or adopted.
std::vector<StateEntry> state;

struct Pending {
  std::string url;       // resolved against the server URL when fetched
  std::string filename;  // "<name>.epub"
  uint32_t idHash = 0;
  uint32_t size = 0;
  uint32_t sha1Prefix = 0;
  bool update = false;
};

std::string downloadFolder() {
  std::string folder = SETTINGS.opdsDownloadFolder;
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
  if (folder == "/") folder.clear();
  return folder;
}

std::string destPathFor(const std::string& filename) {
  std::string base(stripExtension(filename));
  std::string path = downloadFolder();
  path += '/';
  path += StringUtils::sanitizeFilename(base);
  path += ".epub";
  return path;
}

// ---- config / state files --------------------------------------------------------------------------------------

void writeAtomic(const char* path, const std::string& content) {
  const std::string tmp = std::string(path) + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), String(content.c_str()))) return;
  Storage.remove(path);
  Storage.rename(tmp.c_str(), path);
}

std::string readWhole(const char* path) {
  if (!Storage.exists(path)) return {};
  String s = Storage.readFile(path);
  return std::string(s.c_str());
}

template <typename Fn>
void forEachLine(const std::string& text, Fn&& fn) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    fn(std::string_view(text).substr(pos, end - pos));
    pos = end + 1;
  }
}

void loadState() {
  state.clear();
  forEachLine(readWhole(STATE_PATH), [](std::string_view line) {
    StateEntry e;
    if (!parseStateLine(line, e)) return;
    for (auto& existing : state) {
      if (existing.idHash == e.idHash) {
        existing = e;
        return;
      }
    }
    state.push_back(e);
  });
}

void saveState() {
  std::string out;
  out.reserve(state.size() * 24);
  for (const auto& e : state) out += formatStateLine(e);
  writeAtomic(STATE_PATH, out);
}

StateEntry* findState(const uint32_t idHash) {
  for (auto& e : state) {
    if (e.idHash == idHash) return &e;
  }
  return nullptr;
}

void rememberBook(const uint32_t idHash, const uint32_t size, const uint32_t sha1Prefix) {
  if (auto* existing = findState(idHash)) {
    existing->size = size;
    existing->sha1Prefix = sha1Prefix;
    return;
  }
  state.push_back(StateEntry{idHash, size, sha1Prefix});
}

// ---- the card ---------------------------------------------------------------------------------------------------

void scanFolder(const std::string& path, const int depth) {
  if (localKeys.size() >= MAX_LOCAL_BOOKS) return;
  HalFile dir = Storage.open(path.empty() ? "/" : path.c_str());
  if (!dir || !dir.isDirectory()) return;
  std::vector<std::string> subdirs;
  char name[160];
  while (localKeys.size() < MAX_LOCAL_BOOKS) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDir = entry.isDirectory();
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    entry.close();
    if (name[0] == '\0' || name[0] == '.') continue;
    if (isDir) {
      if (depth > 0 && std::strcmp(name, "System Volume Information") != 0) subdirs.emplace_back(name);
      continue;
    }
    if (hasEpubExtension(name)) {
      const uint32_t key = nameKey(name);
      if (key != 0) localKeys.push_back(key);
    }
  }
  dir.close();
  for (const auto& sub : subdirs) scanFolder(path + "/" + sub, depth - 1);
}

void scanCard() {
  localKeys.clear();
  scanFolder(downloadFolder(), 2);
  if (!downloadFolder().empty()) scanFolder("", 0);  // books in the card root too
  std::sort(localKeys.begin(), localKeys.end());
  localKeys.erase(std::unique(localKeys.begin(), localKeys.end()), localKeys.end());
}

bool onCard(const std::string& filename) {
  const uint32_t key = nameKey(filename);
  return key != 0 && std::binary_search(localKeys.begin(), localKeys.end(), key);
}

// ---- catalog access ---------------------------------------------------------------------------------------------

struct Catalog {
  uint16_t total = 0;
  uint16_t synced = 0;
  uint16_t missing = 0;
  bool viaManifest = false;
};

// Feeds each line of a streamed body to `fn`.
class LineSink {
 public:
  explicit LineSink(std::function<void(std::string_view)> fn) : fn(std::move(fn)) {}
  bool write(const uint8_t* data, const size_t len) {
    for (size_t i = 0; i < len; ++i) {
      const char c = static_cast<char>(data[i]);
      if (c == '\n') {
        if (!overflow) fn(buffer);
        buffer.clear();
        overflow = false;
      } else if (buffer.size() < MAX_LINE) {
        buffer += c;
      } else {
        overflow = true;
      }
    }
    return true;
  }
  void finish() {
    if (!buffer.empty() && !overflow) fn(buffer);
    buffer.clear();
  }

 private:
  std::function<void(std::string_view)> fn;
  std::string buffer;
  bool overflow = false;
};

// ---- platform layer -------------------------------------------------------------------------------------------
// The only code that touches HttpDownloader and the library index. Porting Library Sync to another base firmware
// means adapting just these few functions (and, if there is no OpdsParser, building with LIBRARY_SYNC_NO_CRAWL).

enum class Net : uint8_t { OK, ABORTED, FAILED, NO_SPACE };
using Cancel = std::function<bool()>;

// GET `url`, streaming the body to onData (no buffering). `cancel` is polled while the body arrives.
Net netStream(const OpdsServer& server, const std::string& origin, const std::string& url,
              const std::function<bool(const uint8_t*, size_t)>& onData, const Cancel& cancel) {
  (void)origin;  // 2.0.0 withholds credentials from other origins by itself
  HttpDownloader::FetchOptions options;
  options.username = server.username;
  options.password = server.password;
  bool cancelled = false;
  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&](const uint8_t* data, size_t len) {
        if (cancel && cancel()) {
          cancelled = true;
          return false;
        }
        return onData(data, len);
      },
      options);
  if (ok) return Net::OK;
  return cancelled ? Net::ABORTED : Net::FAILED;
}

bool validEpub(const std::string& path);

// Download to `<dest>.dl` (2.0.0 stages that in `.part` itself), check it is a real epub, then publish it over `dest`
// (an existing `dest` is kept as `.old`). Never leaves a truncated book behind. A cut-off download restarts.
Net netDownload(const OpdsServer& server, const std::string& origin, const std::string& url, const std::string& dest,
                const Cancel& cancel, const std::function<void(size_t, size_t)>& progress) {
  (void)origin;
  const std::string tmp = dest + ".dl";
  bool cancelFlag = false;
  const auto result = HttpDownloader::downloadToFile(
      url, tmp,
      [&](size_t done, size_t total) {
        if (cancel && cancel()) cancelFlag = true;
        if (progress) progress(done, total);
      },
      &cancelFlag, server.username, server.password);
  if (result == HttpDownloader::ABORTED) return Net::ABORTED;
  if (result != HttpDownloader::OK) return result == HttpDownloader::FILE_ERROR ? Net::NO_SPACE : Net::FAILED;
  if (!validEpub(tmp)) {
    Storage.remove(tmp.c_str());
    return Net::FAILED;
  }
  if (Storage.exists(dest.c_str())) {
    const std::string old = dest + ".old";
    Storage.remove(old.c_str());
    if (!Storage.rename(dest.c_str(), old.c_str())) {
      Storage.remove(tmp.c_str());
      return Net::FAILED;
    }
  }
  if (!Storage.rename(tmp.c_str(), dest.c_str())) {
    Storage.remove(tmp.c_str());
    return Net::FAILED;
  }
  return Net::OK;
}

void libraryChanged() { library::markLibraryIndexDirty(); }           // new books: the Library rescans next time
void bookReplaced(const std::string& path) { clearBookCache(path); }  // layout caches belong to the old file

struct Pass {
  const OpdsServer* server = nullptr;
  std::string origin;
  bool replaceUpdated = false;
  size_t queueLimit = 0;
  std::vector<Pending> queue;
  Catalog catalog;
  const AbortFn* abort = nullptr;
};

void consider(Pass& pass, const ManifestLine& line) {
  ++pass.catalog.total;
  const uint32_t idHash = fnv1a(line.id);
  const StateEntry* tracked = findState(idHash);
  const bool present = onCard(line.filename);
  const Decision decision = decide(line, tracked, present, pass.replaceUpdated);
  switch (decision) {
    case Decision::UP_TO_DATE:
      ++pass.catalog.synced;
      break;
    case Decision::ADOPT:
      rememberBook(idHash, line.size, sha1Prefix32(line.sha1));
      ++pass.catalog.synced;
      break;
    case Decision::DOWNLOAD_NEW:
    case Decision::DOWNLOAD_UPDATE:
      ++pass.catalog.missing;
      if (pass.queue.size() < pass.queueLimit) {
        Pending p;
        p.url = line.path;
        p.filename = line.filename;
        p.idHash = idHash;
        p.size = line.size;
        p.sha1Prefix = sha1Prefix32(line.sha1);
        p.update = decision == Decision::DOWNLOAD_UPDATE;
        pass.queue.push_back(std::move(p));
      }
      break;
  }
}

bool readManifest(Pass& pass) {
  const std::string url = UrlUtils::buildUrl(pass.server->url, MANIFEST_NAME);
  bool sawBook = false;
  LineSink sink([&](std::string_view line) {
    ManifestLine parsed;
    if (!parseManifestLine(line, parsed)) return;
    sawBook = true;
    consider(pass, parsed);
  });
  const auto result = netStream(
      *pass.server, pass.origin, url, [&sink](const uint8_t* data, const size_t len) { return sink.write(data, len); },
      [&pass] { return (*pass.abort)(); });
  sink.finish();
  return result == Net::OK && sawBook;
}

#ifndef LIBRARY_SYNC_NO_CRAWL
std::string filenameFor(const OpdsEntry& book, const OpdsFilenameFormat format) {
  std::string base;
  if (book.author.empty()) {
    base = book.title;
  } else if (book.title.empty()) {
    base = book.author;
  } else if (format == OpdsFilenameFormat::TITLE_AUTHOR) {
    base = book.title + " - " + book.author;
  } else {
    base = book.author + " - " + book.title;
  }
  return base + ".epub";
}

// Fallback for a server without a manifest: walk the OPDS feeds. Finds new books only.
bool crawlFeeds(Pass& pass) {
  std::unique_ptr<OpdsEntry[]> entries(new (std::nothrow) OpdsEntry[MAX_OPDS_FEED_ENTRIES]);
  if (!entries) return false;
  std::vector<std::string> feeds{""};
  bool sawBook = false;
  for (size_t i = 0; i < feeds.size() && i < MAX_FEEDS; ++i) {
    if (pass.abort && (*pass.abort)()) return false;
    const std::string feedUrl = UrlUtils::buildUrl(pass.server->url, feeds[i]);
    OpdsParser parser(entries.get(), MAX_OPDS_FEED_ENTRIES);
    {
      OpdsParserStream stream{parser};
      const auto result = netStream(
          *pass.server, pass.origin, feedUrl,
          [&stream](const uint8_t* data, const size_t len) { return stream.write(data, len) == len; },
          [&pass] { return (*pass.abort)(); });
      if (result != Net::OK) {
        if (i == 0) return false;
        continue;
      }
    }
    if (!parser) continue;
    for (size_t e = 0; e < parser.getEntryCount(); ++e) {
      const OpdsEntry& entry = entries[e];
      if (entry.type == OpdsEntryType::BOOK) {
        ManifestLine line;
        line.id = entry.id.empty() ? entry.href : entry.id;
        line.path = UrlUtils::buildUrl(feedUrl, entry.href);
        line.filename = filenameFor(entry, pass.server->filenameFormat);
        sawBook = true;
        consider(pass, line);
      } else if (!entry.href.empty()) {
        const std::string next = entry.href;
        if (std::find(feeds.begin(), feeds.end(), next) == feeds.end() && feeds.size() < MAX_FEEDS) {
          feeds.push_back(next);
        }
      }
    }
    const std::string& nextPage = parser.getNextPageUrl();
    if (!nextPage.empty() && std::find(feeds.begin(), feeds.end(), nextPage) == feeds.end() &&
        feeds.size() < MAX_FEEDS) {
      feeds.push_back(nextPage);
    }
  }
  return sawBook;
}
#else
bool crawlFeeds(Pass&) { return false; }  // no OPDS parser in this build: manifest servers only
#endif

bool validEpub(const std::string& path) {
  ZipFile zip(path);
  size_t size = 0;
  return zip.getInflatedFileSize("META-INF/container.xml", &size) && size > 0;
}

}  // namespace

// ---- config -----------------------------------------------------------------------------------------------------

Config& config() {
  if (!cfgLoaded) loadConfig();
  return cfg;
}

void loadConfig() {
  cfg = Config();
  cfgLoaded = true;
  forEachLine(readWhole(CONFIG_PATH), [](std::string_view line) {
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) return;
    const std::string key(line.substr(0, eq));
    std::string value(line.substr(eq + 1));
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    const long number = std::strtol(value.c_str(), nullptr, 10);
    if (key == "schedule") {
      if (number >= 0 && number < SCHEDULE_COUNT) cfg.schedule = static_cast<Schedule>(number);
    } else if (key == "homeOnly") {
      cfg.homeOnly = number != 0;
    } else if (key == "batteryFloor") {
      cfg.batteryFloor = static_cast<uint8_t>(std::min(std::max(number, 0L), 90L));
    } else if (key == "replaceUpdated") {
      cfg.replaceUpdated = number != 0;
    } else if (key == "serverIndex") {
      cfg.serverIndex = static_cast<uint8_t>(std::min(std::max(number, 0L), 7L));
    } else if (key == "homeSsid") {
      std::strncpy(cfg.homeSsid, value.c_str(), sizeof(cfg.homeSsid) - 1);
      cfg.homeSsid[sizeof(cfg.homeSsid) - 1] = '\0';
    } else if (key == "lastCheckMin") {
      cfg.lastCheckMin = std::strtoll(value.c_str(), nullptr, 10);
    } else if (key == "backlog") {
      cfg.backlog = number != 0;
    } else if (key == "lastTotal") {
      cfg.lastTotal = static_cast<uint16_t>(std::min(std::max(number, 0L), 65535L));
    } else if (key == "lastSynced") {
      cfg.lastSynced = static_cast<uint16_t>(std::min(std::max(number, 0L), 65535L));
    }
  });
}

bool saveConfig() {
  if (!cfgLoaded) loadConfig();
  Storage.mkdir("/.crosspoint");
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "schedule=%d\nhomeOnly=%d\nbatteryFloor=%d\nreplaceUpdated=%d\nserverIndex=%d\nhomeSsid=%s\n"
                "lastCheckMin=%lld\nbacklog=%d\nlastTotal=%u\nlastSynced=%u\n",
                static_cast<int>(cfg.schedule), cfg.homeOnly ? 1 : 0, cfg.batteryFloor, cfg.replaceUpdated ? 1 : 0,
                cfg.serverIndex, cfg.homeSsid, static_cast<long long>(cfg.lastCheckMin), cfg.backlog ? 1 : 0,
                static_cast<unsigned>(cfg.lastTotal), static_cast<unsigned>(cfg.lastSynced));
  writeAtomic(CONFIG_PATH, buf);
  return true;
}

const char* resultString(const Result result) {
  switch (result) {
    case Result::OK:
      return "Library complete";
    case Result::PARTIAL:
      return "More books to fetch";
    case Result::NO_SERVER:
      return "No OPDS server saved";
    case Result::NETWORK:
      return "Could not reach the server";
    case Result::ABORTED:
      return "Cancelled";
    case Result::SKIPPED:
      return "Skipped";
  }
  return "";
}

ServerInfo selectedServer() {
  ServerInfo info;
  const OpdsServer* server = OPDS_STORE.getServer(config().serverIndex);
  if (!server) server = OPDS_STORE.getServer(0);
  if (!server) return info;
  info.found = true;
  info.name = server->name.empty() ? server->url : server->name;
  return info;
}

// ---- the sync itself --------------------------------------------------------------------------------------------

Result run(const bool manual, const AbortFn& abortFn, const ProgressFn& progress, Progress& out) {
  // Callers may pass no abort check at all.
  const AbortFn abort = [&abortFn] { return abortFn && abortFn(); };
  Config& c = config();
  const OpdsServer* server = OPDS_STORE.getServer(c.serverIndex);
  if (!server) server = OPDS_STORE.getServer(0);
  if (!server || server->url.empty()) return Result::NO_SERVER;

  const bool charging = gpio.isUsbConnected();
  const Budget budget = budgetFor(manual, charging);
  const uint32_t startMs = millis();
  auto outOfTime = [&] { return millis() - startMs >= budget.maxMs; };

  loadState();
  scanCard();
  const std::string folder = downloadFolder();
  if (!folder.empty() && !Storage.exists(folder.c_str()) && !Storage.mkdir(folder.c_str())) {
    LOG_ERR(TAG, "cannot create %s", folder.c_str());
    return Result::NETWORK;
  }

  const std::string origin = UrlUtils::ensureProtocol(server->url);
  out = Progress();
  uint16_t fetchedTotal = 0;
  uint16_t deferred = 0;  // updates held back because the book is open: not a reason to keep waking
  Result result = Result::OK;
  bool changedCard = false;
  bool firstPass = true;

  while (true) {
    Pass pass;
    pass.server = server;
    pass.origin = origin;
    pass.replaceUpdated = c.replaceUpdated;
    pass.queueLimit = std::min<size_t>(MAX_QUEUE, budget.maxBooks > fetchedTotal ? budget.maxBooks - fetchedTotal : 0);
    pass.abort = &abort;
    if (pass.queueLimit == 0) pass.queueLimit = 1;  // still count what is missing

    bool ok = readManifest(pass);
    pass.catalog.viaManifest = ok;
    if (!ok && !abort()) {
      // A server without a manifest: crawl its feeds (new books only). Start the count over.
      pass.catalog = Catalog();
      pass.queue.clear();
      ok = crawlFeeds(pass);
    }
    if (abort()) {
      result = Result::ABORTED;
      break;
    }
    if (!ok) {
      result = firstPass ? Result::NETWORK : Result::PARTIAL;
      break;
    }
    firstPass = false;

    out.total = pass.catalog.total;
    out.synced = pass.catalog.synced;
    out.missing = pass.catalog.missing;
    if (progress) progress(out);
    saveState();  // adoptions found while reading the catalog
    if (pass.queue.empty()) {
      result = pass.catalog.missing == 0 ? Result::OK : Result::PARTIAL;
      break;
    }

    // Fetch this batch.
    const uint16_t fetchedBefore = fetchedTotal;
    out.batch = static_cast<uint16_t>(pass.queue.size());
    bool stop = false;
    for (size_t i = 0; i < pass.queue.size() && !stop; ++i) {
      const Pending& book = pass.queue[i];
      if (abort()) {
        result = Result::ABORTED;
        stop = true;
        break;
      }
      if (outOfTime() || fetchedTotal >= budget.maxBooks) {
        result = Result::PARTIAL;
        stop = true;
        break;
      }
      const std::string dest = destPathFor(book.filename);
      out.position = static_cast<uint16_t>(i + 1);
      out.title = book.filename;
      out.bytes = 0;
      out.bytesTotal = book.size;
      if (progress) progress(out);

      if (book.update) {
        // Never swap a book out from under the reader: skip it until it is no longer the open one.
        if (!Storage.exists(dest.c_str())) {
          rememberBook(book.idHash, book.size, book.sha1Prefix);  // renamed or moved by the user: new baseline
          continue;
        }
        if (dest == APP_STATE.openEpubPath) {
          ++deferred;
          continue;
        }
      } else if (Storage.exists(dest.c_str())) {
        rememberBook(book.idHash, book.size, book.sha1Prefix);  // never overwrite what is on the card
        ++out.synced;
        continue;
      }

      const std::string url = UrlUtils::buildUrl(server->url, book.url);
      const Net download = netDownload(
          *server, origin, url, dest, [&] { return abort() || outOfTime(); },
          [&](const size_t done, const size_t total) {
            out.bytes = static_cast<uint32_t>(done);
            out.bytesTotal = static_cast<uint32_t>(total);
            if (progress) progress(out);
          });

      if (download == Net::OK) {
        rememberBook(book.idHash, book.size, book.sha1Prefix);
        saveState();
        if (book.update) bookReplaced(dest);
        ++out.fetched;
        ++fetchedTotal;
        if (!book.update) ++out.synced;
        if (out.missing > 0) --out.missing;
        changedCard = true;
        LOG_INF(TAG, "fetched %s", book.filename.c_str());
      } else if (download == Net::ABORTED) {
        result = abort() ? Result::ABORTED : Result::PARTIAL;  // a half-finished .part resumes on the next pass
        stop = true;
      } else {
        ++out.failed;
        LOG_ERR(TAG, "failed %s (%d)", book.filename.c_str(), static_cast<int>(download));
        if (download == Net::NO_SPACE) {
          result = Result::PARTIAL;
          stop = true;
        }
      }
    }
    if (stop) break;
    if (fetchedTotal >= budget.maxBooks || outOfTime()) {
      result = out.missing <= deferred ? Result::OK : Result::PARTIAL;
      break;
    }
    if (fetchedTotal == fetchedBefore) {
      result = out.missing <= deferred ? Result::OK : Result::PARTIAL;  // no progress: stop rather than loop
      break;
    }
    // Another batch? Re-read the catalog (cheap) so the next queue starts where this one ended.
    if (out.missing <= deferred) {
      result = Result::OK;
      break;
    }
  }

  saveState();
  if (changedCard) libraryChanged();
  if (result == Result::OK || result == Result::PARTIAL) {
    int64_t now = 0;
    if (rtcMinutesSinceEpoch(now)) c.lastCheckMin = now;
    c.backlog = out.missing > deferred;
    c.lastTotal = out.total;
    c.lastSynced = out.synced;
#ifndef SIMULATOR
    if (manual && c.homeOnly && c.homeSsid[0] == '\0' && WiFi.status() == WL_CONNECTED) {
      const String ssid = WiFi.SSID();
      std::strncpy(c.homeSsid, ssid.c_str(), sizeof(c.homeSsid) - 1);
      c.homeSsid[sizeof(c.homeSsid) - 1] = '\0';
    }
#endif
    saveConfig();
  }
  return result;
}

bool backgroundDue() {
  const Config& c = config();
  if (c.schedule == Schedule::MANUAL) return false;
  if (!OPDS_STORE.hasServers()) return false;
  const bool charging = gpio.isUsbConnected();
  if (!charging && powerManager.getBatteryPercentage() < c.batteryFloor) return false;
  int64_t now = -1;
  int64_t tmp = 0;
  if (rtcMinutesSinceEpoch(tmp)) now = tmp;
  return dueNow(c.schedule, now, c.lastCheckMin, charging, c.backlog);
}

std::string preferredJoinSsid() {
  const Config& c = config();
  if (c.schedule == Schedule::MANUAL || !c.homeOnly) return "";
  return c.homeSsid;
}

Result runBackground(const AbortFn& abort) {
  if (!backgroundDue()) return Result::SKIPPED;
  Config& c = config();
#ifndef SIMULATOR
  if (c.homeOnly && c.homeSsid[0] != '\0') {
    const String ssid = WiFi.SSID();
    if (std::strcmp(ssid.c_str(), c.homeSsid) != 0) {
      LOG_INF(TAG, "not on the home network, skipping");
      return Result::SKIPPED;
    }
  }
#endif
  Progress progress;
  const Result result = run(/*manual=*/false, abort, nullptr, progress);
  LOG_INF(TAG, "background pass: %s (fetched=%u failed=%u missing=%u)", resultString(result),
          static_cast<unsigned>(progress.fetched), static_cast<unsigned>(progress.failed),
          static_cast<unsigned>(progress.missing));
  return result;
}

}  // namespace LibrarySync
