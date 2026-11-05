# Plus WPA3-Personal station support — 2026-10-25

Latest result: the Mac WPA3-only AP follow-up passed 12/12 reconnects and
font-list requests plus 2/2 complete-body CRCs. Earlier sections below retain
the prior no-AP limitations and failed direct-config replay attempts as history.

## Scope and evidence

This is a Plus-only connection change, on top of the allocation/network fix.
It does not turn on Wi-Fi 6, change transmission power, or change the device's
file-transfer hotspot to WPA3. Pico and SD initialization are unchanged.

Before this change, `firmware/rickyos-plus-1.2/sdkconfig.defaults:86-89`
explicitly disabled `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE`. This was an October 15
workaround for reason 202 on one WPA2/WPA3 transition network, not evidence that
the S31 cannot support WPA3. Arduino 4.0.0-RC1 `libraries/WiFi/src/STA.cpp:393-419`
zero-initializes each station configuration and calls `esp_wifi_set_config`
without explicitly selecting a SAE PWE method. The actual compiled reader
uses `upstream/src/activities/network/WifiSelectionActivity.cpp:628-630` for
`WiFi.begin`; resource and saved-network connections must use the same policy.

The fixed IDF 9a97f6c5 provides the relevant configuration and API definitions
in `components/esp_wifi/Kconfig:300-349` and
`components/esp_wifi/include/esp_wifi_types_generic.h:507-515,575-584`.
Use symbolic enums: its stale field comment and its enum's numeric values do
not agree. No SDK checkout or shared Arduino dependency is modified.

## Implementation

- Enable station SAE, H2E and WPA3 compatibility support. Keep SAE-PK and WPA3
  SoftAP disabled; those extensions are outside the requested station support.
- Link the Plus-only `PlusWifiSecurity.cpp` adapter around
  `esp_wifi_set_config`. Password-based STA calls with unspecified PWE use
  `WPA3_SAE_PWE_BOTH` (Hunting and Pecking plus Hash to Element).
- Retain explicitly selected PWE methods, PMF requirements, authentication
  thresholds, channel/BSSID/scan policy, and all other caller configuration.
  Open-network, AP and null inputs pass through. Driver errors are returned.
- Use one scoped stack copy of the driver config, with no new heap allocation,
  persistent credentials or credential logging. The real SDK copies the
  configuration synchronously.
- PMF stays optional for ordinary WPA2 APs; WPA3 itself requires PMF, as enforced
  by IDF. There is no automatic downgrade after authentication failure.
- A compile-time check and `check_plus_wifi_security.py` reject stale SAE/H2E
  configurations. The build also checks WPA3 compatibility support.
- Diagnostic link records include actual auth mode even outside Wi-Fi 6
  comparisons. This prevents ordinary-network results being called WPA3 tests.

## Verification and remaining acceptance

Host tests compile and execute the actual adapter with ASan/UBSan, checking
both PWE methods, explicit method preservation, PMF/auth threshold preservation,
unchanged caller memory, AP/open/null forwarding and driver error propagation.
Generated configuration checks reject missing/disabled SAE, H2E or compatibility
support. The automatic UI/network test harness is also exercised.

The user currently has no WPA3-only 2.4 GHz network. A successful build or a
successful existing WPA/WPA2 connection cannot verify a SAE handshake. Before
claiming WPA3 interoperability, test WPA3-only with PMF required using both H2E
and Hunting and Pecking AP configurations, plus WPA2/WPA3 transition mode.
Record negotiated auth mode, connection/reconnection, DNS/TLS, complete body
integrity, and wrong-password handling. In particular, the old transition-mode
reason-202 case remains a required regression; enabling both PWE methods alone
does not prove its root cause or its resolution.

Official references:

