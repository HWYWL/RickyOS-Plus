# Plus 联网底层实现对照（2026-10-15）

对照对象：厂商交付的 2.0.62 原包（ELF cb62c770a…、IDF v6.1-dirty）与设备当前 RickyOS（ELF f2aa0ef55…、IDF 9a97f6c5）。完整同机日志与原包来源见 plus-vendor-exact-firmware-20261015.md。本文区分源码差异、运行参数和已验证结果，不把差异自动称为根因。

## 已验证的现象

同一台设备、SD 一直插着：厂商原包在手机热点取得 COS PNG 7865 字节、JSON 2252 字节，均 HTTP 200、证书验证成功，约 13.85 / 9.84 秒。恢复 RickyOS 后，热点 IP 为 172.20.10.2，COS DNS 得到 36.248.13.186，但两次 TCP 失败、0 字节；随后 CDN / Fastly 分别返回 2171 字节，目录共 17 项。全部图片已存在，未测本轮图片下载。

这是一组有价值的对照，但还不是固件差异的严格因果证明：两次运行时 SDK、协议、时序不同，记录到的信号强度也有变化（厂商热点 -55 dBm，RickyOS 关联记录有 -49 / -70 dBm）。后续同进程、同 IP、相同超时的交替 socket 对照更适合区分封装影响。

## 实际路径

厂商 HTTPS：SettingsTestUrlScreen::Fetch → Board::GetNetwork()->CreateHttp → HttpClient → EspSsl::Connect → esp_tls_conn_new_sync → getaddrinfo → lwIP socket/connect/select → mbedTLS → 接收任务 → HTTP body → 图片解码或文本显示。

RickyOS HTTPS：RickyWallpaperDownloadActivity::fetchManifest → HttpDownloader → SecureHttpClient → SecureClient::connectWithMethod → Network.hostByName → NetworkClient::connect(IP) → lwIP socket/connect/select → wolfSSL → HTTP body → HalStorage 临时文件 → JSON 解析。

注意：厂商 HTTPS 使用 EspSsl，不经 EspTcp 的明文连接函数。不能用 EspTcp 的 15 秒超时来代表厂商 HTTPS 的默认 TCP 等待时间。可见 IDF esp_tls.c 的默认连接等待为 10 秒；厂商未交付其 dirty SDK 源，无法证明其所有内部修改均与本地 SDK 一致。

## 具体差异

| 层次 | 厂商原包 | RickyOS 实机 / 可见源码 | 判断 |
| --- | --- | --- | --- |
| Wi-Fi 固件 | e12a754 | 75be901 | 实际二进制版本不同 |
| PHY | 100 / dd4c590 / Jul 14 | 101 / 7b1f880 / Sep 2 | 射频实现不同，不能只比较应用代码 |
| 协商协议 | 原包热点 WPA2-PSK / ax / BW20 | Plus 代码强制 b/g/n；运行 bgn | 有意的兼容策略差异，需控制变量验证 |
| 收包缓冲 | 交付配置 3 静态 / 6 动态，BA 6 | 实机 4 / 32，BA 16 | Arduino 在初始化时覆盖默认参数；sdkconfig 不等于运行值 |
| 普通 malloc 分配阈值 | 交付配置 ALWAYSINTERNAL=16 字节 | 现有对照构建为 16384 字节 | 16B–16KiB 普通分配的优先落点相反；有回退，不等于强制分配。f2aa 不能只凭另一构建配置推定，需运行验证 |
| 内部 RAM 预留 / PSRAM 频率 | 65536 B / 200 MHz | 现有对照构建相同 | 这两项未观察到差异，不应笼统归为“PSRAM 配置都不同” |
| TCP 窗口 | 5760 B | 实机 32768 B | 丢包/窗口协商行为可能不同，尚未定位 |
| SACK / MSS / RTO | SACK 关闭、MSS 1440、RTO 1500 | 对照构建 SACK 开启、MSS 1440、RTO 1500 | 记录配置来源；不能仅据一个构建推定未知 f2aa 的全部选项 |
| DNS | 原生 getaddrinfo，默认 AF_UNSPEC | Arduino Network.hostByName，再取一个 IP | Arduino 还管理 DNS cache 和全局 IPv6 优先检查；本次 COS 已得到 v4，所以不等于 DNS 失败 |
| TCP API | esp_tls 创建 socket，设置选项后 nonblocking connect/select，完成后恢复阻塞 | NetworkClient 创建 socket、nonblocking connect/select，完成后设置超时/恢复阻塞 | 二者最终都调用 lwIP；不能把区别描述成“一个阻塞 TCP、一个非阻塞 TCP” |
| TCP 等待 | 可见 esp_tls 默认单次 10 秒 | SecureClient 4 秒一次 + 5 秒重试 | 已知策略差异，尚未证实延长就能修复 |
| sockaddr 长度 | esp_tls 按实际 AF_INET/AF_INET6 类型长度 | Arduino 传 sockaddr_storage 长度 | 该 lwIP 同时接受两种结构长度；目前没有证据证明它导致失败 |
| TCP 失败日志 | 原生记录 select/SO_ERROR 和 errno | 上层主要记录 0/1，底层 core 日志可能关闭 | 需要更精确定位超时、路由或延迟错误 |
| TLS | mbedTLS / 证书 bundle，三个厂商配置开启 | wolfSSL，独立 CA / SNI / TLS1.3→1.2 策略 | 本轮 COS 失败在 TCP，未到 wolfSSL；暂不把 TLS 库称为根因 |
| TLS 内存 | 三项配置让 mbedTLS 使用外部内存和 bundle 回调 | 正常下载用 wolfSSL，这些选项不会自动替换其分配器 | “开启三个 mbedTLS 配置”不能等价于“修复 wolfSSL 下载” |
| 接收与 UI | 原包接收任务，整段小文件读入 vector；测试时暂停激活轮询 | 流式写入 HalStorage，取消/进度回调；活动与校时共用无线唤醒租约 | 并发、内存和调度不同，需与 TCP 本身分开验证 |
| 内核节拍 | 配置 100 Hz | 配置 1000 Hz | 前一轮家用网络对照单改 100 Hz 未解决，不能再把差异直接当修复 |

