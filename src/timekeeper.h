// timekeeper.h — wall-clock time via SNTP with a POSIX timezone.
//
// Starts SNTP once Wi-Fi is up and exposes the current time as epoch / ISO-UTC /
// local strings. The timezone is a POSIX TZ string (e.g. "GMT0BST,M3.5.0/1,
// M10.5.0" for the UK, "CET-1CEST,M3.5.0,M10.5.0/3" for CET), persisted by Config.
#pragma once

#include <Arduino.h>

#include <ctime>
#include <string>

namespace fogger {

struct TimeConfig {
  std::string tz = "GMT0";                 // POSIX TZ string (default UTC)
  std::string ntp1 = "pool.ntp.org";
  std::string ntp2 = "time.google.com";
};

class TimeKeeper {
 public:
  // Configure TZ and kick off SNTP; sync completes in the background once the
  // network is reachable.
  void begin(const TimeConfig& cfg);

  // Change the timezone at runtime without restarting SNTP.
  void applyTimezone(const std::string& tz);

  // Best-effort: query an IP-geolocation service for the current UTC offset and
  // return it as a FIXED-offset POSIX TZ (e.g. "GMT-1"). Requires STA network.
  // Returns "" on any failure. Fixed offset = correct now, but no future DST
  // transitions; the user can still set a full POSIX TZ with DST rules.
  std::string guessTimezoneViaIp();

  // True once a plausible real time has been acquired.
  bool synced() const;

  time_t nowEpoch() const { return time(nullptr); }
  std::string isoUtc() const;      // "2026-10-14T12:34:56Z" ("" if not synced)
  std::string localString() const; // "2026-10-14 13:34:56"   ("" if not synced)

 private:
  TimeConfig cfg_;
  bool started_ = false;
  // Guards TZ mutation (setenv/tzset in applyTimezone) against localtime_r reads
  // on another task. void* so this header stays free of FreeRTOS includes.
  void* mutex_ = nullptr;
};

}  // namespace fogger
