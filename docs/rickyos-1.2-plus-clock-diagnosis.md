# Plus 校时排查（2026-10-11）

RTC 已验证写入、读回以及重启恢复，不能据此认定在线同步正常。用户报告当前 Wi-Fi 上手动 NTP、关闭无线省电、HTTPS Date+Age 备用均失败；不要求更换热点。

证据：`logs/rickyos-1.2-plus-clock-https-boot.log` 在 110048 ms 报 `NetworkManager.cpp:138 hostByName` 返回 -54，随后 HTTPS status=-1。lwIP EAI_FAIL=202 被框架的 8 位 err_t 截断为 -54。框架 `NetworkClient.cpp:310` 仅检查零，因此负值继续连接空地址；本次 Plus HTTPS 调用先显式要求 DNS 返回 1，避免这个无效 TCP 等待。TLS 信任根和主机名验证保持原有实现。

下一版增加 `components/rickyos_application/port/PlusNetworkProbe.cpp`：在备用 HTTPS 前记录 DHCP 地址、子网掩码、网关、DNS 和信号；并发发送标准 DNS A 查询到 DHCP DNS 与 119.29.29.29、NTP 请求到 119.28.183.184，以及 TCP 连接 OTA 域名当时解析出的 185.199.108.153:443。等待上限 2.2 秒；所有临时 socket 离开函数时关闭。使用栈上请求/响应，socket 内部缓冲为 lwIP 所需临时资源。不记录凭据，不修改 DNS，不使用探测回复设置时间。探测只用于区分地址配置、DNS、UDP 和 TCP 失败，不代表某个公网地址永远可用。

依据源代码位置：HalClock.cpp:326 进入备用校时；PlusHttpsClock.cpp:10 实施备用；框架 NetworkManager.cpp:47–138 和 NetworkClient.cpp:310–316 定义 DNS 返回与 TCP 判断。两个真实 HalClock/TrustedTime 主机回归通过，设备来源及 ELF Plus 触控/校时分支检查通过。最终连接结果待实机日志。

## 字体列表与网络协议排查

用户回程后复现字体列表失败，`logs/rickyos-1.2-plus-font-list.log` 记录三个域名均 DNS EAI_FAIL (-54)。电脑同时访问三个字体列表均返回 200，设备 TLS 预检通过（内部空闲约 93 KiB、最大块 56 KiB），因此这次不是目录不存在或 TLS 内存预检拒绝。

同一日志 115788 ms 的探测显示设备 IPv4 192.168.5.6、网关/DNS 192.168.5.1、掩码 255.255.255.0、RSSI -53。发往 DHCP DNS、119.29.29.29 DNS、119.28.183.184 NTP 的 sendto 分别返回完整请求长度且 errno=0；数字 HTTPS 443 connect 返回 EINPROGRESS，随后四项在 2.2 秒内均无回复。发送返回成功不等于无线帧已被 AP 转发。此次不能单凭 DNS 错误推断是 DNS 配置。

驱动关联日志为 11ax、he:1。ESP-IDF esp_wifi.h:718–739 确认 HE 支持目标在 2.4 GHz 下默认 B/G/N/AX，允许显式 B/G/N。Plus WifiSelectionActivity::attemptConnection 在 disconnect(true) 之后恢复 STA，并在 WiFi.begin 前设置 B/G/N，记录设置/读回返回码与掩码。保持当前 AP/凭据不变，用于验证 S31/当前 AP 协议兼容性，根因仍待实机结果。其它板型连接流程保持原样。

SecureClient Plus 分支增加正常日志入口，避免 Serial 的 DTR 状态条件隐藏 DNS/TCP阶段；首次 TLS 请求也触发现有探测。无新常驻堆缓冲、未改 TLS 主机名/证书校验策略。新构建与硬件结果见 network-bgn 版本日志。

## 2026-10-11 B/G/N retry and delayed replies

B/G/N protocol set/read succeeded (`mask=0x07`), AP mode `11bgn`, HE=0. User still reports font list failure, so this does not establish a fix. The 2.2-second probe timeouts are only bounded observations, not proof of complete network failure.