## 关键源码定位

- 厂商测试入口：.local/vendor-delivery-20261015/xingzhi-ai-470/main/display/screen/settings_screen/settings_test/settings_test_screens.cc:260（Fetch）、:302（Task）。
- 厂商 TLS：.local/vendor-delivery-20261015/xingzhi-ai-470/components/esp-ml307/src/esp/esp_ssl.cc:37（Connect）、:49（配置与同步连接）、:146（ReceiveTask）。
- 可见原生 TCP：tools/esp-idf-v6.1/components/esp-tls/esp_tls.c:221（解析及建 socket）、:301（socket options）、:390（TCP connect/select）。厂商实际 SDK 带 dirty 标记，此处仅作为可见实现基准。
- RickyOS：firmware/rickyos-plus-1.2/upstream/freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp:79（DNS/TCP）、:108（4秒 + 5秒）、:130（TCP 成功后才建立 wolfSSL）。
- Arduino TCP：.local/arduino-esp32-4.0.0-RC1/libraries/Network/src/NetworkClient.cpp:210；DNS：同目录 NetworkManager.cpp:47；参数覆盖：libraries/WiFi/src/WiFiGeneric.cpp:280。
- 原生对照入口：firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusNetSelfTest.cpp:65（raw socket）、:102（Arduino 6秒）、:140（native TLS）、:202（交替顺序）。

## 本轮验证安排

复用已经编译并核验的 headless 对照镜像 f68afcde…（同 IDF 9a97f6c5、同 b/g/n、Arduino 默认缓冲、三项厂商 TLS 配置、CPU 320 MHz），不修改生产下载器。它从 SD 内部读取最后使用的热点配置，不导出网络名称/密码；SD 保持挂载，不安装下载内容。

对同一轮解析出的同一个 COS IPv4，交替先测 raw socket 与 Arduino TCP，两者均 6 秒；再用原生 esp_tls 保留 COS 主机名、SNI、证书验证。该版本 native TLS 是异步 12 秒诊断，不与厂商同步原包混称。随后测正常下载器。手机热点、设备位置保持原样，排除“先测谁/连哪一个IP/连接超时不同”的一部分混杂。

使用现有受控诊断镜像，不新增应用堆分配；原有任务 20 KiB 栈与库内部临时分配在任务结束前正常释放。不会以 headless 的空闲内存表现直接推定正常阅读界面的表现。完整 Flash 重新备份，诊断只覆盖 ota_1，结束后恢复。


