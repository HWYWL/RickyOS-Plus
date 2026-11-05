// Diagnostic link wrappers: observe packet headers without replacing lwIP/driver behavior.
// Each hook uses at most 64 bytes of stack scratch; no heap or retained packet data.
#if RICKYOS_PLUS_TCP_TRACE
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_private/wifi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <lwip/ip4.h>
#include <lwip/pbuf.h>
#include <netif/ethernet.h>
#include <cstdint>
#include <cstring>

namespace {
constexpr char tag[] = "TCPTRACE";
uint16_t u16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t u32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
struct TxSample {
  uint32_t ms;
  uint16_t length;
  uint8_t ok;
  uint8_t format;
  uint16_t source;
  uint16_t destination;
  uint32_t sequence;
  uint8_t interface;
  uint8_t wifiType;
  uint8_t wifiSubtype;
  uint8_t protectedFrame;
};
// Fixed 960-byte sample array; publication/counters protected by a short critical
// section. No serial output, heap operation or borrowed pointer retained in Wi-Fi task.
constexpr unsigned kTxSamples = 48;
TxSample samples[kTxSamples]{};
portMUX_TYPE sampleMux = portMUX_INITIALIZER_UNLOCKED;
unsigned total = 0, successful = 0, failed = 0, recorded = 0;
bool active = false;
uint8_t knownAddresses[12]{};  // AP then STA MAC, used only to validate a frame header.
bool addressesKnown = false;
uint32_t generation = 0;
void txDone(uint8_t interface, uint8_t* data, uint16_t* length, bool ok) {
  uint8_t addresses[12];
  bool canClassify;
  uint32_t observedGeneration;
  portENTER_CRITICAL(&sampleMux);
  if (!active) { portEXIT_CRITICAL(&sampleMux); return; }
  canClassify = addressesKnown;
  std::memcpy(addresses, knownAddresses, sizeof(addresses));
  observedGeneration = generation;
  portEXIT_CRITICAL(&sampleMux);
  const uint16_t n = length ? *length : 0;
  TxSample sample{};
  sample.ms = esp_log_timestamp();
  sample.length = n;
  sample.ok = ok;
  sample.interface = interface;
  sample.wifiType = sample.wifiSubtype = 255;
  // Driver completion data is not documented as Ethernet: detect a bounded
  // IPv4/TCP header instead of assuming a MAC/SNAP prefix length. Retain only
  // the SYN tuple/sequence, never the scanned bytes or application payload.
  if (data) {
    const unsigned limit = n < 128 ? n : 128;
    for (unsigned at = 0; at + 40 <= limit; ++at) {
      if (data[at] >> 4 != 4 || data[at + 9] != 6) continue;
      const unsigned ipLength = (data[at] & 15) * 4;
      if (ipLength < 20 || at + ipLength + 20 > limit || u16(data + at + 2) < ipLength + 20) continue;
      const auto* tcp = data + at + ipLength;
      if ((u16(tcp) == 443 || u16(tcp + 2) == 443) && (tcp[13] & 2)) {
        sample.format = static_cast<uint8_t>(at + 3);
        sample.source = u16(tcp);
        sample.destination = u16(tcp + 2);
        sample.sequence = u32(tcp + 4);
        break;
      }
    }
  }
  // Classify only a valid 802.11 header whose receiver/transmitter match the
  // associated AP and this STA. Retain type/subtype/Protected, never addresses,
  // raw bytes or payload. An encrypted frame cannot be matched to a TCP tuple.
  if (data && canClassify) {
    const unsigned limit = n < 128 ? n : 128;
    for (unsigned at = 0; at + 24 <= limit; ++at) {
      const unsigned type = (data[at] >> 2) & 3;
      if ((data[at] & 3) || type > 2 || std::memcmp(data + at + 4, addresses, 12)) continue;
      const unsigned ds = data[at + 1] & 3;
      if ((type == 2 && ds != 1) || (type != 2 && ds != 0)) continue;
      sample.wifiType = type;
      sample.wifiSubtype = (data[at] >> 4) & 15;
      sample.protectedFrame = (data[at + 1] >> 6) & 1;
      break;
    }
  }
  portENTER_CRITICAL(&sampleMux);
  if (active && observedGeneration == generation) {
    ++total;
    if (ok) ++successful; else ++failed;
    if (recorded < kTxSamples) samples[recorded++] = sample;
  }
  portEXIT_CRITICAL(&sampleMux);
}

void tcp(const char* stage, pbuf* packet, uint16_t offset, bool withIp, netif* iface, int result) {
  uint8_t bytes[64]{};
  const uint16_t n = pbuf_copy_partial(packet, bytes, sizeof(bytes), offset);
  unsigned at = 0;
  if (withIp) {
    if (n < 20 || bytes[0] >> 4 != 4 || bytes[9] != 6 || (u16(bytes + 6) & 0x1fff)) return;
    at = (bytes[0] & 15) * 4;
    if (at < 20 || at + 20 > n) return;
  } else if (n < 20) return;
  const auto* h = bytes + at;
  const auto flags = h[13];
  if ((u16(h) != 443 && u16(h + 2) != 443) || !(flags & (0x02 | 0x04))) return;
  ESP_LOGI(tag, "%s if=%c%c%u srcport=%u dstport=%u flags=0x%02x seq=%lu ack=%lu result=%d",
           stage, iface ? iface->name[0] : '?', iface ? iface->name[1] : '?', iface ? iface->num : 0,
           u16(h), u16(h + 2), flags, static_cast<unsigned long>(u32(h + 4)),
           static_cast<unsigned long>(u32(h + 8)), result);
}
void arp(const char* stage, pbuf* packet, uint16_t offset, netif* iface) {
  uint8_t bytes[28]{};
  if (pbuf_copy_partial(packet, bytes, sizeof(bytes), offset) != sizeof(bytes) ||
      u16(bytes + 2) != 0x0800 || bytes[4] != 6 || bytes[5] != 4) return;
  ESP_LOGI(tag, "%s if=%c%c%u op=%u sender=%u.%u.%u.%u target=%u.%u.%u.%u", stage,
           iface ? iface->name[0] : '?', iface ? iface->name[1] : '?', iface ? iface->num : 0,
           u16(bytes + 6), bytes[14], bytes[15], bytes[16], bytes[17], bytes[24], bytes[25], bytes[26], bytes[27]);
}
}
void plusTcpTraceBegin() {
  portENTER_CRITICAL(&sampleMux);
  active = false;
  ++generation;
  total = successful = failed = recorded = 0;
  portEXIT_CRITICAL(&sampleMux);
  uint8_t station[6]{};
  wifi_ap_record_t ap{};
  const bool known = esp_wifi_get_mac(WIFI_IF_STA, station) == ESP_OK &&
                     esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  portENTER_CRITICAL(&sampleMux);
  addressesKnown = known;
  std::memcpy(knownAddresses, ap.bssid, 6);
  std::memcpy(knownAddresses + 6, station, 6);
  portEXIT_CRITICAL(&sampleMux);
  const esp_err_t result = esp_wifi_set_tx_done_cb(txDone);
  portENTER_CRITICAL(&sampleMux);
  active = result == ESP_OK;
  portEXIT_CRITICAL(&sampleMux);
  ESP_LOGI(tag, "TXDONE registered=%d static_sample_bytes=%u", result, unsigned(sizeof(samples)));
}
void plusTcpTraceEnd() {
  portENTER_CRITICAL(&sampleMux);
  active = false;
  const unsigned count = recorded, all = total, good = successful, bad = failed;
  portEXIT_CRITICAL(&sampleMux);
  ESP_LOGI(tag, "TXDONE total=%u ok=%u failed=%u recorded=%u", all, good, bad, count);
  for (unsigned i = 0; i < count; ++i) {
    const auto& v = samples[i];
    ESP_LOGI(tag, "TXDONE ms=%lu len=%u ok=%u format=%u srcport=%u dstport=%u seq=%lu ifidx=%u wifi_type=%u wifi_subtype=%u protected=%u",
               static_cast<unsigned long>(v.ms), v.length, v.ok, v.format, v.source, v.destination,
               static_cast<unsigned long>(v.sequence), v.interface, v.wifiType, v.wifiSubtype, v.protectedFrame);
  }
}

