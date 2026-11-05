# Plus 网络对照：2026-10-15

Plus 专用分支已快进到 `5474dac7`，包含七个新提交，应用来源固定 `12d1d95e7d7ff17134eb29082209df2aa9810722`，SDK 仍为 `96de1be6`。从固定对象还原 2826 个来源文件、79 项适配并校验通过；本地保留此前源码和端口文件备份。当前诊断修改尚未提交。

## 已确认的设备状态

- 目标 MAC `1c:29:04:d0:2a:50`，本轮串口 `/dev/cu.usbmodem101`。
- 读取到启动程序和原运行应用均为 IDF `9a97f6c5`，原应用 ELF `f2aa0ef554843059e7bc9cfc5e7e97875ca9d5e147d1c4d902a2a3435460c157`。
- 完整 16 MiB Flash 已保存并校验，位于 `.local/device-installations/plus-release-v61-before-nettest-20261015/`。备份和原始串口日志不用于公开提交。
- 当前启动选择 seq=8，对应 ota_1；ota_0 已没有原厂应用描述头，不能再使用旧的「恢复原厂槽选择」脚本。新诊断安装只覆盖 ota_1，核对当前应用身份，并校验启动程序、分区、启动选择和槽外区域未改变；保留原 f2aa 应用供恢复。

## 厂商建议与诊断配置

