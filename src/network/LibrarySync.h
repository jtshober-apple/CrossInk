#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "util/LibrarySyncCore.h"

/**
 * Library Sync: keeps every book of an OPDS catalog on the SD card.
 *
 * "Sync now" does the whole job (first install of a big library, run from a clean network boot while charging).
 * Afterwards a short background pass on the existing Tesserae timed wake looks for new books and fetches a few
 * per wake. Nothing is ever deleted from the card: a book is only ever added, or (opt-in) replaced by a newer
 * copy, and the old copy stays until the new one has been checked.
 *
 * The server publishes a compact manifest at <server url>/manifest.tsv (see LibrarySyncCore.h); a server without
 * one is crawled through its OPDS feeds instead (new books only, since a feed gives no size or hash).
 *
 * Self-contained on purpose: this file, LibrarySyncCore.h and LibrarySyncActivity are the whole feature, plus a
 * few one-line hooks (settings entry, network boot target, the timed wake in main.cpp).
 */
namespace LibrarySync {

using LibrarySyncCore::Schedule;

struct Config {
  Schedule schedule = Schedule::MANUAL;
  bool homeOnly = false;        // background passes only on the network recorded in homeSsid
  uint8_t batteryFloor = 40;    // background passes skip below this percentage (unless on USB power)
  bool replaceUpdated = false;  // fetch a newer copy of a book the server changed
  uint8_t serverIndex = 0;      // which saved OPDS server
  char homeSsid[33] = "";       // set by the first successful manual sync while homeOnly is on
  int64_t lastCheckMin = 0;     // RTC minutes of the last completed manifest check
  bool backlog = false;         // the last pass left books still to fetch
  uint16_t lastTotal = 0;       // books the server listed at the last check
  uint16_t lastSynced = 0;      // of those, books tracked on this card
};

Config& config();
void loadConfig();
bool saveConfig();

struct Progress {
  uint16_t total = 0;     // books on the server
  uint16_t synced = 0;    // tracked on this card (already there, adopted, or fetched)
  uint16_t fetched = 0;   // fetched this run
  uint16_t failed = 0;    // failed this run (each one is also a line in the error log)
  uint16_t waiting = 0;   // not tried this run: in back-off after failing before, or too big for a background pass
  uint16_t missing = 0;   // still to fetch after this run
  uint16_t position = 0;  // 1-based index of the book being fetched in the current batch
  uint16_t batch = 0;
  uint32_t bytes = 0;
  uint32_t bytesTotal = 0;
  std::string title;  // file name of the book being fetched
};

enum class Result : uint8_t {
  OK,         // everything on the server is on the card
  PARTIAL,    // budget ran out or a book failed; more to do on the next pass
  NO_SERVER,  // no OPDS server saved
  NETWORK,    // the manifest or feeds could not be read
  ABORTED,    // cancelled (Back, or Power during a background wake)
  SKIPPED,    // policy said not now (battery, network, not due)
};

const char* resultString(Result result);

using AbortFn = std::function<bool()>;
using ProgressFn = std::function<void(const Progress&)>;

/** Wi-Fi must already be connected. Runs until done, out of budget, or aborted. */
Result run(bool manual, const AbortFn& abort, const ProgressFn& progress, Progress& out);

/** True when a background pass is due now (settings, schedule, battery). Cheap: reads the config only. */
bool backgroundDue();
// The home network, when automatic sync is on and limited to it: the timed-wake Wi-Fi join tries it first.
std::string preferredJoinSsid();

/** Background pass on the timed wake. Wi-Fi is already up from the dashboard refresh. Saves state; never throws. */
Result runBackground(const AbortFn& abort);

// ---- error log -------------------------------------------------------------------------------------------------
// Every book that fails, is skipped as a problem, or is too big for a background pass adds one line to
// /.crosspoint/library_sync_errors.log (time, reason, file, attempt, size). "Seen" lines are tracked separately so
// the next start-up can say how many are new. Only this log (and its marker) is ever deleted, and only on request.
size_t errorsUnread();            // lines not yet viewed
std::string errorsLastSummary();  // "<book> - <reason>" of the newest line, <= 60 bytes (for the heartbeat)
std::string readErrorLog();       // the whole log, oldest line first (bounded to a few KB)
void markErrorsRead();            // everything now in the log counts as seen
bool clearErrorLog();             // remove the log and its marker; true when a log file existed

/** The server the settings point at, or null. */
struct ServerInfo {
  std::string name;
  bool found = false;
};
ServerInfo selectedServer();

}  // namespace LibrarySync