第一次启动自动诊断：镜像/首启匹配 d22a18303… / IDF 9a97f6c5，CPU 320 MHz，实际 RX 4/32、b/g/n。30 秒等待后 join status=6、无关联事件，测试退出；raw TCP / Arduino / native TLS 均未运行，不纳入请求成功率或底层连接结论。已请求用户保持手机个人热点设置页开启，待可见后重启相同镜像一次。当前原正常系统的完整备份 SHA256 29064afc1e18fc5e280887197f186fd2c53e6f48e5a7fc32d0d6c67fc9f37f75，已核验；启动程序/分区/OTA 选择/ota_1 以外区域未改。


内存阈值机制已按实际 SDK 源核对：tools/esp-idf-release-v6.1/components/heap/heap_caps.c:107 中，普通 malloc 小于等于阈值时优先 INTERNAL，大于时优先 SPIRAM，首选失败再退回其他 8-bit heap；带 MALLOC_CAP_DMA/INTERNAL 的显式分配不因此自动转入 PSRAM。厂商 sdkconfig:2315 为 16，本地可用构建 config/sdkconfig.h 为 16384。65536 字节内部预留及 200 MHz PSRAM 则相同。此机制差异可能影响内存压力和碎片，不能替代 TCP 丢包/超时实测；不直接更改全局阈值来声称修复，也不把更大可用内存的 headless 结果当作正常 UI 结果。


## 额外发现：成功首连后的超时未恢复

SecureClient.cpp:108–116 在首连前把 transport 超时限制到 4 秒，但恢复调用只位于“首连失败后重试”分支。若第一次连接成功，后续保留 4 秒。Arduino NetworkClient.cpp:407 / :491 又用同一个 `_timeout` 更新 SO_SNDTIMEO / SO_RCVTIMEO，所以它并非仅影响下一次建连。该问题应单独修正，不能解释本次 COS 两次 TCP 均失败（失败重试分支会恢复超时）的结果。本轮暂未改变生产连接器，避免与同镜像对照混在一起。


## 热点保持设置页后的复测

2026-10-15 第二次运行使用完全相同镜像（f68afcde… / ELF d22a18303… / IDF 9a97f6c5），复用完整备份并在安装前核验全 Flash MD5 一致；只写 ota_1，前后其他区域 MD5 未变。用户明确保持手机个人热点设置页开启。日志 `logs/plus-hotspot-transport-retry-run.log` 显示 CPU 锁定 320 MHz，30 秒等待后连接状态仍为 6，RSSI 0，没有关联事件，60833 ms 结束。未运行 raw TCP、Arduino TCP、native TLS、普通下载器。

两次自动诊断都未关联，故没有可供比较的同进程 COS 建连样本，也不能据此断言手机热点、COS、wolfSSL 或厂商底层存在故障。正常 UI `WifiSelectionActivity.cpp:552` 与诊断 `PlusNetSelfTest.cpp:303` 的连接流程并不完全一致：前者有 NetworkStartup 准备、停止旧 STA 后重建、全信道扫描及按信号排序；诊断直接初始化并 begin。后续应先核对连接目标与启动过程，待取得 IP 后才比较 TCP / TLS。未因该无效样本改变生产无线参数。

复测结束已恢复测试前完整主 Flash，全 16 MiB MD5 一致，无额外扇区变更。正常启动日志 `logs/plus-hotspot-transport-retry-restored-boot.log` 确认 ELF f2aa0ef55… / IDF 9a97f6c5 并进入 EpubReader；不留无界面诊断系统。外部触控固件仍为此前厂商原包更新的 V03，不在该主 Flash 恢复范围内。


## 正常界面连接热点后的有效失败样本

用户按正常界面执行屏保下载，串口 `logs/plus-hotspot-ui-reconnect-20261015.log` 先见原因码 201（IDF `esp_wifi_types_generic.h:170` 为 NO_AP_FOUND），之后成功取得 172.20.10.2 / 网关 172.20.10.1，用户报告“下载失败”。已取得 IP，故这一段是有效请求样本，与上面未关联的自动诊断分开。

| 请求 | 本轮实测 |
| --- | --- |
| COS wallpapers/cos.json | DNS result=-54，7019 ms；0 B，没有 TCP / TLS 握手 |
| cdn.jsdelivr.net | DNS 成功 1363 ms；4 秒 + 5 秒两次 TCP 失败；0 B |
| fastly.jsdelivr.net | DNS 成功 69 ms；第一次 TCP 失败，重试累计 7449 ms 成功；握手被调用者在总耗时 12002 ms 中止；0 B |
| github.com | DNS 成功 200 ms；TCP 累计 1082 ms 成功；TLS1.3 累计 2115 ms 成功；总耗时约 12002 ms 时仍为 0 B |