Later in the same `network-bgn-boot.log`, fastly DNS succeeded at 105390 ms, TCP connected at 122185 ms, and TLS request failed at 123896 ms. At 137626 ms an NTP reply arrived and the system/RTC were synchronized. Host en0 address 192.168.5.3 pinged device 192.168.5.6 three times, all replied (35–135 ms); ARP MAC matched 1c:29:04:d0:2a:50. DNS/TCP/NTP are intermittent or delayed, not universally unavailable.

Next candidate logs Plus TLS allocation, CA load, fatal error string, caller abort and deadline without depending on USB DTR. Certificate verification and original hostname/SNI remain enabled. It is a diagnostic change; acceptance pending.

## TLS diagnostic build: font list accepted by user

Installed image SHA256 `55cde9b234067a4bc56232c8ea475e4eab0eb59b432893369b6b4a7b9fa077c4`, ELF `385a301c8`, factory unchanged. User reports “这次可以了” to the font catalog loading check.

Same boot log (`logs/rickyos-1.2-plus-network-tls-trace-boot.log`) shows DHCP 192.168.5.10, public DNS raw response with one answer at 431136 ms; first CDN DNS attempt failed, second succeeded at 440900 ms; CDN TCP connected at 454656 ms. NTP reply at 453579 ms reports UTC 1791726483. The 480-second capture ended before CA/TLS/manifest completion, so those completion stages are confirmed only by the user's visible catalog result, not captured individually in serial.

This successful attempt does not establish the root cause or sustained reliability. The latest change added diagnostics, rather than a network behavior fix. Actual font file download, repeated catalog loads and manual clock sync remain unverified. Keep the current image; do not initiate another speculative flash just because diagnostics are incomplete.

## Font installation failure reported after catalog success

User reports font installation failure. Catalog success is not full download/install acceptance. Fresh serial capture started in `logs/rickyos-1.2-plus-font-install.log`; opening USB restarted the current application, without flashing. Awaiting reproduction and font family name.

Host sanity check for public catalog first file `RickySans_12.cpfont`: HTTP 200, 1,763,452 bytes, CRC32 2837099957, CPFONT magic all match current manifest (`logs/rickyos-1.2-plus-font-package-host-check.log`). This validates that specific remote package only, not every font or device storage. Do not change installation logic without identifying the recorded failure stage.

## Reproduced font catalog failure with TLS diagnostics

User's next attempt failed at catalog loading again. `logs/rickyos-1.2-plus-font-install.log` records CDN DNS failures; Fastly TLS auto failed with -308 (socket error) and TCP down, then TLS 1.2 succeeded at 231336 ms but no HTTP status was obtained before timeout at 291340 ms. GitHub TLS 1.3 succeeded at 306981 ms, followed by another HTTP transport/status failure at 342603 ms. Heap at failed handshake remained 82,159 bytes, largest block 47,104 bytes. NTP reply arrived at 243713 ms. No font installation/CRC failure was reached in this reproduction.

Next controlled compatibility test changes only Plus station mask from B/G/N (0x07) to B/G (0x03), avoiding HT/AMPDU negotiation. The AP previously negotiated aggregation with window 64. This is a transport compatibility hypothesis, not a proven root cause. Same application/SDK, TLS and endpoints remain in use; compare response latency and full font download on the same network.

B/G comparison image installed: SHA256 `9dd5b601b5923c401626476628ec34f6449e3701f5f930206fd06a8eb0cba876`, 5,129,120 bytes, ELF `394d76ca6`. Application read-back verified; factory slot unchanged; OTA1 selected. Boot entered InxRecent and restored RTC. Continuous capture remains active for 1200 seconds in `logs/rickyos-1.2-plus-network-bg-boot.log`, awaiting same-network catalog and font install retry. No success claim yet.

## B/G download progress, not a total download failure

B/G mode verified (`mask=0x03`, `11bg`, HT=0). DHCP/public DNS probes responded in 20/70 ms. Primary CDN TCP aborted/reset; Fastly loaded the manifest (TLS1.3, 3255 bytes, four families parsed).

RickySans_12 first CDN attempts reset with errno 104. Fastly TLS handshake completed in 656 ms; the 1,763,452-byte font completed after 169,279 ms at 358600 ms. The next RickySans_14 file began at 359863 ms, which means the preceding CRC/magic/rename installation flow returned success. Its Fastly handshake completed in 2199 ms. No whole-family success yet.

