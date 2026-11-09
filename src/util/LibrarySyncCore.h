#pragma once

// Pure logic for Library Sync (no firmware dependencies, so it is unit-tested on the host): the server's compact
// manifest, the on-device state file, and the decision about what to fetch.
//
// Manifest (served by the OPDS host as <server url>/manifest.tsv), one book per line, tab separated:
//   id \t size \t mtime \t sha1 \t path \t filename
// plus an optional first line "#manifest v1 <count> <generated-epoch>". `path` is a percent-encoded URL path
// relative to the server URL; `filename` is the on-card name (".epub" included). `sha1` may be empty.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace LibrarySyncCore {

enum class Schedule : uint8_t {
  MANUAL = 0,       // only "Sync now"
  EVERY_WAKE = 1,   // every background wake (30 min by default)
  DAILY = 2,        // check once a day; keep going on later wakes until caught up
  CHARGING = 3,     // only while USB power is connected
  TWICE_DAILY = 4,  // about every 12 h from the last completed pass; keep going on later wakes until caught up
};
constexpr uint8_t SCHEDULE_COUNT = 5;

// The order the Settings row cycles through. Turning automatic sync on (from MANUAL) lands on TWICE_DAILY.
inline Schedule nextSchedule(const Schedule current) {
  switch (current) {
    case Schedule::MANUAL:
      return Schedule::TWICE_DAILY;
    case Schedule::TWICE_DAILY:
      return Schedule::DAILY;
    case Schedule::DAILY:
      return Schedule::EVERY_WAKE;
    case Schedule::EVERY_WAKE:
      return Schedule::CHARGING;
    case Schedule::CHARGING:
      return Schedule::MANUAL;
  }
  return Schedule::MANUAL;
}

struct ManifestLine {
  std::string id;
  uint32_t size = 0;
  uint64_t mtime = 0;
  std::string sha1;  // lowercase hex, may be empty
  std::string path;
  std::string filename;
};

struct StateEntry {
  uint32_t idHash = 0;
  uint32_t size = 0;
  uint32_t sha1Prefix = 0;  // first 8 hex digits of the manifest sha1 recorded when the book was fetched/adopted
};

enum class Decision : uint8_t {
  UP_TO_DATE,       // already fetched or adopted; nothing to do
  ADOPT,            // not tracked yet but the card already holds this book: just remember it
  DOWNLOAD_NEW,     // a book the card does not have
  DOWNLOAD_UPDATE,  // tracked, but the server's copy changed (only when updates are enabled)
};

inline uint32_t fnv1a(std::string_view s) {
  uint32_t h = 2166136261u;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

inline uint32_t sha1Prefix32(std::string_view hex) {
  if (hex.size() < 8) return 0;
  uint32_t v = 0;
  for (size_t i = 0; i < 8; ++i) {
    const char c = hex[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return 0;
    }
    v = (v << 4) | d;
  }
  return v;
}

inline std::string_view stripExtension(std::string_view name) {
  if (name.size() > 5) {
    const std::string_view tail = name.substr(name.size() - 5);
    if (tail.size() == 5 && tail[0] == '.' && (tail[1] | 0x20) == 'e' && (tail[2] | 0x20) == 'p' &&
        (tail[3] | 0x20) == 'u' && (tail[4] | 0x20) == 'b') {
      return name.substr(0, name.size() - 5);
    }
  }
  return name;
}

// Order-independent fingerprint of a book's file name: the set of lower-cased words. "Author - Title" and
// "Title - Author" (and sanitised variants such as a colon turned into an underscore) give the same key, so a
// book already on the card under a different naming style is recognised and not fetched a second time.
inline uint32_t nameKey(std::string_view name) {
  name = stripExtension(name);
  uint32_t sum = 0;
  uint32_t words = 0;
  size_t i = 0;
  while (i < name.size()) {
    const auto isWordByte = [](const unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
    };
    while (i < name.size() && !isWordByte(static_cast<unsigned char>(name[i]))) ++i;
    if (i >= name.size()) break;
    uint32_t h = 2166136261u;
    while (i < name.size() && isWordByte(static_cast<unsigned char>(name[i]))) {
      unsigned char c = static_cast<unsigned char>(name[i]);
      if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
      h ^= c;
      h *= 16777619u;
      ++i;
    }
    sum += h * 2654435761u + 0x9e3779b9u;
    ++words;
  }
  if (words == 0) return 0;
  return sum ^ (words * 0x85ebca6bu);
}

inline bool hasEpubExtension(std::string_view name) { return stripExtension(name).size() != name.size(); }

// Splits "a\tb\tc" into at most `max` fields; returns the number found.
inline size_t splitTabs(std::string_view line, std::string_view* out, const size_t max) {
  size_t n = 0;
  size_t start = 0;
  while (n < max) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      out[n++] = line.substr(start);
      break;
    }
    out[n++] = line.substr(start, tab - start);
    start = tab + 1;
  }
  return n;
}