最后 Manifest ready=0，列表 0 项，总耗时 41573 ms。这次并非 JSON 内容解析/CRC/SD 文件写入失败，也不能把历史 Fastly 成功推广为始终正常。

### 新核实的预算差异

RickyOS `HttpDownloader.h:74` 的目录预算为 12000 ms；`HttpDownloader.cpp:179–181` 同时将它传作单操作超时与总预算，:64–70 根据剩余时间设置，:89–92 的回调在全流程耗尽时取消。这 12 秒包含 DNS、两次 TCP、TLS、重定向和正文。Fastly 本次 TCP 已用去 7.45 秒，余下约 4.55 秒后由该回调中止；不能把这条日志称为 wolfSSL 自身 12 秒握手失败。

厂商测试入口 `settings_test_screens.cc:265` 调用 SetTimeout(15000)，但 `EspSsl::Connect:49–55` 创建零初始化 esp_tls_cfg，只设置证书 bundle，没有把 HTTP 的 15 秒写入 TLS 配置。可见 SDK `esp_tls.c:581–605` 只有 cfg.timeout_ms > 0 时才做同步连接整体截止；默认 socket 接收/发送等待为 10 秒（:301–315）。其 HttpClient.cpp:609 等处的 15 秒是等待响应数据时单独计时，不等价于 RickyOS 的全流程 12 秒。这是已核实的策略差异；延长总预算是否能让 COS DNS / TCP 成功仍需独立实测。

正常下载器 `HttpDownloader.cpp:71–74` 对没有专用 rootCa 的公共资源调用 setInsecure，厂商则实际验证证书 bundle。不能认为正常公共下载会使用厂商三项 mbedTLS 证书配置，或因证书验证更严格而先失败。本轮没有改动这些策略。


## 连接器修正与第二代自动对照

已修正 SecureClient 的 TCP 策略：统一恢复 caller transport timeout，DNS 与两次连接共享剩余连接预算，避免接近截止时仍无条件再等 5 秒；仅 Plus 编译分支、无新增堆分配。实际生产片段的主机回归（首连/重试/预算耗尽/取消/millis 回绕，ASan+UBSan）及原 TLS 版本回退回归均通过。未声称它已经修复 COS。

第二代诊断增加与正常 UI 相同的 STA 停止/重建、全信道扫描与按信号排序，记录保存目标是否可见、扫描结果数量、begin 返回值与数字断线原因；网络名称/密码不输出。最多两次 45 秒连接。Arduino 的扫描结果在连接前 scanDelete，避免保留库分配的扫描数组。

对照改为两种 DNS API（顺序交替；共享 SDK DNS cache，不能拿耗时差直接当性能优劣），对同一解析 IPv4 的 raw socket / Arduino TCP 都用 15 秒，原生 TLS 使用厂商同名同步 API esp_tls_conn_new_sync，并显式限制 15 秒以便无人值守退出。原生 HTTP 只取状态行，完整 COS JSON/PNG 另由真实下载器读取。移除本轮无关的 LAN / 大文件测速，只做两个 COS JSON、厂商同一张 PNG、Fastly 小目录。所有数据只计数，不安装到 SD。

构建脚本 scripts/build_rickyos_plus.sh:14 原先仅 build，环境控制的诊断开关在复用构建目录时可能保留旧 CMake 配置；现先 reconfigure 再 build。最初产出的正常镜像未安装，第二次构建已核实 compile_commands 中两个诊断宏及 binary 的 headless / saved_target_visible 标记。有效诊断镜像 SHA256 7114e99e95ef8ec72b89c1338c34a65a49d3b075642a77af8df017cd7a1992c1，ELF 4bf1bdce…，3,381,712 字节，IDF 9a97f6c5。

新的安装前备份保留正常 UI 最近一次联网后的状态，16 MiB SHA256 708623a603d2754d6674878bf6dc480ec87f7cdbfe1b64b6f730cf0bb8f7ec95，私有文件 .local/device-installations/plus-network-policy-20261015/pretest-flash.bin。

第二代自动诊断两轮启动均未关联：用户重新打开热点设置页后的复测，扫描两次均 4 个 AP、saved_target_visible=0，均等待 45 秒后 status=6，128822 ms done。日志 logs/plus-network-policy-retry-run.log。未运行 DNS/TCP/TLS 对照，不能报告连接器修正已解决 COS；正常修正版已构建但未安装。后续需要先核对 SD 保存目标与用户手选热点是否一致，或在正常界面成功连接后触发测试，避免继续重复无效的自动启动。