UI progress tracks the sum of six sizes and refreshes only each 10 percent (`FontDownloadActivity.cpp`, `updateDownloadProgress`). The first file represents less than 10 percent, so a long 0-percent display can appear stalled despite transfer progress. User was told not to reboot/flash during this ongoing transfer, and asked for exact current progress/error state. This observation does not imply underlying transport reliability is solved.

## Transfer diagnostics prepared while preserving active download

User confirms download has started but is extremely slow. Second file completed and the third began in the active BG firmware, so do not interrupt this successful ongoing transfer. Prepared candidate only: `dist/RickyOS-1.2.0-plus-design-24c05ff7-font-transfer.bin`, SHA256 `eaf3cf841be693a4524657cd872ca5dcaad9d749081516df477877b73514ce7f`, 5,129,552 bytes. NOT installed; active manifest still refers to network-bg firmware.

Changes: remember a successful file host by swapping existing base/mirror strings, avoiding repeated failed primary connections on subsequent sizes; Plus-only per-download accumulated SD write and progress callback times. Counters are stack-owned, callback captures remain two references, no per-chunk allocation/logging; summaries appear on request completion. This removes known failed-host retries and allows measuring processing vs transfer time; it is not a demonstrated steady-throughput fix. Source/build guards passed (2758 pinned files, 30 adaptations), linked touch/clock owner checked; image checksum/hash valid. Next hardware installation deferred until ongoing family download ends or is cancelled.

## 2026-10-12 transfer candidate installed

User confirmed cancelling the previous download. The old capture ended after 1200 seconds; it confirms sizes 12, 14 and 16 installed and size 18 had transport retries, not complete-family success.

Installed prepared transfer candidate SHA256 `eaf3cf841be693a4524657cd872ca5dcaad9d749081516df477877b73514ce7f` with readback verification, factory slot unchanged. ELF `aab6dc827` entered InxRecent and restored RTC. Timings and Plus touch symbol verified in linked ELF. Active capture `logs/rickyos-1.2-plus-font-transfer-boot.log` runs for 3600 seconds to cover the slow remaining family transfer. Awaiting user retry; no throughput improvement claimed.

## RickyGrin retry and LAN upload failure evidence

User selected RickyGrin instead of RickySans, a valid transfer test. Current capture skipped verified RickyGrin_12, then primary CDN reset TCP. Fastly DNS failed once, TLS1.2 succeeded after fallback; subsequent resumable attempts included a 60-second TLS handshake timeout and later TLS1.3 success. Full file timing not yet emitted. Host downloaded the exact Fastly RickyGrin_14 URL: 1,332,884 bytes in 6.552 s, CRC32 3677988872, matching catalog (`logs/rickyos-1.2-plus-rickygrin-host-speed.log`). Host/device paths may differ; this does not prove a common transport root cause.

User supplied `/Users/rick/Downloads/IMG_1881.HEIC`, showing local web file upload failure “上传过程中 WebSocket 已关闭” under `/books`. This indicates browser/device WebSocket closure, not a font format error. LAN upload does not depend on the font CDN or TLS path. Could share Wi-Fi/storage/resource issues, but requires its own server log. Requested cancel active slow font transfer and reproduce the failed book upload while the existing serial capture stays open. Do not reopen serial or reset during reproduction.

## No-data cancellation bug fixed and installed

User could not cancel RickyGrin. FontDownloadActivity polled input only in the download body progress callback, so stalled TLS/HTTP waits did not update the cancel flag. Added an optional input polling callback to HttpDownloader; the existing abort checks now poll at most once per 20 ms during handshake, response/body waits and retries. DNS/TCP blocking calls can still delay cancellation. Transfer input timing includes this polling. No changes to LAN upload behavior yet.

Focused host test extracts the actual abort lambda and checks cancellation without incoming chunks, polling throttle, optional callbacks, redirect rejection and timer wrap; passed. Build and pinned source/linked touch/clock checks passed. Installed image SHA256 `4cf87633bdcb5395a10dabfd29f0b6cf12b04072f428e4b3dc86f12470ef52ef`, 5,130,128 bytes; readback verified and factory preserved. ELF `946731446` restored RTC and entered InxRecent. Continuous capture: `logs/rickyos-1.2-plus-font-cancel-boot.log`. Physical cancellation and LAN upload retry pending; slow transfer and shared network root cause remain unresolved.

