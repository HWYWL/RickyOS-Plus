# Plus 1.2.6 修复分支交接（2026-10-22）

分支 `codex/plus-1.2.6`，基于 `plus-v1.2.5`（bca656dc）。Pico 对应分支 `codex/pico-1.2.6`，基于 `pico-v1.2.5`（dafc0601）。
两条分支都未发版。

## Plus 提交

| 提交 | 内容 | 验证 |
|---|---|---|
| dc998498 | MOBI/AZW3 固定版式漫画（Kmoe，EXTH 122）不再被拒；RickyOS 主题对话框显示 `showMessage` 的正文 | 电脑端导入测试（含海贼王样本）；Plus 实机能打开 |
| b5d170e4 | MOBI 打开提速：缓存标记 v3 抽样校验，v2 首次全量校验后沿用；查表 CRC；32 KiB 缓冲 | 测试含 v2 沿用/失配重建；Plus 实机重新打开约 2.9 秒 |
| df2c8892 | 切 tab 等区域 UI 刷新改用 Pico 式切换表（`MODE_GL16_INV` 槽，37 拍、左对齐、对角线保持、变黑不先擦白） | 完整测试；用户确认不闪、有淡残影 |
| 3ce1e785 | 看图按文件签名识别 PNG/JPEG/BMP（JPEG 命名为 .png 的图）；可选耗电日志 `RICKYOS_PLUS_POWER_LOG=1` | 完整测试；看图修复在 Pico 实机验证，Plus 未实机 |
| be3fe1a9 | 淡墨：普通阅读翻页改为整屏厂商 GL16（`epd_hl_update_screen_full(MODE_GL16)`），留在原位的黑笔画每页重新压黑 | 完整测试；Plus 实机 A/B 对比，选每页整屏 |
| a79a0432 | 取代上一行的表：翻页用自有表（`MODE_GC16_FAST` 槽）= UI 切换表（37 拍、左对齐、未变像素保持、变黑不先擦）+ 留在原位纯黑 5 拍擦 + 5 拍压，仍整屏推送 | 完整测试；Plus 实机对比，用户认为速度和墨色都可以 |

## Pico 提交

| 提交 | 内容 |
|---|---|
| fd546f17 | 同 dc998498（`lib/KindleImport`、对话框正文） |
| ba0e8186 | 同 b5d170e4 |
| 03b8fb26 | 同 3ce1e785 的看图修复（Pico 实机验证） |

Pico 的翻页仍是文字转页表（不重压黑→黑），未改；目前没有 Pico 淡墨反馈。

## 测量记录

- 耗电（Plus，拔线，日志 `rickyos-power.csv`）：阅读静止与时钟待机均约 57–58 mA（BQ27220 电流与累计计数一致），机器从不睡眠；2000 mAh 约 34 小时。关机 28.7 分钟累计仅 1 mAh，需过夜复测。
- VCOM：TPS65185 存储值为 0x7D（1250 mV，芯片默认，无出厂校准）；厂商源码同样写死 1200 mV；1450 mV 对比差别不大，未改。

## 合并注意

- Plus 移植改动都已用 `sync_plus_design.py --record` 记录；`run_plus_port_tests.sh` 需要 `.local/build-rickyos-plus-1.2/compile_commands.json`。
- 若同步把 Plus 基准升到 `pico-v1.2.5` 或更新：`KindleImport.cpp` / `KindleDocument.cpp` 在 Plus 是 `components/rickyos_application/port/` 下的移植文件，需单独带过去。
- Plus 新的联网代码必须持有 `HalWifiAwakeLease`，否则会卡在 20 dBm 下的 SYN 重传问题。
