#include <PlusHttpsClock.h>
#include <PlusHttpDate.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <Arduino.h>
#include <WiFi.h>
#include <PlusNetworkProbe.h>
#include <cstdio>
#include "network/RickyOtaPolicy.h"
#include "network/RickyOtaTrust.h"

bool metalioPlusFetchHttpsUtc(time_t& epoch) {
  // Reuse the firmware's existing wolfSSL transport and OTA trust anchor.
  // Its connection/header buffers are temporary and freed by the stack
  // object's destructor; never buffer the body or add another TLS stack.
  metalioPlusProbeNetwork();
  // Arduino 4 RC1 returns negative DNS errors. Its TCP wrapper tests only
  // zero and otherwise tries 0.0.0.0; reject every result except success.
  IPAddress resolved;
  const unsigned long dnsStarted = millis();
  const int dnsResult = WiFi.hostByName("chinoryunqin.github.io", resolved);
  LOG_INF("CLK", "HTTPS DNS result=%d elapsed=%lu", dnsResult, millis()-dnsStarted);
  if (dnsResult != 1) return false;
  freeink::SecureHttpClient http;
  http.setCACert(ricky_ota::ROOT_CA);
  http.setTimeout(7000);
  http.setReuse(false);
  http.setFollowRedirects(0);
  char url[160];
  const int length=snprintf(url,sizeof(url),"%s?clock=%lld-%lu",ricky_ota::MANIFEST_URL,
                            static_cast<long long>(time(nullptr)),millis());
  if (length<0 || static_cast<size_t>(length)>=sizeof(url) || !http.begin(url)) return false;
  http.addHeader("Cache-Control","no-cache");
  size_t bodyBytes=0;
  const unsigned long started=millis();
  const int status=http.GET([&bodyBytes](const uint8_t*,size_t bytes) {
    if (bytes>4096-bodyBytes) return false;
    bodyBytes+=bytes;
    return true;
  },[started] { return millis()-started>=12000; });
  const std::string date=http.getHeader("date");
  const std::string age=http.getHeader("age");
  if (status!=200 || !http.responseComplete() || millis()-started>=12000 ||
      !plusclock::parseHttpUtc(date.c_str(),age.c_str(),epoch)) {
    LOG_ERR("CLK","Verified HTTPS clock failed: status=%d complete=%d elapsed=%lu",status,
            http.responseComplete(),millis()-started);
    return false;
  }
  LOG_INF("CLK","Verified HTTPS clock received: UTC epoch=%lld age=%s elapsed=%lu",
          static_cast<long long>(epoch),age.c_str(),millis()-started);
  return true;
}