extern "C" {
err_t __real_ip4_output_if(pbuf*, const ip4_addr_t*, const ip4_addr_t*, u8_t, u8_t, u8_t, netif*);
err_t __wrap_ip4_output_if(pbuf* p, const ip4_addr_t* src, const ip4_addr_t* dst,
                          u8_t ttl, u8_t tos, u8_t proto, netif* iface) {
  if (proto == 6) tcp("IP-TX", p, 0, false, iface, 0);
  const err_t result = __real_ip4_output_if(p, src, dst, ttl, tos, proto, iface);
  if (proto == 6 && result != ERR_OK) ESP_LOGI(tag, "IP-TX error=%d", result);
  return result;
}
err_t __real_ethernet_output(netif*, pbuf*, const eth_addr*, const eth_addr*, u16_t);
err_t __wrap_ethernet_output(netif* iface, pbuf* p, const eth_addr* src, const eth_addr* dst, u16_t type) {
  if (type == 0x0800) tcp("ETH-TX", p, 0, true, iface, 0);
  else if (type == 0x0806) arp("ARP-TX", p, 0, iface);
  const err_t result = __real_ethernet_output(iface, p, src, dst, type);
  if (result != ERR_OK) ESP_LOGI(tag, "ETH-TX type=0x%x error=%d", type, result);
  return result;
}
err_t __real_ethernet_input(pbuf*, netif*);
err_t __wrap_ethernet_input(pbuf* p, netif* iface) {
  uint8_t header[14]{};
  if (pbuf_copy_partial(p, header, sizeof(header), 0) == sizeof(header)) {
    if (u16(header + 12) == 0x0800) tcp("ETH-RX", p, 14, true, iface, 0);
    else if (u16(header + 12) == 0x0806) arp("ARP-RX", p, 14, iface);
  }
  return __real_ethernet_input(p, iface);
}
}
#endif