用户转述厂商：开源代码无专用补丁，开启以下配置应可访问。用户提供的仓库为 [CloudZao/Metalio-E-INK4-Plus](https://github.com/CloudZao/Metalio-E-INK4-Plus/commits/main/)。本轮查询到最新提交 `b400605e9a6a`；提供的本地厂商包 HTTPS 使用 `esp_tls` 异步接口与证书包。另读取该远程提交的 HTTPS 实现，其当前版本使用 `esp_tls_conn_new_sync` 和证书包，不能把旧本地包当作当前远程实现。

```text
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_MBEDTLS_X509_TRUSTED_CERT_CALLBACK=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y
```

本地原构建配置为内部 mbedTLS 分配、可信证书回调及交叉签名验证未开启。诊断使用独立 IDF `9a97f6c54ec638111ce55cd36581b3c192f15207`、独立构建目录和配置文件，三项实际配置确认均为 y；没有用旧 sdkconfig 假定新 defaults 已生效。现有下载仍走 wolfSSL，因此同时加入原生 esp_tls 对照，保留证书、主机名和 SNI 验证。

## 第一组自测

固件 `RickyOS-1.2.0-plus-5474dac7-vendor-tls-selftest.bin`，5051696 字节，SHA256 `1fd93a3ac2a3748480aa8c23966d9acec77387b2aafd364aec850580ff893c91`，ELF `9f135d2ab…`，受控安装与匹配首启通过。完整 Plus 主机回归通过。

自动连接已有网络，用同一 COS IPv4 交替测试原生 socket、Arduino TCP，再用原生 TLS 测试同一 IP（通过 common_name 保留 COS 的证书验证和 SNI）。现有下载器另测 COS/Fastly 列表、WeRead、站点 OTA 和两个最多 1 MiB 的读取；结果区分正常 EOF 与达到读取上限，不安装字体或向 SD 写入下载内容。

第一、二轮 COS 列表均收到 3331 字节，分别约 6.8 和 7.3 秒；第三轮仍失败。原生与 Arduino TCP 均曾超时；原生 TLS 第一轮握手完成但总预算内未收到 HTTP 响应，第三轮仍停在握手；wolfSSL 的其他站点也出现 DNS、连接及握手失败。三项配置开启本身尚未使设备网络稳定。

电脑的默认路由经过 utun9，不能把它直接当作相同网络出口的对照。改用 `curl --interface en0` 并固定到设备使用的 `36.248.13.173`、`.174`，电脑源地址为 `192.168.5.3`，两次均约 0.25 秒返回 200。电脑至路由器 ping 约 3 ms/无丢失，至设备当轮 ping 20 次未收到回复；后者仍需结合固件状态判断，不能单独定位原因。

日志：`logs/plus-vendor-tls-selftest-run.log`、`logs/plus-cos-lan173-control-result.json`、`logs/plus-cos-lan174-control-result.json`、`logs/plus-wifi-lan-ping-during-selftest.log`。

## 性能频率对照

发现自动自测先空闲 30 秒，主循环在 3 秒空闲后可降低 CPU 频率；`plusNetSelfTestActive()` 只重置活动时间，不能主动恢复已降低的频率。`HalPowerManager` 对 PSRAM 设备的低频为 80 MHz，正常频率为 320 MHz。这是需要实测排除的诊断干扰，第一组没有直接记录频率，尚不把低频当作已证实根因。

第二组保留同样 IDF、三项配置和请求路径，只在自测作用域使用 `HalPowerManager::Lock`，记录前后实际频率。将请求流程放进独立函数，确保性能锁和借用对象正常析构，再执行 `vTaskDelete`，包括无凭据的提前退出。

第二组候选 `RickyOS-1.2.0-plus-5474dac7-vendor-tls-perf-lock.bin`，5051776 字节，SHA256 `a53a671125cae940a7d82a6414fb6bb01f590faa858ee4299e0cca540e921b1e`，ELF `715f20bc…`。编译、安装及匹配启动通过。日志实测 80 → 320 MHz，但 COS 列表仍需 9.6–13 秒，DNS、TCP 与其他站点继续失败；CPU 频率不是充分解释。三个完整轮次未全部跑完，在第三轮采集中止以进行下一组隔离。


## 界面隔离及实际 Wi-Fi 初始化参数

第三组仅保留板级电源、RTC 与 SD 初始化，SD 挂载后直接启动自测，不初始化界面、字体或后台校时，主循环只 delay。CPU 全程 320 MHz，首次栈对照内部空闲约 256 KiB，PSRAM 约 16.8 MB。局域网对照直接连接电脑 `192.168.5.3:54321`，无 DNS、TLS 或外部路由；原生 TCP 和 Arduino TCP 均 6 秒超时，COS 原生 TCP 失败、Arduino TCP 偶尔成功、原生 TLS 握手超时、COS 列表解析失败。因此失败在无 UI 的固件中也可复现，不能直接归因于 UI 刷新或字体内存占用。第三组在第一轮列表结果后中止，未完成两轮全部站点。

第三组镜像 SHA256 `f68afcde53df8a8de4e135ec694a2de5a1fe2b88d6ecea1171b65ccdd3df2722`，ELF `d22a18303…`；日志 `logs/plus-vendor-tls-headless-run.log`。

发现 Arduino 4 RC1 的 `WiFiGeneric.cpp:280–286` 在默认 `useStaticBuffers=false` 下覆盖 `WIFI_INIT_CONFIG_DEFAULT` 中的 RX 缓冲数量为 4/32；启动日志实际也为 4/32，故此前「sdkconfig 24/64 已生效」只对配置值成立，不代表运行参数。BA window 仍为 16；IDF Kconfig 建议 static RX 数不少于 BA window。第四组只在 WiFi 初始化前加 `WiFi.useStaticBuffers(true)`，实际是否改善需检查启动参数和传输结果，不能仅据源代码判断。


第四组启动确认实际 RX=24/64；COS 原生 TCP 两次成功分别约 3.1/1.2 秒、原生 TLS 收到 HTTP 200（握手约 8.4 秒）。但 wolfSSL 完整 COS 列表约 20.8 秒、Fastly 列表约 18.7 秒，局域网首轮仍连接失败；不能据这一次原生 HTTP 成功宣称配置已修复网络。第四组只完成第一轮部分请求便进入下一组，未做完整两轮吞吐验证。

## 原生 ESP-IDF Wi-Fi 初始化对照

第五组跳过 `WiFi.mode/begin` 和 Arduino Network DNS，以 `esp_netif_create_default_wifi_sta`、`WIFI_INIT_CONFIG_DEFAULT`、`cfg.nvs_enable=false`、`esp_wifi_*` 初始化并加入原已保存的网络；只创建 STA，RAM 存储，固定 b/g/n，连接后 `WIFI_PS_NONE`。DNS 使用原生 getaddrinfo，HTTPS 使用原生 esp_tls，保留证书验证。保留 Arduino 运行时及板级/SD 初始化，因此是网络初始化封装隔离，并非完全不含 Arduino 的独立 IDF 工程。

两轮完整执行：第一轮局域网 TCP 两次失败；第二轮原生 TCP 55 ms 成功，但 20 秒只收到 234389 字节（包含 HTTP 头）。COS DNS 44 ms/1340 ms，TCP 约 5.8/1.4 秒，两轮 TLS 均在 12 秒预算结束时仍处于 `ESP_TLS_HANDSHAKE`。说明问题可在不走 wolfSSL、HTTP 下载器、Arduino Wi-Fi 初始化和 DNS 包装时复现。电脑应用防火墙查询为关闭；局域网阴性结果仍不单独排除 AP/VPN 等环境影响。

第五组 SHA256 `5b41417e06c680d26ea6be8e167a14277f944fd717a0e7d83b2365b819aba773`，ELF `042ed7c5…`，日志 `logs/plus-vendor-tls-native-idf-run.log`。后续对照仅改变协议为 b/g，避免把 n 的聚合与其他变量同时修改。

用户补充：厂商成功演示使用的是厂商自己的设备，未在本机完成相同网络、相同地址的对照，故不能据此排除本机射频/供电或 AP 互通问题，也不能据此断定是我们的框架。


## 协议及内核节拍对照

第六组仅把第五组原生网络测试的协议改为 b/g，保持 1000 Hz。两轮局域网连接均失败；COS 第一轮 TCP 与 TLS 连接阶段超时，第二轮 TCP 约 2.8 秒、TLS 约 0.86 秒收到 HTTP 200。故不能把 b/g 当成稳定修复；偶发快响应和首次连接失败同时存在。

第七组保留原生 b/g 测试，仅把 FreeRTOS 从 1000 Hz 改为厂商配置的 100 Hz。通过 Arduino CMake 自带的 `ARDUINO_SKIP_TICK_CHECK=1` 只放行此次无界面的诊断构建；**正常系统仍为 Arduino 要求的 1000 Hz**。两轮局域网仍均失败；COS 首轮 TCP 与 TLS 连接阶段超时，第二轮 TCP 约 2.8 秒、TLS 约 2 秒收到 HTTP 200，与第六组相似。未确认节拍是根因。

第七组 SHA256 `af249bcba97a0d7e3f89abca0b3ca896477df9fee4d8e86b000f4fe3a43b710c`，ELF `a136e1d5…`；实际配置检查确认 100 Hz 和三项 mbedTLS 选项开启。日志 `logs/plus-vendor-tls-native-bg-run.log`、`logs/plus-vendor-tls-native-100hz-run.log`。

## 当前结论与下一步

尚未修复联网稳定性。已确认 Arduino 默认覆盖了 RX 数量，但按 sdkconfig 实际生效后仍失败；CPU 性能、界面/字体初始化、后台校时、wolfSSL、Arduino Wi-Fi 初始化封装、n 协议和 RTOS 节拍均不能单独解释所有现象。原生 socket 的首连失败和局域网低吞吐，将排查重点移至 Wi-Fi/驱动配置、本机射频供电及 AP 互通；这些项目前还不能彼此区分。

有价值的下一步是对**同一台机器、同一 AP、同一 URL**运行厂商实际成功版本，并记录其完整 sdkconfig、ESP-IDF/Wi-Fi 库提交和请求日志。厂商另一台机器成功及当前仓库说明不能代替这一对照；本轮没有擦除 RF 校准、修改路由器、关闭 VPN 或改变 Flash 分区来冒充排除。

诊断选项保留为显式开关，正常构建全部关闭：`RICKYOS_PLUS_NETTEST=1`（自动请求）、`RICKYOS_PLUS_NETTEST_HEADLESS=1`（不启动 UI/校时）、`RICKYOS_PLUS_NETTEST_NATIVE=1`（要求 HEADLESS，原生 STA/DNS/TLS）、`RICKYOS_PLUS_NETTEST_BG=1`（原生测试只用 b/g）、`RICKYOS_PLUS_NETTEST_CONFIG_BUFFERS=1`（Arduino 遵守配置的缓冲数量）。HEADLESS/NATIVE 两轮，完整系统自测三轮。原生 TLS 的握手与 HTTP 响应各有独立 12 秒预算；普通 TCP 六秒、LAN 读取二十秒、下载读取达到 1 MiB 后主动终止，不向 SD 安装内容。测试服务只在电脑 LAN 地址临时监听，已关闭。


## 收尾及设备当前状态

七组诊断结束后，已只覆盖 ota_1 恢复完整备份中的 f2aa 原应用（5 MiB 槽完整 MD5 校验）；启动程序、分区表、OTA 选择和槽外区域校验一致。40 秒首启日志匹配 ELF `f2aa0ef55…`、IDF `9a97f6c5`，进入 `InxRecent`，显示初始化及刷新返回成功。当前不是无界面诊断版。原 ota_0 已是字体缓存；没有选择旧原厂槽。

正常 1000 Hz 构建通过，诊断宏全部缺席，镜像中无 headless 提示；候选 `RickyOS-1.2.0-plus-5474dac7-vendor-tls-normal-candidate.bin` 为 4886112 字节，SHA256 `befa96901662f07596747a5982f1a463b11f1405acebab91187c96f3a3f35fd4`，**未安装**，不作为网络修复交付。正常构建和普通/原生无界面诊断源语法检查通过，来源校验 2826 文件／79 适配通过；同步后完整 Plus 主机回归记录仍在 `logs/plus-5474dac7-port-tests.log`。

可复用的三项厂商配置位于 `firmware/rickyos-plus-1.2/sdkconfig.nettest.defaults`，作为独立 SDKCONFIG_DEFAULTS 叠加；正常工程 defaults 未强行改成未经稳定性验收的实验配置。诊断源及分支来源记录保留，本轮未提交或推送。设备当前状态见 `firmware/rickyos-plus-1.2/dist/installed-device.json`，旧 dist/manifest.json 已标为历史镜像，不能据其旧验收字段判断当前设备。