User confirms cancellation now exits, but feels laggy. Capture confirms Fastly DNS took ~2 seconds and TCP connection ~5.8 seconds before TLS caller abort at 196523 ms; request returned cancellation result 3, then UI redraw took ~2.1 seconds followed by ~1.4 seconds. Button press timestamp is unavailable, so these durations are not exact button-to-exit latency. Cancellation path now works; blocking connect and display refresh remain latency contributors. Awaiting LAN upload reproduction with font download stopped.

## LAN uploads complete; file page stalls and overlapping radio owner defect

Font-cancel capture shows both EPUB uploads completed by declared byte count: 758,914 bytes in 50,811 ms (14.6 KB/s), then 1,414,335 bytes in 58,029 ms (23.8 KB/s). The browser then requested `/files`; the compressed HTML body stalled and aborted after 15,979 ms, then again after 10,056 ms. No device reset or server stop. Independent direct host curl received HTTP 200 but only 12,880/51,489 bytes within 15 s, reproducing a partial page response. This does not establish book content integrity beyond the byte count.

Definite ownership bug: web server disables power save at 305741 ms, but the NTP exchange begun earlier restores MIN_MODEM at 339275 ms while the server still runs. Added Plus-only HAL radio-awake leases shared by clock, web service and HTTP guards. Static mutex/storage, no heap allocation, synchronized owner count supports non-LIFO completion. Only the final owner restores the initial mode. Synchronous HTTPS clock fallback is deferred while another owner is active to avoid blocking the web event loop. Other targets retain their existing radio guard.

Real HalClock host lifecycle tests cover overlapping web/download owners, cancellation, success, deadline, double acquire/release and existing awake state. Focused download cancellation and shared clock-owner tests pass. Candidate SHA256 `3bb9c6a1c756288e298a86c829360cc327ad99337adf043ed53de393c8551be5`, 5,130,576 bytes; ELF `8332cc1838`. Build guards pass (2758 pinned files, 34 source adaptations; HAL lease header is port-owned). Hardware page throughput and upload retry remain pending.

Radio ownership candidate installed with readback verification and factory unchanged. Boot capture confirms ELF `8332cc183`, RTC restore and InxRecent. Capture `logs/rickyos-1.2-plus-radio-owners-boot.log` remains active. Asked user to enter web transfer and refresh file list before additional uploads; physical responsiveness and throughput remain pending. The extended clock test also verifies HTTPS fallback is deferred during another awake owner and resumes after its release.

On-device overlap verified: NTP completed at 47359 ms, then shared lease logged `Radio awake retained: owners=1` at 48732 ms while web server remained active. Direct curl received complete HTTP 200 `/files`, 51,489 compressed bytes in 7.256 s; gzip decompressed successfully to 216,373-byte HTML. `/api/files?path=/books` returned both uploaded books with matching declared sizes 758,914 and 1,414,335. This verifies persistence and sizes, not binary CRC/content. A separate `/files` response still aborted after 148 ms; its exact client/reason is not identified. Browser responsiveness and new upload throughput are pending; do not claim all network issues fixed.

User confirms web page and file list now work after shared radio ownership correction. Record this as browser file-page acceptance only; further upload stability and font throughput remain pending.

Capture additionally confirms a new upload of 1,768,639 bytes completed in 59,659 ms (29.0 KB/s), followed by a file listing with three entries. This is a successful new transfer, not proof of expected throughput. Requested one-minute RickyGrin retry after leaving web transfer, keeping continuous serial capture.

## Reliable font progress, remaining throughput limit

User confirms progress but slow. Radio-owners capture completed and installed RickyGrin sizes 14, 16, 18, 20; next-size transition proves CRC/magic/rename succeeded. Size 12 was already verified. Size 22 is still downloading. Timings:

| Size | Bytes | Total ms | SD write ms | Input/progress ms |
|---|---:|---:|---:|---:|
|14|1332884|111159|1157|15271|
|16|1694195|133101|713|18288|
|18|2082422|165352|1830|22898|
|20|2494862|189752|1552|26081|

SD write time is small; input/progress ~14% and the remaining transfer/TLS path dominates. Do not describe the remainder as pure network wait before measuring read/decryption time. Successful Fastly host is reused between sizes, handshakes below one second. Current run is not interrupted.