inline uint64_t parseU64(std::string_view s) {
  uint64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') break;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  return v;
}

// Returns false for comments, blank lines and malformed lines.
inline bool parseManifestLine(std::string_view line, ManifestLine& out) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
  if (line.empty() || line.front() == '#') return false;
  std::string_view f[6];
  if (splitTabs(line, f, 6) < 6) return false;
  if (f[0].empty() || f[4].empty() || f[5].empty()) return false;
  if (!hasEpubExtension(f[5])) return false;
  out.id.assign(f[0]);
  out.size = static_cast<uint32_t>(parseU64(f[1]));
  out.mtime = parseU64(f[2]);
  out.sha1.assign(f[3]);
  out.path.assign(f[4]);
  out.filename.assign(f[5]);
  return true;
}

// The state file has one line per tracked book: idhex \t size \t sha1prefixhex. Later lines win.
inline bool parseStateLine(std::string_view line, StateEntry& out) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
  std::string_view f[3];
  if (splitTabs(line, f, 3) < 3) return false;
  const std::string idHex(f[0]);
  const std::string shaHex(f[2]);
  if (idHex.empty()) return false;
  out.idHash = static_cast<uint32_t>(std::strtoul(idHex.c_str(), nullptr, 16));
  out.size = static_cast<uint32_t>(parseU64(f[1]));
  out.sha1Prefix = static_cast<uint32_t>(std::strtoul(shaHex.c_str(), nullptr, 16));
  return out.idHash != 0;
}