- [ESP32-S31 Wi-Fi security / WPA3 and PMF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-guides/wifi-security.html)
- [Manufacturer public configuration](https://github.com/CloudZao/Metalio-E-INK4-Plus/blob/main/sdkconfig)

The manufacturer source enables station SAE/H2E, but it is not proof of what
was compiled into the separately supplied 2.0.67 binary or how that binary
performs on a WPA3-only network.

This is an experimental source branch, not a published firmware release.

## Local build evidence

The diagnostic package was built with clean patched IDF
`9a97f6c54ec638111ce55cd36581b3c192f15207`; application size is 5,050,608
bytes (within the 6,553,600-byte app slot), SHA-256
`1e4a62d92ed6f81d03cc5fb881a6585f91aee598a0f1a3dd5f0ea68e7f8d1f25`.
Final disassembly confirms six external call sites reach the Plus security
adapter. Allocation tracing hooks remain in IRAM; the reviewed configured
changes are limited to the preceding 2 KiB allocation preference and the
explicit station WPA3/SAE feature keys. Wi-Fi 6 comparison is off.

`artifacts/plus-wireless-20261025/wpa3-r1-build.json` retains sanitized build
identity and checks. The diagnostic version is `1.2.7-netmem`; it must not be
mistaken for a regular release or left running unattended tests indefinitely.

The user reflashed the test device during development. A fresh identity read
found `1.2.7-bt4` (ELF SHA-256
`39b2c1095605a9ffdbcb8f5dee2719369ff55c922e5995fbe50598f86a294490`)
in app0. The temporary test must preserve and restore this new image rather
than replacing it permanently with the earlier network baseline. An initial
single-read recovery attempt failed with a USB stream interruption **before
any Flash write**; retain that failed attempt, then use bounded verified
recovery chunks rather than treating an incomplete backup as usable.

The chunked USB read also stopped after 4,194,304 bytes, before any write.
The exact locally archived `bt4` image was then found and validated against
its full image digest and ELF identity. A padded complete-slot recovery copy
is accepted only if the physical device's complete-slot MD5 matches it;
a version label or a matching ELF header alone is insufficient.

## Initial device runs (retained failures)

The first diagnostic image booted successfully, with SD initialized and an
initial WPA2-PSK association (`pmf:0`, b/g/n). In each of two independent boots,
the automatic runner's first direct replay of `esp_wifi_get_config` after
full station teardown timed out at 35,034 ms. Both runs ended before any font
HTTP request; zero completed HTTP rounds is not a pass. Reason 201
(`NO_AP_FOUND`) was recorded, with no observed allocation failure or panic.
These are not WPA3 authentications and do not reproduce the old SAE reason-202
case. The failed full-cycle results are saved as `wpa3-r1-run1.json` and
`wpa3-r1-run2.json`.

A subsequent default-off security diagnostic uses the same Arduino
`WiFi.begin(ssid, password)` builder as the real application rather than
replaying the driver's normalized configuration. It retains the reference TX
API and b/g/n in every phase, counts failed associations even when no HTTP
request begins, and continues bounded UI rounds after failed associations.
This is an additional diagnostic path, not a change to production reconnect
semantics. The original two failures are not erased by a later success.

## Production-entry regression results

Diagnostic r2 uses the same product security adapter with an automatic
`WiFi.begin` connection loop. It was built with clean IDF 9a97f6c5,
5,050,656 bytes, SHA-256
`1d678b19930e7aac66ee2bdbf1e8c30b42ffbf0a45d90bfc9ccf2546b23945f1`,
ELF `dbeff30429677aaab5ca92847051ecdd86fc7109641e32cc4e796ae5077d12bd`.
Final disassembly confirms five call sites reach the security adapter.

- 12/12 fresh station connections and 12/12 font list requests succeeded in
  170,186 ms overall. Join times were 6,388–6,468 ms; this includes radio startup.
- All rounds used reference TX and b/g/n (mask 7), negotiated HT20. AP records
  report authmode 4 (WPA/WPA2), so none is evidence of a WPA3 handshake.
- The 9,024-byte index transfers took 592–664 ms including production SD writes;
  full UI iteration time is different and includes painting/teardown.
- Both complete 1,763,452-byte font bodies passed SD read-back CRC 2837099957,
  taking 5,515 and 5,728 ms. Temporary files were cleaned up.
- No observed heap-wrapper allocation failure, pbuf allocation failure, driver
  NO_MEM, panic, or SD initialization CRC failure. Non-NO_MEM interface errors
  during explicit teardown are retained and are not claimed to be zero.
- WeRead session probes returned `needs_login` in all three rounds. This is
  **not** a WeRead authenticated end-to-end pass; no login or renewal was done.

Numeric evidence: `wpa3-r2-build.json` and `wpa3-r2-run1.json`. The two earlier
failed direct-replay runs remain available. R2's fresh builder and AP record
are different (authmode 4 vs the earlier bootstrap's WPA2-only security log),
so these results do not uniquely identify why raw driver-config replay failed.
They do establish the current normal application's connection entry works in
this small WPA/WPA2 network sample with SAE enabled.

At this stage of the earlier ordinary-network runs, WPA3-only/H2E-only/
transition-network real associations and wrong-password handling were still
unverified because no corresponding AP was then available.
The ordinary regression cannot close the historic SAE reason-202 issue.

## Ordinary build and restoration

A separate ordinary build (no diagnostic/autotest flags) also passed the
source, configured-security, memory-policy, fixed-IDF and touch/clock checks.
Version `1.2.7-wpa3`, 5,038,496 bytes; SHA-256
`e9b55b44217a8cd4b3b9275cb988e6a88892eb40189bca57c611dbb86d6e80aa`;
ELF `b99aded8aa6066abba64b1b98a5e28ef2389a797ef3991dc1c50d7ccd28b5983`.
The final ELF has five external security-adapter call sites and no automatic
font runner, allocation wrappers, or copy-TX control symbols. Its image
checksum, digest trailer, chip and length were independently checked. This
ordinary binary is local only and has **not** itself been flashed/accepted.

After r2, the complete verified `1.2.7-bt4` application slot was restored.
Written digest and both outside-slot protection digests matched. Bootloader,
OTA selection, NVS/settings partitions and SD were not written by the installer.
The device is not intended to remain on an automatic-test image.

Source, diagnostics and sanitized records are collected on
`codex/plus-wifi6-wpa3-20261025` in the private `chinoryunqin/RickyOS-dev`
repository. Firmware binaries, raw logs, private test credentials and the
separate SD investigation are excluded. Release indexes are unchanged.

Final passive boot capture confirmed `1.2.7-bt4`, matching ELF prefix, and
`InxRecent` home, with no automatic runner, observed panic or SD init CRC error.
All serial capture/install sessions were closed. Sanitized evidence is in
`wpa3-device-restored.json`.

## Mac WPA3-only AP follow-up

The user enabled macOS Internet Sharing using an iPhone USB upstream.
System Settings confirmed **WPA3 Personal**, channel **11** (2.4 GHz).
The agent briefly stopped sharing to inspect those otherwise-disabled options,
then restarted it with the same network name, password, security and channel.
This is a new AP/network context, not a speed A/B against the prior router.

A new default-off private-profile diagnostic calls the same Arduino
`WiFi.begin` entry, requires a WPA3 auth threshold, and refuses bootstrap success
unless the connected AP record is `WIFI_AUTH_WPA3_PSK`. Credentials are provided
by the user for this local test, stay in a mode-0600 ignored header and temporary
private firmware artifacts, and are neither printed in numeric reports nor
written to the saved-network store. Ordinary builds do not include the header
or private-profile macro. Test profile failure does not fall back to a different
saved network; native bootstrap tests exercise this boundary.

The previously used legacy NAT preferences did not match the UI configuration;
they were not used for credentials or to infer actual encryption. The current
UI and the eventual on-device auth/SAE evidence are the relevant checks.

The first private-profile build was rejected before flashing: a source-file
property set from the project directory did not reach the component compilation
scope. The check of **actual** `PlusRadioAuto.cpp` compile arguments detected
that omission. The profile is now enabled with component target definitions
and a private include directory. `check_plus_private_network_profile.py` runs
after reconfiguration and rejects either a missing requested profile or a
profile unexpectedly present in an ordinary build. Native tests cover both
sides, saved-network fallback exclusion, and numeric WPA3 claim rejection for
legacy auth/missing security evidence/interrupted runs.

The executed private candidate is 5,049,664 bytes, SHA-256
`323774ad38bd2430374c41bdf35cf498e7c6d2fb19b77bd0598b2f6d1cca6667`,
ELF `49767b990c4cd0988b2168df006215e2ab3e7e6d6c975a28b41066b8edeb08d0`.
It uses the same fixed clean IDF and product WPA3 adapter, with reference TX and
b/g/n throughout; private firmware/ELF/build logs are not repository artifacts.

## WPA3-only AP device results

Private profile bootstrap joined in 3,915 ms and reported AP authmode 6
(`WIFI_AUTH_WPA3_PSK`), PWE method 3 (`WPA3_SAE_PWE_BOTH`), and a WPA3 auth
threshold. The configured PMF flag remained optional; the actual link logs
explicitly report `security: WPA3-SAE` and `pmf:1` on all 13 associations
(bootstrap plus 12 reconnects). This is actual SAE/PMF evidence, not an inference
from Wi-Fi 6 capability or build flags.

- 12/12 repeated WPA3-only associations and 12/12 font-list requests succeeded.
  Every round reports AP authmode 6 and HT20/mask 7. Reconnect totals were
  4,352–4,930 ms including radio teardown/reinitialization; the test loop took
  163,950 ms. These are not isolated handshake latency measurements.
- Both 1,763,452-byte font bodies passed read-back CRC 2837099957, with measured
  download times 8,949 and 10,833 ms. Temporary files were removed.
- All 12 small 9,024-byte index transfers completed, taking 705–5,213 ms; the
  slower sample is retained. The Internet upstream is now iPhone USB, so these
  timings do not isolate encryption cost or establish a Wi-Fi throughput ceiling.
- No observed allocation-wrapper failure, pbuf allocation failure, driver
  NO_MEM, panic or SD initialization CRC failure. Explicit teardown's other
  driver-interface errors remain in the private log.
- Three WeRead probes still require login. No account login/renewal was done;
  authenticated book operations are not counted as passed.

The numeric reporter requires the pure-AP bootstrap, all 12 AP authmode values,
a real WPA3 security log and completed request results before marking the
WPA3-only regression passed. See `wpa3-only-ap-r2-run1.json`; its
`wpa3_only_ap_regression_passed` is true. The test profile is never a fallback
for normal connections, and no saved-network credential is installed.

This validates one Mac WPA3-Personal AP on 2.4 GHz channel 11. Separate
H2E-only, Hunting-and-Pecking-only, WPA2/WPA3 transition networks (including the
historic reason-202 case), other router vendors, and wrong-password behavior
still need their own radio acceptance. Wi-Fi 6 remains disabled in ordinary
connections. Firmware images were not published or uploaded during this follow-up.

After the private-profile run, an ordinary build from the final sources passed
all configuration/source/IDF/touch/clock checks. The compile-command gate
explicitly confirmed private profile disabled. The final ELF has the security
adapter and no private bootstrap, automatic runner or allocation wrappers;
the binary contains neither the private AP name nor the WPA3 test log tag.
Ordinary application SHA-256 `0d9c20d56b47ff6bded1df96bd40d9918cdeaa623568db047dc010d005d56d08`, ELF `5a5d767677acaf1b6fc152d194e7d2596da90251f34a16b989445504d2f2df03`,
5,038,496 bytes. It has not itself been flashed or published; its separate
build record is `wpa3-normal-final-build.json`.

Final restoration confirmed the original `1.2.7-bt4` ELF and `InxRecent` home,
with no unattended runner, observed panic or SD init CRC error. App-slot and
outside-slot protection checks passed; serial captures are closed. The private
credential header was removed. The user's Mac Internet Sharing stays enabled
with the same security/channel/upstream. See `wpa3-only-ap-device-restored.json`.

## Access-speed readout

The 12 small-index transfers have median **816 ms**, minimum 705 ms and maximum
5,213 ms; eleven are 705–1,253 ms. The slow 5,213-ms sample is retained. The SD
write portions are 11–29 ms; this observation alone does not locate the remaining
wait within DNS/TCP/TLS/server/radio stages. Full font throughput including the
measured production download/write path is **159.0–192.4 KiB/s** (0.163–0.197
MB/s). This is neither the radio ceiling nor a paired WPA2/WPA3 comparison.
Normal connect bootstrap was 3,915 ms; repeated connect median 4,416 ms includes
radio reinitialization, so it is not an isolated SAE handshake time. Automated
UI round times also include navigation/painting/destruction and must not be
presented as pure download or first-page-open latency. Numeric summary:
`wpa3-only-ap-speed.json`.