Prepared body-reader timing using a port-owned stack struct, wrapping actual Client.read in SecureHttpClient readFixed/readUntilClose; logs nonpositive read count and accumulated read-call time at body-reader scope end. An initial per-SecureClient counter implementation failed fixed WeRead workspace size assertions; removed it completely. Stack counters avoid growing HTTP/TLS objects or allocating heap memory. Rebuild passes pinned guards, linked touch/clock checks. Candidate manifest is separate (`manifest-tls-read-timing-candidate.json`), NOT installed and active manifest remains radio-owners.

RickyGrin_22 completed: 2,997,768 bytes, total 235466 ms, SD write 2782 ms, input/progress 32321 ms. At 1150130 ms log confirms `font family installed` and registry found six RickyGrin files, proving all sizes passed CRC/magic/rename. Full family installation verified in log; speed remains ~12–13 KB/s. Prepared timing candidate will be installed only after this successful completion, not mid-download.

After recorded family completion, installed body-read timing candidate SHA256 `872c32d43ac14b70fec4801ddb199fec1a15b59946a7f310421aba24a025c5f0`, 5,130,544 bytes, readback verified and factory preserved. Boot ELF `282ca6567` entered InxRecent. Active capture `logs/rickyos-1.2-plus-tls-read-timing-boot.log`; next test is 30-second remaining RickySans download followed by cancellation to emit body timing without waiting for a whole family. No throughput fix claimed.

## Partial font read timing: mostly no data available

User cancelled short RickySans test. Fastly TCP connect took ~10.5 s, TLS completed ~1.6 s later. Body timing: 15,158 bytes, 5422 calls, 5411 nonpositive reads, 551 ms accumulated Client.read time, 12,724 ms body scope. Whole request 35,488 ms, SD writes 21 ms, input/progress 3423 ms, cancelled result 3. This points to waiting for data rather than SD writes or TLS read/decryption CPU being the dominant cost. It does not identify the exact network layer responsible.

The B/G-only restriction was an earlier controlled comparison and did not fix intermittent/slow TCP. Next candidate restores B/G/N mask 0x07, keeps HE/11ax disabled and retains shared radio leases and all read counters. Compare same endpoint and remaining RickySans file; do not claim throughput gain before hardware results.

Installed B/G/N body-read timing image SHA256 `3c957abec94ae603f8949912283baa115b7b41da46e0571d443f023f6bce5703`, 5,130,544 bytes, ELF `de3240fcc`, application readback and factory preservation verified. Entered InxRecent, RTC restored. Capture `logs/rickyos-1.2-plus-bgn-read-timing-boot.log` active for same-file RickySans short retry. Protocol negotiation still requires a Wi-Fi connection; no throughput conclusion yet.

BGN negotiated 0x07/phy bgn. First catalog attempt eventually got Fastly manifest after 69.3 s; user retry then failed all three hosts with DNS -54, TCP delay and TLS -308 socket error. No valid same-file throughput comparison obtained. Reverting to previously verified B/G image 872c32..., retaining read timing and shared radio leases. Next isolate LAN transport using existing web upload/download endpoints and uniquely named temporary binary data, verify hash and remove only the test file. No existing books will be modified.

## LAN preflight itself stalls while server loop remains alive

After USB reinsert, capture resumed after device startup. Web service started at IP 192.168.5.10; shared radio lease retained awake after NTP completion. Local benchmark cannot yet reach the file upload stage: Node fetch connection timeout, curl HTTP/1.1 got full 618-byte file-list JSON but did not terminate within 10 s. Device reports serving the list in 12–14 ms and continues periodic handleClient logging without reported WiFi loss. A separate HTTP/1.0 request completed in 6.9 s with 630 bytes (chunk framing included despite HTTP/1.0; not valid JSON as-is). Later raw sockets to both ports 80/81 report macOS errno 64 Host is down. No test data bytes uploaded (curl upload bytes=0). Current evidence supports transport loss/unresponsiveness, not a proven STA disconnect event.

Preparing Plus HAL diagnostics: one lifetime Arduino WiFi event subscription logs numeric disconnect reason and STA link/IP events; web handleClient periodic log adds status, driver AP lookup result/RSSI/channel/HT, power-save and free heap. No SSIDs/credentials logged, no per-request allocation; AP record stack-owned. Existing service health logging polls WiFi.status only and omits driver link/error details. Hardware capture is required to distinguish associated-but-unresponsive transport from explicit disconnection.

