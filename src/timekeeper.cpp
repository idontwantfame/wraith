// timekeeper.cpp — SNTP + timezone. See timekeeper.h.
#include "timekeeper.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace fogger {
namespace {
// Any epoch past this (2023-11) means SNTP has set a real time, not the 1970 boot clock.
constexpr time_t kPlausibleEpoch = 1700000000;

SemaphoreHandle_t asMutex(void* h) { return static_cast<SemaphoreHandle_t>(h); }

class Lock {
 public:
  explicit Lock(void* m) : m_(m) {
    if (m_) xSemaphoreTake(asMutex(m_), portMAX_DELAY);
  }
  ~Lock() {
    if (m_) xSemaphoreGive(asMutex(m_));
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

 private:
  void* m_;
};

// Convert a UTC offset in seconds EAST of UTC (what IP APIs report) into a
// fixed-offset POSIX TZ string. POSIX offsets are sign-inverted vs. ISO 8601.
// The sign comes from the whole offset, so sub-hour zones (e.g. +00:30 east)
// keep the right sign even though the hours field is 0.
std::string offsetToPosixTz(long offsetEastSec) {
  const long posix = -offsetEastSec;  // POSIX: seconds to add to local -> UTC
  const bool neg = posix < 0;
  const long mag = neg ? -posix : posix;
  const int hours = static_cast<int>(mag / 3600);
  const int minutes = static_cast<int>((mag % 3600) / 60);
  const char sign = neg ? '-' : '+';
  char buf[16];
  if (minutes == 0) {
    snprintf(buf, sizeof(buf), "GMT%c%d", sign, hours);
  } else {
    snprintf(buf, sizeof(buf), "GMT%c%d:%02d", sign, hours, minutes);
  }
  return std::string(buf);
}
}  // namespace

void TimeKeeper::begin(const TimeConfig& cfg) {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
  cfg_ = cfg;
  // configTzTime sets TZ and starts the SNTP client; it syncs when the network
  // is reachable, so it is safe to call before Wi-Fi is up.
  Lock lock(mutex_);
  configTzTime(cfg_.tz.c_str(), cfg_.ntp1.c_str(), cfg_.ntp2.c_str(), nullptr);
  started_ = true;
  log_i("time: SNTP started, tz=%s", cfg_.tz.c_str());
}

void TimeKeeper::applyTimezone(const std::string& tz) {
  Lock lock(mutex_);  // serialise setenv/tzset against localtime_r readers
  cfg_.tz = tz;
  setenv("TZ", tz.c_str(), 1);
  tzset();
  log_i("time: timezone set to %s", tz.c_str());
}

std::string TimeKeeper::guessTimezoneViaIp() {
  HTTPClient http;
  // Free, no-key endpoint; returns the current offset (DST-aware) + IANA name.
  if (!http.begin("http://ip-api.com/json/?fields=status,offset,timezone")) {
    return "";
  }
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    log_w("time: IP geolocation HTTP %d", code);
    return "";
  }
  // The reply is a few fields of JSON (~100 B). Cap the body before buffering it
  // so a hostile/compromised endpoint on this plain-HTTP path can't exhaust the
  // heap of the small guess task. Unknown length (chunked) is rejected too.
  constexpr int kMaxGeoBody = 1024;
  const int bodyLen = http.getSize();
  if (bodyLen < 0 || bodyLen > kMaxGeoBody) {
    http.end();
    log_w("time: IP geolocation body length %d rejected", bodyLen);
    return "";
  }
  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return "";
  if (std::string(doc["status"] | "") != "success") return "";

  const long offset = doc["offset"] | 0L;  // seconds east of UTC (incl. DST now)
  if (offset < -50400 || offset > 50400) return "";  // sanity: within +/-14 h
  const std::string tz = offsetToPosixTz(offset);
  log_i("time: IP guess %s -> %s", (const char*)(doc["timezone"] | ""), tz.c_str());
  return tz;
}

bool TimeKeeper::synced() const { return time(nullptr) > kPlausibleEpoch; }

std::string TimeKeeper::isoUtc() const {
  if (!synced()) return "";
  time_t now = time(nullptr);
  struct tm tm;
  gmtime_r(&now, &tm);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf);
}

std::string TimeKeeper::localString() const {
  if (!synced()) return "";
  time_t now = time(nullptr);
  struct tm tm;
  {
    Lock lock(mutex_);  // localtime_r reads TZ; don't race applyTimezone
    localtime_r(&now, &tm);
  }
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return std::string(buf);
}

}  // namespace fogger
