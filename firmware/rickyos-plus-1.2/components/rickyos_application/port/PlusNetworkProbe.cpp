#include <PlusNetworkProbe.h>
#include "PlusNetAllocTrace.h"
#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace {
struct Probe {
  int fd = -1;
  const char* name;
  bool tcp;
  bool done = false;
  ~Probe() { if (fd >= 0) close(fd); }
};
void addressText(const IPAddress& ip, char (&out)[16]) {
  snprintf(out, sizeof(out), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}
void start(Probe& probe, const char* address, unsigned port, const uint8_t* payload, size_t size) {
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  if (inet_pton(AF_INET, address, &peer.sin_addr) != 1 || peer.sin_addr.s_addr == 0) {
    LOG_ERR("NETPROBE", "%s: missing IPv4 destination", probe.name);
    probe.done = true;
    return;
  }
  probe.fd = socket(AF_INET, probe.tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
  if (probe.fd < 0 || fcntl(probe.fd, F_SETFL, O_NONBLOCK) < 0) {
    LOG_ERR("NETPROBE", "%s: socket setup errno=%d", probe.name, errno);
    probe.done = true;
    return;
  }
  const int result = probe.tcp ? connect(probe.fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) :
      sendto(probe.fd, payload, size, 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
  const int error = result < 0 ? errno : 0;
  LOG_INF("NETPROBE", "%s destination=%s:%u result=%d errno=%d", probe.name, address, port, result, error);
  if (result < 0 && (!probe.tcp || error != EINPROGRESS)) probe.done = true;
}
}

void metalioPlusProbeNetwork() {
#if RICKYOS_PLUS_NET_ALLOC_TRACE
  plusNetAllocTraceSnapshot("probe-begin");
#endif
  // Diagnostic only: no clock changes, DNS replacement or credential output.
  // Stack buffers; lwIP sockets own bounded temporary buffers and all close on return.
  char local[16], gateway[16], dns[16], mask[16];
  addressText(WiFi.localIP(), local);
  addressText(WiFi.gatewayIP(), gateway);
  addressText(WiFi.dnsIP(), dns);
  addressText(WiFi.subnetMask(), mask);
  LOG_INF("NETPROBE", "IPv4=%s gateway=%s mask=%s DHCP-DNS=%s RSSI=%d", local, gateway, mask, dns, WiFi.RSSI());
  // A standard recursive A query for ntp.aliyun.com; distinct transaction ID.
  const uint8_t query[] = {0x52,0x4b,1,0,0,1,0,0,0,0,0,0,3,'n','t','p',6,'a','l','i','y','u','n',3,'c','o','m',0,0,1,0,1};
  uint8_t ntp[48] = {0x23};
  ntp[40] = 0x52; ntp[41] = 0x4b; ntp[47] = 1;
  Probe probes[] = {{-1,"DNS-DHCP",false}, {-1,"DNS-public",false}, {-1,"NTP-direct",false}, {-1,"HTTPS-TCP",true}};
  start(probes[0], dns, 53, query, sizeof(query));
  start(probes[1], "119.29.29.29", 53, query, sizeof(query));
  start(probes[2], "119.28.183.184", 123, ntp, sizeof(ntp));
  // Numeric GitHub Pages address obtained for the existing OTA hostname;
  // this checks TCP only, without accepting unverified data as time.
  start(probes[3], "185.199.108.153", 443, nullptr, 0);
  const unsigned long started = millis();
  while (millis() - started < 2200) {
    fd_set readSet, writeSet;
    FD_ZERO(&readSet); FD_ZERO(&writeSet);
    int highest = -1;
    for (auto& probe : probes) if (probe.fd >= 0 && !probe.done) {
      FD_SET(probe.fd, probe.tcp ? &writeSet : &readSet);
      if (probe.fd > highest) highest = probe.fd;
    }
    if (highest < 0) break;
    timeval wait = {0, 100000};
    if (select(highest + 1, &readSet, &writeSet, nullptr, &wait) < 0) {
      LOG_ERR("NETPROBE", "select errno=%d", errno); break;
    }
    for (auto& probe : probes) if (probe.fd >= 0 && !probe.done) {
      if (probe.tcp && FD_ISSET(probe.fd, &writeSet)) {
        int error = 0; socklen_t length = sizeof(error);
        const int result = getsockopt(probe.fd, SOL_SOCKET, SO_ERROR, &error, &length);
        LOG_INF("NETPROBE", "%s completed result=%d error=%d elapsed=%lu", probe.name, result, error, millis()-started);
        probe.done = true;
      } else if (!probe.tcp && FD_ISSET(probe.fd, &readSet)) {
        uint8_t response[512]; sockaddr_in peer{}; socklen_t length = sizeof(peer);
        const int bytes = recvfrom(probe.fd, response, sizeof(response), 0, reinterpret_cast<sockaddr*>(&peer), &length);
        char sender[16]{}; inet_ntop(AF_INET, &peer.sin_addr, sender, sizeof(sender));
        LOG_INF("NETPROBE", "%s received bytes=%d from=%s:%u elapsed=%lu", probe.name, bytes, sender, ntohs(peer.sin_port), millis()-started);
        if (bytes >= 12 && probe.name[0] == 'D')
          LOG_INF("NETPROBE", "DNS transaction=%02x%02x flags=%02x%02x answers=%u", response[0], response[1], response[2], response[3], (response[6]<<8)|response[7]);
        if (bytes >= 48 && probe.name[0] == 'N')
          LOG_INF("NETPROBE", "NTP mode=%u stratum=%u originateMatches=%d", response[0]&7, response[1], memcmp(response+24,ntp+40,8)==0);
        probe.done = true;
      }
    }
  }
  for (const auto& probe : probes) if (!probe.done) LOG_ERR("NETPROBE", "%s no response within 2200 ms", probe.name);
#if RICKYOS_PLUS_NET_ALLOC_TRACE
  plusNetAllocTraceSnapshot("probe-end");
#endif
}