inline std::string formatStateLine(const StateEntry& e) {
  char buf[48];
  const int n = std::snprintf(buf, sizeof(buf), "%08x\t%u\t%08x\n", static_cast<unsigned>(e.idHash),
                              static_cast<unsigned>(e.size), static_cast<unsigned>(e.sha1Prefix));
  return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

// What to do with one manifest line. `tracked` is the state entry for this id (null when none).
inline Decision decide(const ManifestLine& line, const StateEntry* tracked, const bool presentOnCard,
                       const bool replaceUpdated) {
  if (tracked) {
    // A book this device fetched earlier and that is no longer on the card (removed by hand) comes back on the next
    // pass: the sync keeps the card a mirror of the server, so a book you do not want must be removed from the server
    // library, not just from the card.
    if (!presentOnCard) return Decision::DOWNLOAD_NEW;
    if (replaceUpdated) {
      const uint32_t serverPrefix = sha1Prefix32(line.sha1);
      // Only a recorded hash AND a published hash that differ count as a real update; sizes alone are never
      // enough (a different size with the same hash cannot happen, and a missing hash must not trigger refetches).
      if (serverPrefix != 0 && tracked->sha1Prefix != 0 && serverPrefix != tracked->sha1Prefix) {
        return Decision::DOWNLOAD_UPDATE;
      }
    }
    return Decision::UP_TO_DATE;
  }
  return presentOnCard ? Decision::ADOPT : Decision::DOWNLOAD_NEW;
}

// Is a background pass due? `nowMin`/`lastCheckMin` are RTC minutes (nowMin < 0: no usable clock).
inline bool dueNow(const Schedule schedule, const int64_t nowMin, const int64_t lastCheckMin, const bool charging,
                   const bool backlog) {
  switch (schedule) {
    case Schedule::MANUAL:
      return false;
    case Schedule::EVERY_WAKE:
      return true;
    case Schedule::TWICE_DAILY:
      if (backlog) return true;
      if (nowMin < 0 || lastCheckMin <= 0) return true;
      return nowMin - lastCheckMin >= 12 * 60;
    case Schedule::DAILY:
      if (backlog) return true;
      if (nowMin < 0 || lastCheckMin <= 0) return true;
      return nowMin - lastCheckMin >= 24 * 60;
    case Schedule::CHARGING:
      if (!charging) return false;
      if (backlog) return true;
      if (nowMin < 0 || lastCheckMin <= 0) return true;
      return nowMin - lastCheckMin >= 60;
  }
  return false;
}

struct Budget {
  uint32_t maxMs = 0;
  uint16_t maxBooks = 0;
};

// Per-wake budget: tiny on battery so it costs little, generous on USB power or for a manual run.
inline Budget budgetFor(const bool manual, const bool charging) {
  if (manual) return Budget{20u * 60u * 1000u, 500};
  if (charging) return Budget{8u * 60u * 1000u, 40};
  return Budget{75u * 1000u, 2};
}

// ---- problem books: deadlines, quarantine, error log -------------------------------------------------------------

// Why a book could not be fetched. Stored as a number in the fail file; the text is what the error log shows.
enum class FailReason : uint8_t {
  NONE = 0,
  NO_START = 1,      // connect + headers + first byte did not arrive in time
  STALLED = 2,       // the download started and then went quiet
  HTTP_ERROR = 3,    // the server answered with an error (or cut the body short)
  NOT_EPUB = 4,      // arrived, but is not a readable epub (damaged file)
  WRITE_ERROR = 5,   // the SD card could not take it (full, or a write failed)
  NO_MEMORY = 6,     // not enough free memory for a secure connection
  TOO_BIG = 7,       // would not finish inside a background pass: left for 'Sync library now'
  UNAUTHORIZED = 8,  // the server refused the saved user name / password
};

inline const char* failReasonText(const FailReason r) {
  switch (r) {
    case FailReason::NO_START:
      return "no data within 25 s";
    case FailReason::STALLED:
      return "download stalled (no data for 30 s)";
    case FailReason::HTTP_ERROR:
      return "server error or cut-off download";
    case FailReason::NOT_EPUB:
      return "not a readable epub (damaged on the server?)";
    case FailReason::WRITE_ERROR:
      return "SD card write failed or card full";
    case FailReason::NO_MEMORY:
      return "not enough free memory";
    case FailReason::TOO_BIG:
      return "too big for a background pass, use Sync library now";
    case FailReason::UNAUTHORIZED:
      return "server refused the password";
    case FailReason::NONE:
      break;
  }
  return "unknown problem";
}

// Per-book deadlines: no byte at all inside FIRST_BYTE_MS of starting (connect, TLS, request, headers and the first
// body byte count), or silence for STALL_MS once bytes have flowed. Wrap-safe on millis().
constexpr uint32_t FIRST_BYTE_MS = 25000;
constexpr uint32_t STALL_MS = 30000;

enum class Watch : uint8_t { OK, NO_START, STALLED };

inline Watch watchdog(const uint32_t nowMs, const uint32_t startMs, const uint32_t lastByteMs, const bool gotByte,
                      const uint32_t firstByteLimit = FIRST_BYTE_MS, const uint32_t stallLimit = STALL_MS) {
  if (!gotByte) return (nowMs - startMs) >= firstByteLimit ? Watch::NO_START : Watch::OK;
  return (nowMs - lastByteMs) >= stallLimit ? Watch::STALLED : Watch::OK;
}

// One line per book that has failed (or was too big): `idHash(hex) \t failures \t nextRetryMin \t reason \t size`.
struct FailEntry {
  uint32_t idHash = 0;
  uint8_t count = 0;         // consecutive failures (TOO_BIG does not count)
  int64_t nextRetryMin = 0;  // RTC minutes; 0 = no wait recorded
  FailReason reason = FailReason::NONE;
  uint32_t size = 0;
};

constexpr uint8_t QUARANTINE_AFTER = 3;

// Back-off before a failed book is tried again: 30 min, 2 h, then 24 h, then 7 days.
inline int64_t retryDelayMinutes(const uint8_t failures) {
  if (failures <= 1) return 30;
  if (failures == 2) return 2 * 60;
  if (failures == 3) return 24 * 60;
  return 7 * 24 * 60;
}

inline bool isQuarantined(const FailEntry& e) { return e.count >= QUARANTINE_AFTER; }

// May this book be tried in this run? A manual 'Sync library now' tries even quarantined books once (the caller
// attempts each book at most once per run). A background pass waits out the back-off (no usable clock: only books
// that have not been quarantined).
inline bool shouldAttempt(const FailEntry* e, const int64_t nowMin, const bool manual) {
  if (!e || e->count == 0) return true;
  if (manual) return true;
  if (nowMin < 0) return !isQuarantined(*e);
  return nowMin >= e->nextRetryMin;
}

inline void recordFailure(FailEntry& e, const FailReason reason, const int64_t nowMin, const uint32_t size) {
  if (e.count < 250) ++e.count;
  e.reason = reason;
  e.size = size;
  e.nextRetryMin = nowMin < 0 ? 0 : nowMin + retryDelayMinutes(e.count);
}

inline bool parseFailLine(const std::string_view line, FailEntry& out) {
  std::string_view f[5];
  if (splitTabs(line, f, 5) < 5) return false;
  char* end = nullptr;
  const std::string idText(f[0]);
  out.idHash = static_cast<uint32_t>(std::strtoul(idText.c_str(), &end, 16));
  if (end == idText.c_str() || out.idHash == 0) return false;
  out.count = static_cast<uint8_t>(std::min<uint64_t>(parseU64(f[1]), 250));
  out.nextRetryMin = static_cast<int64_t>(parseU64(f[2]));
  const uint64_t reason = parseU64(f[3]);
  out.reason =
      reason <= static_cast<uint64_t>(FailReason::UNAUTHORIZED) ? static_cast<FailReason>(reason) : FailReason::NONE;
  out.size = static_cast<uint32_t>(parseU64(f[4]));
  return true;
}

inline std::string formatFailLine(const FailEntry& e) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%08x\t%u\t%lld\t%u\t%u\n", static_cast<unsigned>(e.idHash),
                static_cast<unsigned>(e.count), static_cast<long long>(e.nextRetryMin), static_cast<unsigned>(e.reason),
                static_cast<unsigned>(e.size));
  return buf;
}