## 正常界面再次手选热点，用户报告成功

日志 logs/plus-current-ui-network-20261015.log：当前仍是原正常 f2aa0ef55… / IDF 9a97f6c5（连接器新修正版尚未安装），热点 172.20.10.2，4728 ms 连接完成。COS DNS 成功，36.248.13.185 / 468 ms，但两次 TCP 均失败，0 B / 9501 ms；CDN DNS 成功后两次 TCP 失败，0 B / 6400 ms。Fastly 151.101.129.229，TCP 305 ms、TLS1.3 688 ms、HTTP200 / 2171 B / 1347 ms，最终目录17项 / 17360 ms。用户确认流程成功。

该成功明确来自 Fastly，不能标记 COS 验证通过，也不是新连接器修正的效果。保留串口会话避免下一次重新打开导致复位；后续应在这一有效连接后触发对照，不能再用未关联的 headless 启动替代同热点结果。


## 正常界面成功联网后直接对照

新增显式诊断开关 RICKYOS_PLUS_NETTEST_CONNECTED（仅 NETTEST=1、HEADLESS=0 可用）。main.cpp 的开机自动任务在此模式关闭；屏保下载 onWifiResult 内 requestUpdateAndWait / NetworkStartup::prepare 后，串行调用 plusNetSelfTestOnConnected。它沿用刚取得 IP 的现有 STA，不读取凭据、不扫描、不新建任务。HalPowerManager 性能锁与 HalWifiAwakeLease 仅在对照作用域持有，退出恢复。通过现有应用网络调用栈运行，两轮原生/Arduino解析和相同IPv4的15秒TCP对照、同步nativeTLS状态行，再回到原始完整目录下载。诊断原生TLS/HTTP的临时库内分配正常释放，不保留新堆对象。正常构建该入口调用与诊断源码不启用。

该修改旨在取得有效同连接样本，不代表已修复 COS；不会因为 Pico 已确认能访问而再次要求验证 Pico。


### 有效连接对照结果

ELF 0d4f28527…，SHA256 74e8a5ce9d83f4b7ed3a0022e468c4748fa265342fce973a8150a0550dac3071，正常界面、SD挂载，CPU320、RSSI -61，内部空闲90479B。日志 logs/plus-connected-compare-run.log。

- 第一轮同IP36.248.13.174：Arduino DNS136ms，原生DNS缓存1ms；rawTCP首个连接15秒超时(errno116)，Arduino随后690ms成功；原生mbedTLS1262ms完成握手且HTTP200，状态行读取512B、证书flags0。
- 第二轮同IP36.248.13.186：原生DNS375ms，Arduino缓存0ms；Arduino138ms、raw115ms均成功；原生TLS904ms、HTTP200、flags0。
- 对照之后正常wolfSSL下载器：同.186，TCP117ms、TLS1.3 1094ms、COS完整JSON2252B / HTTP200 /1652ms，17项列表 /1711ms。此次直接命中COS，无备用镜像回退。

该结果表明ArduinoTCP与wolfSSL都能在S31上访问COS，首次raw连接也会失败，不能把故障简单归于Arduino封装或加密库。对照顺序、网络就绪时间、DNS缓存以及前面几个连接可能影响后续成功；这不是未经前置请求的稳定验收。

据此准备下一份普通候选（无NETTEST）：Plus重试TCP由5秒增至最多15秒（仍扣除已消耗预算），索引总预算12秒增至30秒；首个短连接4秒保持。其代价是失效镜像最多等待更久，不能解释为底层根因修复。仅Plus、无新堆分配。新增回归覆盖长重试仍受全程预算约束。上机必须验证正常下载作为关联后的首个外部请求，并重复重启后验证；只有预热后成功不能作为该候选的成功证据。


## 无前置测试的首次 COS 请求：预算候选第一轮通过

候选 4,886,176B，SHA256 17e7395ed5d5c5f82ec19d31381a83e8967f91b29fde9f8793296e23ed6bb966，ELF0b59d3315…、IDF9a97f6c5。构建 compile_commands 确认 NETTEST / CONNECTED / HEADLESS 宏都未启用，二进制没有对照入口文本。只写 ota1，其他区域保持完整备份9b35a796…一致；此前对照诊断已完整恢复（包括改动NVS扇区0xb000）后才安装该候选。

