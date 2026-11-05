# RickyOS 1.2 · Metalio E-Ink4-Plus

设备专用分支：`chinoryunqin/RickyOS-dev:codex/metalio-e-ink4-plus`。

适用设备：**Metalio E-Ink4-Plus V1.5 / E0470A14-AF**，ESP32-S31、4.7 寸并行墨水屏、FT6336U、冷暖双路前光。普通 E-Ink4 / ESP32-S3 / 3.97 寸 SPI 设备不使用此工程。

本目录是本分支的 Plus 开发入口。仓库根部的通用 RickyOS 工程保留为来源基线，不能作为这台设备的构建或刷机入口。

## 最新交接 · 2026-10-20

Plus 1.2.4 已通过本次用户真机确认并正式上线；沿用已验收的 BIN `d633d920…`，
包含传书网页响应修正、热点地址跳转、独立锁屏右滑解锁开关，以及来源文案／首用指南清理。
固定来源仍为 `a42d9399`／SDK `96de1be6`，2,844 文件、136 项适配。
发布与校验记录见 [Plus 1.2.4](docs/plus-1.2.4-release-20261020.md)。
不包含 `codex/plus-display-service` 的持续系统 Wi-Fi／显示服务实验；其他设备正式版本不变。

独立功能分支 `codex/plus-kindle-formats`：MOBI/AZW3 导入与原阅读引擎已接入，
两本用户样书通过主机差分检查并在独立模拟器中打开。仅以私有开发分支交接，未刷机、未发布。
实现、容量限制和复现方式见 [Kindle 格式接入记录](docs/plus-kindle-import-20261020.md)。
当前模拟器地址为 http://127.0.0.1:8785/，不是旧 8769 页面。

当前阅读优化位于私有 `RickyOS-dev:codex/plus-online-fix-1.2.2`，不是公开发布或其他机型更新。
先读 [阅读性能进度与审查入口](docs/plus-reader-performance-handoff-20261020.md)：包含改动范围、开关、
101 次真机翻页结果、当前体验版摘要、构建/测试方式及未完成事项。
固定设计来源 `a42d9399`，SDK `96de1be6`，2,844 个来源文件、102 项 Plus 适配。
已刷体验版 `1.2.2-dev`（BIN `ebbd2f2d…`），仅写 app0 并显式重启到首页。
相位预备重叠是默认关闭的实验开关，复现实机体验须显式开启；尚未合入正式渠道。

## 2026-10-16 迁移基线与设备状态（历史）

- RickyOS固定来源：`e5e1dcccdce1e897470939e841519333d23943ab`（1.2.0 发布版，含 Wi-Fi 密码/账号 Latin 输入和 12pt 字体频率分组）；FreeInk SDK：`96de1be6ce08eb732909e6e8149af8f892b9a2c5`。还原校验2835文件、79项适配；不使用其他设备工作区的未提交源码。12pt 字体沿用设计分支的频率分组，`compress_plus_builtin_fonts.py` 对已分组字体原样拷贝。
- 当前设备运行 1.2.0（同步 e5e1dccc，含微信读书功率租约），4,816,672B，应用SHA256 `3153045ff3ac210d0612a9e460e42d6de3b207812e157b1083488e54a4351694`、ELF `306e98d4e…`、IDF `9a97f6c5`。仅写ota_1，读回散列一致，启动至InxRecent。Wi-Fi 密码键盘的实机确认待做。
- 联网使用者持有共享租约时，Plus发射功率上限取min(原值,5dBm)，最后使用者退出恢复原值。两次独立启动对照显示5dBm首次TCP约0.11秒；20/13dBm首次连接超时或明显偏慢。供电/天线/PHY的唯一根因未锁定，远距离及其他AP尚未验收。
- 真机自动验证：正常DNS、SD挂载、各阶段冷重连，COS屏保列表827/565ms、字体列表676ms，完整字体1,763,452B/6702ms且SD读回CRC一致，原生NTP1096ms成功；下载累计805次发送完成、失败0。字体为临时完整文件验证，不冒称整字体家族安装或用户界面验收。见[实际功能验收](artifacts/plus-tx-power-acceptance-20261016/report.md)和[普通版安装记录](artifacts/plus-tx-power-compat-20261016/report.md)。
- 既有阅读、七入口应用页、基础录音回放、触控震动及Tab叠影修复的用户验收保留为历史结果。蓝牙配对和自动旋转尚未接入；本轮未重新验收USB传输、音量曲线及录音待机保存。

