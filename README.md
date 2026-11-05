# RickyOS-Plus

RickyOS 在 **Metalio E-Ink4-Plus（V1.5 / E0470A14-AF，ESP32-S31 / `esp32s31`）** 上的独立工程。

本仓库从 `RickyOS` 仓库 `codex/metalio-eink4-1.2.11` 分支的 `ports/metalio-e-ink4-plus` 全量移植而来，
构建系统是**原生 ESP-IDF 6.1 + CMake**，不再使用 PlatformIO。固件功能与移植前一致，
后续才会逐步把应用层换成 Rust。

## 设备

| 项 | 值 |
| --- | --- |
| MCU | ESP32-S31（RISC-V），16 MB Flash，OCT PSRAM 200 MHz |
| 屏 | 4.7 寸 1216×684 并行墨水屏，EPDiy 输出路径 + TPS65185，16 灰阶 |
| 触控 | FT6336U + 屏下三键 / 侧键 / AI 键 |
| 音频 | BH1098G（UART AT 控制 + I2S），麦克风录音、扬声器回放 |
| 其他 | 冷暖双路前光、TCA9555 震动与电源、SDMMC 4-bit、USB HS MSC、PCF8563 RTC、BQ27220 电量计 |
| 分区 | 双 6.25 MiB OTA 槽 + 3.375 MB SPIFFS（`firmware/rickyos-plus-1.2/partitions.csv`） |

## 目录

```
firmware/rickyos-plus-1.2/     ESP-IDF 工程（CMakeLists.txt、main/、components/）
  upstream/                    还原后的 RickyOS 1.2 应用与 FreeInk SDK 快照（已校验，随仓库提交）
  components/
    metalio_board/             厂商板级 C：并行屏时序、LUT、波形、PMIC、触控、SD
    rickyos_display/           FreeInkDisplay 与面板驱动抽象（Plus 走 MetalioPlusDriver）
    rickyos_application/       上游应用适配、Kindle 导入、音频、网络与诊断
    rickyos_platform/          HAL 声明、BoardConfig、电池估算
    rickyos_storage/           SdFat 封装与存储租约
    plus_usb/                  S31 TinyUSB MSC（原始扇区，SdFat 独占）
scripts/                       引导、构建、烧录、校验与资源生成
tests/                         主机侧检查
docs/                          移植与验收文档（含原端口说明 port-readme.md）
rust/                          Rust 迁移预留位置（当前为空）
```

## 快速开始（Windows / PowerShell）

```powershell
# 1. 一次性准备：ESP-IDF 6.1(9a97f6c5) + esp32s31 工具链 + 全部库依赖
./scripts/bootstrap_plus.ps1

# 2. 构建（会重跑所有守卫检查）
./scripts/build_plus.ps1

# 3. 烧录（默认只写应用镜像）
./scripts/flash_plus.ps1 -Port COM5
```

首次引导会下载约 2–3 GB（IDF 源码、工具链、依赖），之后可随时用
`-SkipIdf` / `-SkipInstalledIdf` / `-SkipDeps` / `-SkipUsb` 跳过已完成阶段。

构建产物：`.local/build-rickyos-plus/RickyOS-Plus-1.2.bin`。

## 依赖是怎么来的（没有 PlatformIO）

`scripts/fetch_plus_deps.py` 按 `dependency-lock.json` 取包并逐文件校验 SHA-256：

- 之前作为 PlatformIO 注册表包的库（ArduinoJson、SdFat、PNGdec、QRCode、WebSockets、
  PubSubClient、Arduino-wolfSSL）直接从包 CDN 下载，**不需要 `pio` 可执行文件**；
- 之前是 Git 检出的依赖（JPEGDEC、esp-mdns、Arduino-ESP32 4.0.0-RC1、esp-usb）按提交号浅克隆，
  并强制 LF，避免 Windows 的 `core.autocrlf` 破坏哈希；
- SdFat / JPEGDEC / wolfSSL 需要上游审过的补丁，脚本会自动套用并再次校验。

`.piopm` 是 PlatformIO 的安装元数据，会被改写，因此只报告、不判定失败。

TinyUSB 走 ESP 组件注册表，由 `scripts/prepare_plus_usb.py` 在 IDF 的 Python 环境里完成。

## 构建守卫

构建脚本会在编译前后各跑一遍，任何一项不通过都不会产出可烧录镜像：

| 检查 | 作用 |
| --- | --- |
| `verify_plus_release_idf.py` | IDF 必须是干净的 `9a97f6c5`；镜像描述里的 IDF 路径与目标必须是 `esp32s31` |
| `check_rickyos_plus_sources.py` | 2842 个固定来源文件 + 285 项移植改动逐文件哈希 |
| `check_plus_wifi_security.py` | Arduino STA 连接的 TLS/安全策略 |
| `check_plus_web_memory.py` | Web/网络内存预算 |
| ELF 符号探测 | 链接结果必须包含 Plus 触控后端 |
| `tests/check_plus_clock_objects.py` | RTC / 校时的所有权归属 |
| 镜像体积 | 应用不得超过 6.25 MiB OTA 槽 |

`upstream/` 已随仓库提交，因此新克隆无需再还原；如需重放，运行
`python scripts/restore_rickyos_plus_sources.py --source-repo <RickyOS> --sdk-repo <FreeInk SDK>`，
它会先校验现有快照。

## 烧录注意

- 默认 `-Mode app` 只写应用镜像，不动 bootloader / 分区表 / 另一个 OTA 槽。
- `-Mode full` 会重写 bootloader 与分区表，属于首次安装或救援路径，必须显式加
  `-IHaveFullBackup`，并已对整片 16 MiB 做过校验后的备份。
- 烧录前会核对芯片型号，非 ESP32-S31 直接拒绝。

## 后续：应用层迁移到 Rust

`rust/` 为迁移预留。边界是现成的 ESP-IDF 组件图：

- 保留 C/C++：`metalio_board`（并行屏时序、波形、LUT）、`rickyos_display`、音频、USB、SD、网络与 wolfSSL。
- 逐步 Rust 化：EPUB/TXT 解析与排版、设置与统计、i18n、Activity 框架，最后才是渲染路径。
- 接入方式：Rust 编译为 `staticlib`，作为一个 IDF 组件链接进来，只通过 C ABI 交互
  （禁止 `String`/`std::vector` 跨边界，`panic = "abort"`）。
- 硬约束：6.25 MiB 代码槽、32 KB 内部 RAM/DMA 预留、PSRAM 优先分配。

## 许可

RickyOS 与 FreeInk SDK 的 MIT 许可及第三方组件许可随快照保留；
厂商板级代码的许可见 `firmware/rickyos-plus-1.2/components/metalio_board/LICENSE.vendor`。
