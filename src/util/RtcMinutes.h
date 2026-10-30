#pragma once

#include <HalClock.h>

#include <cstdint>

// Minutes since 1970 according to the RTC fields. Only differences between two calls are meaningful: the RTC may
// hold local time rather than UTC. Returns false when there is no RTC or it was never set.
inline bool rtcMinutesSinceEpoch(int64_t& out) {
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (!halClock.isAvailable() || !halClock.getDateTime(year, month, day, hour, minute) || year < 2024) return false;
  const int y = static_cast<int>(year) - (month <= 2 ? 1 : 0);
  const int era = y / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
  out = days * 1440 + hour * 60 + minute;
  return true;
}