## 还原和验证源码

从本目录运行：

```sh
python3 scripts/restore_rickyos_plus_sources.py
python3 scripts/check_rickyos_plus_sources.py
python3 tests/test_plus_audio.py
python3 tests/test_plus_frontlight_panel.py
python3 tests/test_plus_standby_frontlight.py
```

还原脚本读取固定 Git 提交、逐文件核对 SHA256，再应用 `firmware/rickyos-plus-1.2/upstream-plus.patch`，重新核对全部 2,844 个来源文件和 102 项适配。它不会使用其他设备的工作区修改；已存在的源码只校验，不覆盖。也可通过 `--source-repo` 和 `--sdk-repo` 使用本地 Git 对象进行离线还原。

完整应用源码还原到 `firmware/rickyos-plus-1.2/upstream/`；Plus 自有板级组件位于 `firmware/rickyos-plus-1.2/components/`。生成文件、源码还原目录、二进制、设备私有备份和日志均不提交。

## 构建

当前普通版验证使用独立ESP-IDF release/v6.1 `9a97f6c5`（通过 `RICKYOS_IDF_PATH` / `RICKYOS_BUILD_DIR` 选择）；历史固定依赖为ESP-IDF v6.1 `fff9895c82d744c7237be8847347bdd1b07c6643`，Arduino-ESP32 4.0.0-RC1 `76f683d935b390f2805301ec10a5562bbbb37811`，mDNS 1.11.2 `4fb97b209fbc695cc3019e0f1ef7937f766e6503`。其他库的版本与逐文件散列见 `dependency-lock.json`。

还原源码后，需要按上述固定版本准备本目录 `.local/arduino-esp32-4.0.0-RC1`、`.local/dependencies/` 和 `tools/esp-idf-v6.1`。当前分支未提供所有第三方依赖的一键下载器；完整构建已在原迁移工作区验证，不宣称全新环境已自动安装验证。

```sh
python3 -m pip install -r scripts/requirements-assets.txt
bash scripts/prepare_rickyos_plus_resources.sh
bash scripts/build_rickyos_plus.sh
bash tests/run_plus_port_tests.sh
```

2026-10-20 热点／加入网络传书排查：已存在的 `sdkconfig` 不会自动采用新默认值；
网络内存试验须用独立配置并通过生成头检查，见[网页响应排查与配置迁移](docs/plus-web-response-20261020.md)。
不手改生成配置，也不要把第一轮“顺序访问恢复”当作并发验收通过。

构建输出 `.local/build-rickyos-plus-1.2/`。不要直接执行 IDF 输出提示里的整包 `idf.py flash`：它会默认写原厂 ota_0。本机受控安装依赖独立保存的原厂全闪存备份与设备身份校验，这些私有恢复材料不上传；本提交也不提供脱离备份的通用刷机脚本。

## 接入状态

- [全部外设与接入计划](docs/rickyos-1.2-plus-peripherals.md)
- [前光与待机行为](docs/rickyos-1.2-plus-frontlight.md)
- [显示、输入、电量和 RTC](docs/rickyos-1.2-plus-display-acceptance.md)
- [尚未解决的网络问题](docs/rickyos-1.2-plus-clock-diagnosis.md)

共享功能更新从设计分支显式引入，并在 Plus 上重新构建和验收；其他设备的引脚、波形、音频、电源配置不合入本目录。

厂商代码保留原版权声明，MIT 许可见 `firmware/rickyos-plus-1.2/components/metalio_board/LICENSE.vendor`。RickyOS 与 SDK 的许可随固定来源还原保留。

本地新增 [控制中心重设计](docs/rickyos-1.2-plus-control-center-design.md)：依据设计规范，矩形快捷键、前光／音量分组、横竖屏布局和收起按钮。构建及 32 组真实 FUI 几何测试通过，设备视觉验收待完成。

Plus 按键设置已实现并通过分类／逻辑映射及 JSON 持久化主机测试；本轮完整固件构建通过，实机按键验收待确认。见 [按键自定义](docs/rickyos-1.2-plus-buttons.md)。

