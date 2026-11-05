#pragma once
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdint>

namespace plusclock {
// GMT parsing never changes the user's TZ. The cache Age is part of the
// current server-time estimate, not the age of the manifest's contents.
inline bool parseHttpUtc(const char* date, const char* age, time_t& epoch) {
  if (!date || std::strlen(date) != 29 || !age) return false;
  char weekday[4] = {}, monthName[4] = {}, zone[4] = {};
  unsigned day, year, hour, minute, second;
  int consumed = 0;
  if (std::sscanf(date, "%3s, %2u %3s %4u %2u:%2u:%2u %3s%n", weekday, &day, monthName, &year,
                  &hour, &minute, &second, zone, &consumed) != 8 || consumed != 29 || std::strcmp(zone,"GMT")) return false;
  if (year < 2024 || year > 2099 || hour > 23 || minute > 59 || second > 59) return false;
  constexpr const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  unsigned month = 0;
  for (unsigned i = 0; i < 12; ++i) if (!std::strcmp(monthName,months[i])) month=i+1;
  if (!month) return false;
  constexpr unsigned lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  unsigned length=lengths[month-1];
  if (month==2 && year%4==0 && (year%100!=0 || year%400==0)) ++length;
  if (!day || day>length) return false;
  unsigned ageSeconds=0;
  for (const char* p=age; *p; ++p) {
    if (*p<'0' || *p>'9') return false;
    ageSeconds=ageSeconds*10+static_cast<unsigned>(*p-'0');
    if (ageSeconds>3600) return false;
  }
  int y=static_cast<int>(year)-(month<=2);
  const int era=y/400;
  const unsigned yoe=static_cast<unsigned>(y-era*400);
  const unsigned shifted=month>2 ? month-3 : month+9;
  const unsigned doy=(153*shifted+2)/5+day-1;
  const int64_t days=static_cast<int64_t>(era)*146097+yoe*365+yoe/4-yoe/100+doy-719468;
  constexpr const char* weekdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  if (std::strcmp(weekday,weekdays[(days+4)%7])) return false;
  const int64_t value=days*86400+hour*3600+minute*60+second+ageSeconds;
  const time_t result=static_cast<time_t>(value);
  if (static_cast<int64_t>(result)!=value) return false;
  epoch=result;
  return true;
}
} // namespace plusclock