日志 logs/plus-cold-cos-budget-run.log：正常界面连接手机热点4752ms，第一次请求即 COS wallpapers/cos.json。DNS36.248.13.174 /154ms；首个TCP4157ms失败；第二个累计16617ms成功，即这次新socket实际用了约12460ms，超过旧5秒限制；wolfSSL TLS1.3累计17402ms成功，HTTP200完整2252B /17875ms，目录17项 /17938ms。用户确认“成功”。未执行任何前置raw/原生TLS探测，也未回退其他镜像。

这证明候选时间预算修正可使该次原本会过早结束的正常请求成功，但不能证明驱动丢包根因已消除、所有请求稳定或下载速度问题已解决。已请求一次退出自动重启后的重复首请求验证，不要求再刷机。


### 普通候选第二轮：明确USB重启后通过

用户说明按返回并没有观察到重启，第一次退出不能算一次已确认的重启验证。随后本会话停止串口、仅通过USB进入ROM并watchdog复位、不写Flash；新启动日志明确ELF0b59d3315… / IDF9a97f6c5 / Boot→InxRecent，之后用户再次进入屏保下载。

日志 logs/plus-cold-cos-repeat-run.log：连接热点4786ms，COS DNS36.248.13.186 /191ms，首个TCP4194ms失败，第二个累计18170ms成功（第二个socket13976ms），wolfSSL TLS1.3累计19497ms，HTTP200完整2252B /20191ms，17项 /20254ms。无前置诊断/备用镜像。与第一轮17.875秒相比，普通候选已两次独立启动首次请求直接成功。

当前保留该普通候选，不留诊断固件；尚未提交/推送。三项聚焦回归（TCP预算、TLS版本回退、索引预算/取消/重定向）及完整固件构建、来源2826文件/79适配、链接触控/时钟归属检查通过。此次设备验收只覆盖手机热点COS屏保列表，未覆盖家用Wi-Fi、字体大文件、下载速度、校时。两次12.46/13.976秒的重试连接仍异常偏慢，延长预算是已实测有效的失败规避，不等价于消除S31底层丢包或锁定唯一根因。失败镜像等待上限变长（Plus每路目录30秒），为当前候选的明确代价。


## 慢建连包头追踪

新增诊断专用 PlusTcpTrace.cpp，通过链接器 --wrap 包装 ip4_output_if、ethernet_output、ethernet_input；不改 IDF/Arduino 源，不替换协议实现，调用原函数并原样返回。CMake 仅在显式 RICKYOS_PLUS_TCP_TRACE=1 时启用；LTO 下通过 -u 保留三个包装符号，已以 ELF nm 确认。普通构建无追踪宏/链接包装。仅记录端口443的SYN/RST包头（端口、标志、序列号、ACK、网卡编号）、ARP查询/回复的IP、原始错误返回；不读取应用正文，不留存帧。每次最多64B栈暂存，无新增堆分配。ASan/UBSan主机测试编译实际包装文件，覆盖SYN/SYNACK、普通ACK排除、短包、IP选项长度、ARP和原函数返回值。该观测层只能证明到lwIP/驱动网卡接口，不能证明无线空口实际发射/收到或云端收包；少量串口日志仍可能扰动时序，不能把本版耗时当严格性能基准。

备份为当前两次COS成功的普通候选，完整Flash SHA2565840b36a88cef85cca11c93776ee829da720068e3145fbcb0f3c792c695e9bf9。使用既有全备份+已验证普通应用+读回变化区域组合，并与设备整片MD5校验；包含当前NVS。诊断SHA256302ac1b2a2f1ddc7aa9bdf95bee887aae4d5cbff26562e18c0b6daf40937cc40，ELFa8d90311a…、IDF9a97f6c5，4887376B。仅写ota1、其他区域校验不变。日志logs/plus-tcp-trace-run.log。


### 包头追踪第一轮已定位到SYN/SYN-ACK等待

设备正常界面连接手机热点，WPA2-PSK / bgn / 关联RSSI -68。ARP对网关172.20.10.1的查询/回复成功，SYN发出前已有网关ARP回复（65043ms）。COS解析36.248.13.174。socket54461在系统时间65255/66505/68012ms发出相同seq1122106738的SYN，IP-TX和ETH-TX均出现，无错误返回，也未见匹配的SYN-ACK；应用在约4秒关闭。

socket54462从69273ms发出seq4275429750的SYN，之后约每1.5秒重复，共9个IP/ETH-TX；81275ms才收到端口443→54462、flags0x12、ack4275429751的SYN-ACK。随后TLS1.3顺利完成，COS HTTP200 /2252B /18054ms、17项 /18116ms，用户确认成功。原始私有日志logs/plus-tcp-trace-run.log。

