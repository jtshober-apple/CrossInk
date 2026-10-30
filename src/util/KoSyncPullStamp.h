#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "util/RtcMinutes.h"

// Remembers when the open book's server position was last checked (pull on open, or a background push, which
// compares against the server too). Every check reboots through the network boot, and the reader reopens
// afterwards, so this stamp is what stops the reopen from triggering another check.
namespace KoSyncPullStamp {

inline constexpr const char* kPath = "/.crosspoint/kosync_pull";
inline constexpr int64_t kRecentMinutes = 10;

inline bool recentlyChecked(const std::string& bookPath) {
  FsFile file;
  if (!Storage.openFileForRead("KOSync", kPath, file)) return false;
  char buf[512] = {};
  file.read(buf, sizeof(buf) - 1);
  file.close();
  char* nl = strchr(buf, '\n');
  if (!nl) return false;
  *nl = '\0';
  if (bookPath != nl + 1) return false;
  int64_t now = 0;
  // Without a clock the age is unknown: treat the stamp as recent so a reopen can never loop.
  if (!rtcMinutesSinceEpoch(now)) return true;
  const long long stamped = atoll(buf);
  return stamped <= 0 || now < stamped || now - stamped < kRecentMinutes;
}

inline void mark(const std::string& bookPath) {
  FsFile file;
  if (!Storage.openFileForWrite("KOSync", kPath, file)) return;
  int64_t now = 0;
  rtcMinutesSinceEpoch(now);
  char head[24];
  const int n = snprintf(head, sizeof(head), "%lld\n", static_cast<long long>(now));
  file.write(reinterpret_cast<const uint8_t*>(head), n);
  file.write(reinterpret_cast<const uint8_t*>(bookPath.c_str()), bookPath.size());
  file.close();
}

}  // namespace KoSyncPullStamp
