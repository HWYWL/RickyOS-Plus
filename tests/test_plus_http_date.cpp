#include "../firmware/rickyos-plus-1.2/components/rickyos_platform/include/PlusHttpDate.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <initializer_list>
int main() {
  setenv("TZ","Asia/Shanghai",1);tzset();
  time_t epoch=0;
  assert(plusclock::parseHttpUtc("Sun, 11 Oct 2026 04:25:50 GMT","",epoch) && epoch==1791692750);
  assert(plusclock::parseHttpUtc("Sun, 11 Oct 2026 04:25:50 GMT","66",epoch) && epoch==1791692816);
  assert(plusclock::parseHttpUtc("Thu, 29 Feb 2024 23:59:59 GMT","1",epoch));
  tm utc={};gmtime_r(&epoch,&utc);assert(utc.tm_mon==2 && utc.tm_mday==1 && utc.tm_hour==0);
  for(const char* value:{"Sun, 32 Oct 2026 04:25:50 GMT","Sat, 29 Feb 2025 04:25:50 GMT",
                         "Sun, 11 Oct 2026 24:25:50 GMT","Sun, 11 Oct 2026 04:25:60 GMT",
                         "Sun, 11 Oct 2026 04:25:50 UTC","Mon, 11 Oct 2026 04:25:50 GMT"})
    assert(!plusclock::parseHttpUtc(value,"",epoch));
  for(const char* age:{"-1","+1","1x","3601","999999999999999999"})
    assert(!plusclock::parseHttpUtc("Sun, 11 Oct 2026 04:25:50 GMT",age,epoch));
  // Round-trip every day for nine years; local timezone must have no effect.
  constexpr time_t first=1704067200;
  for(unsigned day=0; day<3288; ++day) {
    time_t sample=first+day*86400+43210;
    gmtime_r(&sample,&utc);char date[30];strftime(date,sizeof(date),"%a, %d %b %Y %H:%M:%S GMT",&utc);
    assert(plusclock::parseHttpUtc(date,"30",epoch) && epoch==sample+30);
  }
  assert(!std::strcmp(getenv("TZ"),"Asia/Shanghai"));
  puts("HTTP UTC date: cache age, leap days, all-day round trips, invalid data and TZ independence passed");
}