WiFi link diagnostics built and installed SHA256 `bf0c6aeb2a3e807505d3bfb51d8cc0eaecaee776c914f0b318e3667c391be38d`, 5,131,408 bytes. Readback verified, factory unchanged, ELF `987b688fa` entered InxRecent. Capturing `logs/rickyos-1.2-plus-wifi-link-boot.log` for next web-service reproduction. No network settings changed; only numerical connection/driver/event logs added.

## Associated STA, slow/lossy LAN transport reproduced

Diagnostic capture `logs/rickyos-1.2-plus-wifi-link-boot.log` reports WL_CONNECTED/status 3, driver AP lookup success, channel 7, HT disabled and power-save NONE throughout the first LAN failure/slow-upload interval; no disconnect event logged. RSSI varied from about -62 to -76 dBm. This does not establish a healthy transport link or rule out driver/radio problems.

WebSocket upload of uniquely named 262144-byte temporary data completed in 17449 ms (14.7 KB/s). Node HTTP readback terminated before hash verification, so byte integrity is NOT verified. The script successfully deleted its own file. A later curl 404 occurred after deletion and is expected. The second HTTP test was interrupted by the operator; firmware logged upload aborted and removed its partial file. Root listing subsequently returned nine entries; neither temporary file remains. No user books modified.

Host ARP maps 192.168.5.10 to the verified device MAC 1c:29:04:d0:2a:50. A five-packet device ping sample got one reply; a previous three-packet gateway sample got all replies. These are small diagnostic samples, not a long-term loss estimate. User moved device closer to router for same-firmware/same-payload comparison. Vendor station code uses default TX power unless an optional NVS max_tx_power override exists; no board-specific antenna GPIO configuration found in board source. No firmware network setting changed this round.

Near-router comparison: user confirmed relocation. RSSI during attempt -64/-65 dBm, status 3/AP lookup success/PS NONE. Same 256 KiB WebSocket upload closed before DONE; firmware removed its incomplete unique file at 401141 ms. Cleanup's subsequent not-found response is expected. No STA disconnect event recorded. This modest RSSI improvement did not resolve the failure, and does not rule out RF problems. Preparing controlled default-protocol negotiation candidate: remove Plus B/G override, matching vendor wifi_station.cc Start() which starts station without esp_wifi_set_protocol. Existing radio leases, timing and driver diagnostics retained; no new allocation.

Default-protocol candidate build passes 2758 pinned file checks/35 source adaptations, linked Plus touch and clock owner checks. Image checksum/hash valid, 5131392 bytes; SHA256 d921f0724c3a37dc5d30a44ef283648a5edbf81f0ea51ac34c51b9f283f28125, ELF d0d1387c25cd99774839b84306974ca893d51c95999d0a0459a3ad2fa544de89. Controlled ota_1 installation started after factory/bootloader/partition-table preflight. No performance conclusion yet.

Default-protocol image installed: application readback hash verified, factory full-slot MD5 unchanged, OTA selected last. Boot ELF d0d1387c2 confirmed, RTC restored, InxRecent entered. Continuous capture logs/rickyos-1.2-plus-wifi-default-boot.log. Waiting for user to reopen web transfer near router; actual negotiated protocol and transfer improvement still unverified.

## Network investigation deferred by user (2026-10-12)

Default protocol mask 0x47 negotiated AX. During web service, STA status=3/AP lookup success/PS NONE; RSSI approximately -65..-70 dBm. Earlier reason 8/2 events occurred during connection attempts, before web service readiness, not during the benchmark. Device ping short sample 9/10 replies versus gateway 10/10; this is not long-term stability evidence. WebSocket connection failed without a server START/upload event, so no new temporary file was created. Direct no-proxy curl TCP port 80 timed out after 5 seconds, zero response bytes. No throughput improvement established.

Host VPNLite tunnel process observed; system HTTP/SOCKS proxies disabled and route to device via en0. Tunnel influence is unproven. User chose to defer the issue before VPN-off comparison; no VPN or host network settings changed. Stopped continuous serial capture. Current installed default-protocol image d921f072... remains booted with factory slot preserved. Manual clock sync and network stability remain unresolved. Resume from these records rather than repeat completed hardware checks.