按键入口已统一为「设置 → 系统 → 按键」，原有三项按键偏好保留在同一页；不再新增独立入口。

触控震动短脉冲和设置开关已完成，主机测试与完整构建通过；用户已确认正常。见 [触控震动](docs/rickyos-1.2-plus-haptic.md)。

2026-10-13：Plus 专用「应用 → 录音机」已实现麦克风采集、PCM WAV 保存和回放，主机与完整固件构建通过，用户已确认基础录音回放正常；待机保存、分页和故障场景仍待单独验收。见 [实现记录](docs/rickyos-1.2-plus-recorder.md)。

## 本机录音机界面预览

先按上文还原源码并生成资源，再运行 `python3 scripts/prepare_plus_recorder_preview.py`。在生成的 `.local/plus-recorder-simulator/` 目录运行 `pio run -e simulator_rickyos`，随后运行 `python3 tools/rickyos-browser/server.py --port 8783`，打开 http://127.0.0.1:8783/ 的「应用 → 录音机」。需要 macOS、SDL2 和 PlatformIO。

使用实际 RecorderActivity 的 684×1216 帧缓冲；采集和回放由内存状态模拟，不调用电脑麦克风或扬声器，重启清空模拟录音。不会操作 USB 或修改实机固件。

录音准备状态现由首批 PCM 判定，采集就绪时立即切换状态并短震一次；短震沿用触控震动开关，用户已确认「可以了」。

2026-10-13 已整合并上机 design 73901ae8 的阅读模式、阅读线、字重/TTF、统计月报、下载线路及绘制优化。无损字体压缩保持原 5MiB 槽；详见 [整合与验证记录](docs/rickyos-1.2-plus-design-73901ae8.md)。新功能实机交互待验收；小狗伴读左边框缺失已通过 DMA 完成判断修正，用户确认恢复。

2026-10-14：提交联网入口复用、15 秒自动连接等待、继承按键防护、字体容量进度和两秒更新、小目录共享请求预算、TLS 回退条件及静态待机重画抑制。正式候选已构建、安装并正常进入主页；新增主机回归通过，列表联网和静态待机的实机验收仍未完成。详见 [修复与网络对照记录](docs/rickyos-plus-network-ui-standby.md)。

2026-10-14：整合 design d091adbe 的字号／字重菜单、开机准备和 USB 传书改进，补齐 Plus 原生 16 级相邻页缓存与小章节预取，避免重复黑白正文绘制；可选缓存约 1.19 MiB，内存不足时回退，未单独测量实机缓存命中速度。字体下载显示实际请求来源，包含 Fastly 和重定向站点。应用页按可用高度排列，竖屏七入口同页，用户确认正常。完整构建、主机回归和匹配 ELF 首启通过，见 [整合与验收记录](docs/rickyos-1.2-plus-design-d091adbe.md)。网络稳定性仍待解决。

## Plus 联网诊断开关

普通构建默认启用 `RICKYOS_PLUS_TX_POWER_COMPAT=1`，其余诊断均关闭。`RICKYOS_PLUS_RADIO_COMPARE=1` 要求同时启用 `RICKYOS_PLUS_TCP_TRACE=1`；受控对照默认禁用功率兼容，可显式设置开启。`RICKYOS_PLUS_RADIO_AUTOSTART=1` 会在正常UI启动后自动连接可见的已保存网络，仅供已授权的无人值守设备测试；不会输出凭据。`RICKYOS_PLUS_NETWORK_ACCEPTANCE=1` 运行实际下载、SD读回及原生校时验收，临时文件独占创建并清理。`RADIO_TIMING/RADIO_RATE/RADIO_POWER` 用于隔离时序、聚合/速率、功率，不能当日常固件开关。每次构建先reconfigure，避免复用上次诊断宏。

新增聚焦主机检查：`tests/test_plus_tcp_timeout.py`、`test_plus_clock_radio.py`、`test_plus_tcp_trace.py`、`test_plus_radio_compare.py`、`test_plus_radio_auto.py`、`test_plus_wifi_tx_config.py`、`test_plus_acceptance_integrity.py`。普通构建触控/时钟链接检查及完整构建通过。详细诊断过程见[底层对照](docs/plus-network-low-level-diff-20261015.md)。设备完整Flash、原始日志、凭据与固件二进制保持本地，不提交。
