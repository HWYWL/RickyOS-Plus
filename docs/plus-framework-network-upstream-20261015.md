# Plus 框架已知问题与版本核验（2026-10-15）

当前使用 Arduino-ESP32 4.0.0-RC1、ESP-IDF release/v6.1 提交 9a97f6c54ec638111ce55cd36581b3c192f15207（2026-09-22）。通过官方 GitHub API 与公开 issue / PR 核验，原始数据保存在 .local/framework-network-audit-20261015/，未更改 SDK 或升级依赖。

## 可确认的缺陷和版本状态

| 项目 | 官方记录 | 本机情况与边界 |
| --- | --- | --- |
| DNS 失败被误认为成功 | [PR #12967](https://github.com/espressif/arduino-esp32/pull/12967)，2026-10-05 合入，提交 505d78b41808d061293681968609b373620ed9d7 | RC1 尚未包含。错误 -54 是 EAI_FAIL 202 被 err_t 截成 8 位；错误判断让 connect(主机名) 继续连 0.0.0.0。现有 Plus SecureClient.cpp:88–93 已检查结果必须为 1，主下载已规避；不能声称该补丁能让失败的 DNS 查询返回成功。 |
| S31 软件复位遗漏 DMA 清理，可能污染下一次启动的堆 | [IDF #19176](https://github.com/espressif/esp-idf/issues/19176)，修复 [a496148](https://github.com/espressif/esp-idf/commit/a496148f547553c1bf08dd7063ef31b699e2e20a)，报告者 10 月 8 日确认有效 | 当前 system_internal.c:44 的退出清理没有官方新增的 DMA 中止/复位。报告的直接触发条件是运行 I2S RX 时软件复位；不能把本机没有崩溃的 DNS/TCP 超时直接归因于它。未将 SDK 补丁混入本轮同框架对照。 |
| S31 Null Function 帧长度异常报告 | [IDF #19116](https://github.com/espressif/esp-idf/issues/19116) | 仍开放；官方怀疑 Realtek 抓包器错误，要求换网卡复测。尚无已确认驱动修复，不能作为本机丢包根因。 |

## 有没有可直接升级的已修复版本

- [Arduino 官方发布列表](https://github.com/espressif/arduino-esp32/releases) 最新条目仍为 4.0.0-RC1（2026-09-23，预发布）。release/v4.0.x 已有 RC1 之后的提交，包括 DNS 判断修复；这是分支补丁，不是一个已验证解决本机问题的新正式发行版。
- [S31 支持说明](https://github.com/espressif/arduino-esp32/issues/12485)：维护者确认 S31 支持在 4.0.0-RC1，最终将进入 4.0.0；不能用版本号看似成熟的 3.3.12 直接替代本机 S31 目标。
- 核验时 ESP-IDF 官方 release/v6.1 分支头即本地 9a97f6c5。仅拉取该分支不能得到 10 月的 master DMA 修复；整体跳到 master 还会改变驱动与 ABI，需独立构建和设备回归。

## 已完成的本地连接器修正

SecureClient.cpp 原先只在首连失败的重试分支恢复 transport timeout，首连成功时遗留 4 秒，Arduino 后续收发也使用该值；现改为所有完成路径统一恢复。同时 DNS 和 TCP 已消耗的时间会从重试剩余预算扣除，不再无条件追加 5 秒。无新增堆分配，限定 Plus 分支，不改变 C3。

`tests/test_plus_tcp_timeout.py` 编译实际生产连接策略片段，验证首连成功、重试成功、预算耗尽、取消与 millis 回绕，并在地址/未定义行为检查下执行；原 TLS 版本回退回归也通过。这是代码层修正，不代表 COS 上机验收通过。


## Pico 为什么能连、Plus 不能据此视为同框架

本地产品仓库 platformio.ini:983–985 显示 Read Pico 是 ESP32-S3（N16R8），base:10 锁定 pioarduino 55.03.311；本机 framework-arduinoespressif32/package.json 版本3.3.11，S3 SDK esp_idf_version.h 为5.5.5。Plus 是ESP32-S31、Arduino4.0.0-RC1和IDF9a97f6c5。以上为构建配置/安装包来源，未读到用户那台Pico当前固件ELF，不能把它当作实机版本验真。

Pico配置显式启用SPIRAM_TRY_ALLOCATE_WIFI_LWIP，通用malloc内部阈值256字节、内部预留32768字节；Plus可用对照构建对应未启用、16384字节、65536字节。分配策略不同，但不证明内存就是建连超时根因。应用层依然使用SecureNet/wolfSSL，Plus另有DNS检查、路由设置、4+5秒TCP策略、强制bgn等专用分支。

用户报告Pico正常与当前Plus失败组合，使共同URL/JSON格式错误的解释不合理，但需要Pico同网络、同COS域名/最终源日志才能排除Pico也走备用镜像。Plus当前日志COS失败在TCP、未进入TLS，优先检查S31平台和专用适配路径，不能仅凭Pico成功判定wolfSSL库故障，也不能把S3驱动二进制直接移植给S31。
