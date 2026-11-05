# RickyOS → Metalio E-Ink4-Plus 初步迁移评估

日期：2026-10-10。状态：文档和源码静态评估；未构建、未刷机、未做硬件验证。

## 材料与基线

- 厂商说明：https://github.com/CloudZao/Metalio-E-INK4-Plus/blob/main/README_zn.md
- 本地厂商包：/Users/rick/Downloads/Metalio-E-INK4-Plus-source-2.0.76-debug-20261007/
- 厂商工程：包内 firmware；PACKAGING.txt 标记 2.0.76-debug；dependencies.lock 锁定 ESP-IDF 6.1.0、target esp32s31。
- 厂商 docs/development-status.md 明确此包用于 Metalio V1.5 / E0470A14-AF 4.7 寸 34Pin 屏。网页与 config.h 采用 ED047TC2_1216 命名，不能据此推定其他批次面板的波形和 VCOM 一致。
- RickyOS 当前开发目录：/Users/rick/Documents/Codex/2026-10-02/https-github-com-0x1abin-crossmux-https/repository-publish/RickyOS-1.1.5-hotfix/；正式基线 1.1.4，1.1.5 补丁仍在独立会话收尾。

## 判断

这是跨主控和显示后端迁移。RickyOS 使用 Arduino/PlatformIO 与 FreeInk SDK；Plus 是 ESP32-S31 RISC-V、原生 ESP-IDF 6.1、EPDiy 并行屏。现有 MetalioEink4Board 名称不能作为 Plus 兼容证据。旧 ESP32-S3 固件不能直接写入新设备。

建议先建立独立 ESP-IDF 6.1 bring-up 工程，复用厂商板级驱动，验证显示、电源、输入和 SD，再将 RickyOS 的硬件接口接入。是否采用 Arduino-as-component 需先验证 S31 支持与依赖兼容，不能把它当作已可用方案。保留 RickyOS 阅读与 UI 的目标需要评估 Arduino/WiFi/文件系统依赖；不建议起步时重写为厂商 LVGL 应用。

## 适配清单

| 部分 | 厂商事实 | RickyOS 适配工作 |
| --- | --- | --- |
| 构建 | esp32s31 / ESP-IDF 6.1.0；厂商脚本为 Windows PowerShell | 配置 macOS 工具链，恢复锁定组件，先复现厂商工程构建 |
| 显示 | EPDiy + TPS65185；原生 1216×684，竖屏 684×1216 | 新显示后端：旋转、位序、1-bit/灰阶、刷新模式、局刷矩形与串行调度 |
| 布局 | config.h 四边各 8 px 内缩 | 可绘区域 668×1200；排查固定尺寸、触摸映射、状态栏和阅读分页 |
| 输入 | FT6336U 0x38；屏外虚拟键；TCA9555 侧键；BOOT GPIO61 低有效 | 触控与按键事件统一、长短按、去抖、待机唤醒 |
| 总线 | I2C SDA0/SCL1，共用 TCA/触控/电量计/RTC/充电/加速度计/TPS | 共用一个总线生命周期及访问同步，避免各模块重复初始化 |
| 存储 | SDMMC 4-bit GPIO20–25 | 文件访问适配、挂载/卸载与睡眠恢复；复核现有 SdFat 等依赖 |
| 电源 | BQ27220、CX25601N；TCA 控制关机、屏电源等 | 电量/充电状态、欠压、关机脉冲、浅睡/RTC/USB 联动 |
| 前光 | GPIO6/7 冷暖 PWM | 亮度与色温、设置持久化、待机策略 |
| USB | S31 HS UTMI TinyUSB MSC；GPIO36 选路 | 区分调试/刷机/MSC，互斥 SD 文件系统访问，验证睡眠后连接 |
| 音频 | 外置 BH1098G，经 UART AT + I2S | 第二阶段独立接入；并非直接复用 ESP 内置蓝牙音频 |
| 分区 | 16MB；ota_0 0x80000 / ota_1 0x580000；每槽 5MiB | 重新设计构建与 OTA 容量；禁止套用 Read Pico 的应用地址 |

当前 Read Pico 的 MD5 候选镜像约 6,029,312 字节，大于厂商每槽 5,242,880 字节。跨芯片构建后尺寸会变化，因此这是容量规划问题，不能拿旧镜像尺寸直接推定 S31 结果。若调整分区，需同步安装器、OTA、资源与备份恢复方案。

## 厂商已记录的待查问题

以 firmware/docs/development-status.md 和 tps65185-boot-overheat.md 为准：

- BOOT 下载模式下 TPS65185 严重发热是已有用户反馈，根因未实测确认。ROM 模式不运行应用初始化；迁移驱动不能代替硬件默认电平验证。
- 板级代码记录了先设输出方向后写低、deinit 改为输入、故障时保持使能三处电源审查点。需分别核对并验证，不能未经测试照搬诊断行为。
- VCOM=1200mV 是代码配置，未确认面板背标或实测值。
- EPD err=0x400 并发回归未完成；起步时统一刷新入口，测量完整交互延迟。
- 偶发语音错误识别、首次 TLS 等待与部分 HTTP 正文超时尚未定位。

## 分阶段验收

1. 基线复现：工具链版本、组件锁、厂商源码校验、编译产物与分区检查。确认实际 PCB 修订和面板型号/VCOM；源码支持不等于手上设备相同。
2. 最小硬件：启动日志、I2C、屏电源、黑白/灰阶/局刷、白边、输入、前光、SD。记录刷新及峰值内存。
3. RickyOS 核心：显示/输入/存储/电源接口，首页、书库、TXT/EPUB/MD、字号重排、进度恢复、目录跳转。
4. 系统功能：电池供电待机与唤醒、关机、RTC、Wi-Fi 传书、USB MSC、OTA/恢复；再加入音频和扩展功能。

厂商源码根 LICENSE 为 MIT；组件和移植文件仍须保留各自许可及版权声明。

本评估没有确认 macOS ESP-IDF 安装情况，也没有验证依赖下载、编译或设备连接。实际硬件修订是后续刷机和驱动参数选择的必需信息。

## 源码细化（2026-10-10）

实际 `metalio_epd.c` 使用 `E0470A14_AF`、`EPD_ROT_LANDSCAPE` 和 `MY_WAVEFORM`，竖屏通过上层 logical→physical 映射实现。`metalio_epd.h` 的 inverted-portrait 注释不能作为实际实现依据；移植必须避免驱动与上层重复旋转。

LVGL I1 补丁说明要求恢复 managed_components 后运行 `lv_draw_sw_blend_to_i1/apply_patch.py`。当前 `main/CMakeLists.txt` 确实添加了 display include，但未见自动执行该补丁脚本；依赖恢复后需检查补丁状态，不应因 README 声称自动处理而省略。