因此这次不是DNS耗时、ARP未解析、lwIP未调用网卡、驱动接口返回错误、TLS计算或文件解析拖慢。时间主要消耗在有效SYN-ACK进入网卡接收入口之前；仍可能是发送之后/无线接收/热点转发/网络或对端，不能仅据ETH-TX==ERR_OK证明实际无线发射成功，也不能断言COS服务器是根因。软件包头追踪不是空口抓包。下一步若使用驱动发送完成回调，需要核对回调数据格式/所有权和非阻塞记录，避免日志本身拖慢Wi-Fi。


### 发送完成状态补充观测

SDK esp_private/wifi.h:949–961 提供 wifi_tx_done_cb_t / esp_wifi_set_tx_done_cb。显式追踪构建在普通列表请求前注册，回调仅把时间、长度、状态及可识别的Ethernet/IPv4 SYN头字段写入固定48×16=768B静态数组；单独计数全部回调成功/失败，满后不再留样本但继续计数。使用短portMUX临界段发布，无回调内打印/堆分配/借用指针保留。请求完成后停记并统一打印，回调对象应用终生有效。格式未识别的记录不作为某个SYN的发送证明；状态为成功也不能代替云端收包证据。主机测试新增超过数组容量、停止后不再记录等检查。

该追加诊断SHA256ad65621b0fc4659ba6071541d7802e11f480c73db9d99660f58c92eb65713561，ELFfd57163e2…，4888416B。沿用原始普通候选5840b36a…完整恢复备份；更新前旧诊断应用MD5/启动程序/分区/OTA选择核验，更新前后其他区域MD5一致。测试日志logs/plus-tcp-txdone-run.log。


## SYN 与发送失败时间关联（2026-10-16）

追踪 ELF839ae883a…，同一手机热点和 SD 挂载，普通 UI 请求直接 COS 成功，2252B、17 项，列表总耗时17319ms。DNS247ms；第一次TCP在4266ms失败，第二次TCP累计16232ms成功，TLS累计16564ms完成，即建连后握手约332ms。没有备用站回退。

驱动发送完成统计28次，成功15、失败13。lwIP记录两个socket共12次SYN；11条length=72的失败回调位于前11次SYN网卡输出后17–335ms。最后一次SYN于79730ms输出，79897ms收到匹配SYN-ACK。ARP网关回复63840ms，早于首个SYN63922ms。没有观察到IP/网卡接口非零错误返回。

有界128B扫描仍没有识别出回调IPv4/TCP头（format=0、端口/序列号均0）；因此**逐包SYN发送失败匹配未证实**。时间和长度关联很强，但不能把13次失败全部称为SYN失败，不能声称已证明无线空口丢包/热点未ACK或锁定硬件/驱动根因。SDK esp_private/wifi.h:942–949只说明txStatus=false表示发送失败，没有说明帧格式、失败具体原因、或云端是否收包。

原始日志仅保存在私有logs目录；公开整理为artifacts/plus-tcp-trace-20261015/txdone-match-filtered.log和txdone-match-results.json，不含SSID/凭据或应用载荷。下一项有判别力的实验是正常UI关联后，同一设备/网络/SD/地址下只改变无线协议或速率策略，比较TCP建连和发送完成状态；不继续重复修改包头识别并刷机。该对照本轮未执行，不能报告其结果。


### 本轮结束状态

已恢复测试前普通候选ELF0b59d3315…（30秒目录预算、15秒TCP重试上限），启动前整片16MiB与备份MD5一致；恢复了诊断期间变化的NVS扇区0xb000。随后watchdog正常启动，日志ELF/IDF匹配，进入InxRecent主页，无NETTEST/TCPTRACE；诊断固件已移除。外部触控芯片仍为厂商更新的V03，不在主Flash恢复范围。没有提交/推送。

后续协议对照应参考现有WifiSelectionActivity.cpp:578–582的历史HE失败记录，不能忽略已有实验后直接永久开启AX。当前正常代码显式BGN，厂商Start()只做默认STA初始化（wifi_station.cc:178–193），厂商有效日志曾协商AX；驱动二进制版本也不同。协议掩码只是可隔离的一个差异，不能据此直接认定根因或报告AX修复。此次没有改变正常无线协议。


## 新的协议对照进行中（2026-10-16）

详见artifacts/plus-radio-compare-20261016/report.md。诊断ELFa6287f9b4…已启动并由用户通过正常UI连接热点，固定COS IPv4后自动比较original/BG/BGNAX/BGN-repeat，每组两个TCP探测；结果尚待采集。已准备自动恢复普通固件，不能提前认定协议修改修复了问题。


