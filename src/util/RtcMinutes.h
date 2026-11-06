#pragma once

#include <HalClock.h>

#include <cstdint>
#include <ctime>

// Minutes since 1970 according to the RTC fields. Only differences between two calls are meaningful: the RTC may
// hold local time rather than UTC. Returns false when there is no RTC or it was never set.
inline bool rtcMinutesSinceEpoch(int64_t& out) {
  struct tm t{};
  if (!halClock.isAvailable() || !halClock.localTime(t) || t.tm_year + 1900 < 2024) return false;
  const int year = t.tm_year + 1900;
  const int month = t.tm_mon + 1;
  const int day = t.tm_mday;
  const int hour = t.tm_hour;
  const int minute = t.tm_min;
  const int y = static_cast<int>(year) - (month <= 2 ? 1 : 0);
  const int era = y / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
  out = days * 1440 + hour * 60 + minute;
  return true;
}