// Could this book finish inside what is left of a background pass? Assumes a slow but realistic link (the X4 over
// Wi-Fi to a remote server). Manual runs never ask: they get the long budget and may take whatever they need.
constexpr uint32_t ASSUMED_BYTES_PER_SEC = 150 * 1000;

inline bool fitsRemainingTime(const uint32_t sizeBytes, const uint32_t remainingMs,
                              const uint32_t bytesPerSec = ASSUMED_BYTES_PER_SEC) {
  if (sizeBytes == 0 || bytesPerSec == 0) return true;
  const uint64_t needMs = static_cast<uint64_t>(sizeBytes) * 1000u / bytesPerSec;
  return needMs <= remainingMs;
}

// ---- error log (/.crosspoint/library_sync_errors.log): one line per event, plus how many lines have been seen ----

constexpr size_t ERROR_LOG_MAX_LINES = 120;

inline std::string cleanField(const std::string_view in, const size_t maxLen = 120) {
  std::string out;
  for (const char c : in) {
    if (out.size() >= maxLen) break;
    out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
  }
  return out;
}

// "2026-11-09 08:14\tno data within 25 s\tHidden Pictures.epub\t3\t118501344"
inline std::string formatErrorLine(const std::string_view stamp, const FailReason reason, const std::string_view name,
                                   const unsigned attempts, const uint32_t size) {
  std::string line = cleanField(stamp, 20);
  line += '\t';
  line += failReasonText(reason);
  line += '\t';
  line += cleanField(name);
  line += '\t';
  line += std::to_string(attempts);
  line += '\t';
  line += std::to_string(size);
  line += '\n';
  return line;
}

inline size_t countLines(const std::string& log) {
  size_t n = 0;
  for (const char c : log) n += c == '\n';
  return n;
}

inline size_t unreadCount(const std::string& log, const size_t seenLines) {
  const size_t lines = countLines(log);
  return lines > seenLines ? lines - seenLines : 0;
}

// Keep at most `maxLines`, dropping the oldest first (read lines before any unread one, since they come first).
// `seenLines` follows the lines that are dropped.
inline void rotateLog(std::string& log, size_t& seenLines, const size_t maxLines = ERROR_LOG_MAX_LINES) {
  size_t lines = countLines(log);
  while (lines > maxLines) {
    const size_t nl = log.find('\n');
    if (nl == std::string::npos) break;
    log.erase(0, nl + 1);
    --lines;
    if (seenLines > 0) --seenLines;
  }
  if (seenLines > lines) seenLines = lines;
}

// Short text for the heartbeat: "<book> - <reason>" of the newest line, cut to `maxLen` bytes on a UTF-8 boundary.
inline std::string lastErrorSummary(const std::string& log, const size_t maxLen = 60) {
  if (log.empty()) return {};
  size_t end = log.size();
  while (end > 0 && (log[end - 1] == '\n' || log[end - 1] == '\r')) --end;
  const size_t start = log.rfind('\n', end ? end - 1 : 0);
  const std::string_view line(log.data() + (start == std::string::npos ? 0 : start + 1),
                              end - (start == std::string::npos ? 0 : start + 1));
  std::string_view f[5];
  if (splitTabs(line, f, 5) < 3) return {};
  std::string out(f[2]);
  out += " - ";
  out += f[1];
  if (out.size() > maxLen) {
    size_t cut = maxLen;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
  return out;
}

}  // namespace LibrarySyncCore