### 协议对照已结束

original/BG/BGNAX/BGN-repeat四组首socket均15003ms左右超时；第二socket依次574ms成功、15002ms超时、983ms成功、3936ms成功。相应首socket驱动TX失败12/13/9/13次，成功的第二socket均0次失败。协议切换没有消除首连异常；数据/局限详见artifacts/plus-radio-compare-20261016/results.json及report.md。已自动完整恢复普通固件并验证主页，后续准备完整SDK版本对照。


### 厂商同代完整SDK对照完成

完整SDKfff9895c…、Wi-Fi e12a754/PHY100-dd4c590、同RickyOS/同自动探测，四阶段首socket均15秒超时；第二socket为94ms成功/BG超时/1445ms成功/3054ms成功。原生TCP和发送失败同样复现，旧SDK没有消除该症状。COS地址本轮.173，上一SDK.174，因此不能当严格同一peer速度对比。普通启动程序与应用已完整恢复并验证主页。证据详见artifacts/plus-vendor-sdk-radio-compare-20261016/。


### 时序对照候选已构建，等待Plus USB

0/5/20/0秒关联后等待对照ELFc7a15804b…已编译并通过实际源主机回归，但目标USB消失，仅检测到另一ESP32-S3；芯片校验拒绝安装，无Flash写入。S3识别复位后已恢复启动。Plus最后验证为普通候选主页，时序候选未安装/未运行。详见artifacts/plus-radio-timing-20261016/report.md。


### 无人值守时序对照已完成

用户离开前授权自主操作；自动选择当前可见保存网络成功，绕过DNS、固定36.248.13.174，BGN/PHY4/RSSI-56/无线省电关闭一致。关联后0/5/20/0秒等待四组的首socket全部15003ms超时；第二socket1344/1113/1112/3026ms成功。空闲等待20秒没有避免首连失败，不能把“关联后延迟发请求”当修复。原始TX身份尚未匹配，只能说首次实际发流量后的状态变化更值得检查。普通固件已完整恢复/主页验证，详见artifacts/plus-radio-timing-fixed-20261016/。


### 聚合/固定速率对照及数据帧确认

关闭TX AMPDU、固定6Mbps未观察到改善；normal-before首15秒失败/第二4.353秒成功，AMPDU-off及fixed6各两次均15秒失败。76条失败回调均被AP/STA地址验证识别为加密QoS Data（type2/subtype8），不是精确TCP匹配。normal-repeat未关联且测试当时未显式清除fixed-rate，因此反向对照不完整，不作严格因果结论。已修正清理逻辑；本轮实际普通Flash恢复及主页验证成功。详见artifacts/plus-radio-rate-20261016/。


### 无人值守功率对照和实际功能验收（2026-10-16）

已找到可重复的功率相关差异：同一AP/同一COS地址/正常AMPDU与自动速率，两次独立启动，5dBm首TCP114/116ms且TX失败0；20/13dBm首TCP15秒超时。恢复20dBm后第一轮再次超时，第二轮14.810秒成功但12次TX失败。不能将第二轮恢复组说成严格超时。仅支持功率相关链路异常，供电/天线/校准/PHY唯一原因未分离。

在已有共享HalWifiAwakeLease加入Plus限定的运行时上限min(原值,20 quarter-dBm)，最后使用者退出恢复，并覆盖失败回滚/多持有者/外部修改。实际生产下载和校时接口、正常DNS、SD挂载、每次完整Wi-Fi重连验收：COS屏保列表827ms、字体列表676ms、1,763,452B字体6702ms且CRC32读回一致、原生NTP1096ms成功、再次冷连接屏保列表565ms。四次下载TX累计失败0、进度有推进，结束租约归零/原功率恢复。字体为独占创建的临时下载验证，未覆盖整家族安装UI；远距离及其他AP未验收。详见artifacts/plus-tx-power-acceptance-20261016/report.md和results.json。


### 当前保留固件

普通TX-power兼容版SHA2569287f68b…/ELFb1006c937…已写入ota_1并读回一致，启动至InxRecent；所有自动测试/追踪宏关闭，仅启用联网租约功率兼容。启动程序、分区、OTA选择和槽外Flash保持一致。原始普通版恢复备份仍保留，诊断镜像已移除。见artifacts/plus-tx-power-compat-20261016/report.md；尚未提交/推送。
