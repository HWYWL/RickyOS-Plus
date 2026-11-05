# Plus 待机省电与电量显示：合并说明（2026-10-23）

## 分支

- 分支 `codex/plus-1.2.6-battery`。
- 它基于 `codex/plus-1.2.6` 的 `1d470cce`，电量改动已经合并在上面。如果 1.2.6 之后没有新提交，可以直接快进合并。
- 和 `1d470cce` 相比，它只多了电量和待机相关的改动：
  - `PlusBoard`、`PlusGaugeConfig`、显示驱动的唤醒恢复、`metalio_sd`、CMake 开关；
  - 补丁里的 BatteryMonitor、HalPowerManager、HalStorage、StandbyActivity、main.cpp；
  - 相关测试。
- 存储页（`RickyStorageActivity`）保持 1.2.6 的原样。
- 另一个会话在 `wt-plus-126` 里有没提交的存储页改动（SD 统计改为只显示），这些改动不在本分支里。
- 版本号仍是 1.2.5，发版时再改。

## 内容

1. **静态待机**
   - 进入待机：只做一次收尾（暂停 SD、触摸、总线、IMU、按键掩码，断开 P5 摄像头/屏幕供电），然后浅睡，直到按电源键或到自动关机时间。
   - 唤醒后：先把 MSPI 恢复到高速时序，否则 PSRAM 降到 22 MB/s，屏幕会变白。
   - P14 保持供电：屏幕需要它，重新上电会因冲击电流导致复位。
   - 实测待机约 6 mA（原厂 3 mA，原来 57 mA）。
2. **电量百分比**
   - 开机时，如果电量计 BQ27220 里的设计容量不是 1871，就写入电池实际参数：1871 mAh，EDV 3300/3400/3500 mV。
   - 之后显示电量计自己的 StateOfCharge。
   - 写入失败时，退回原来的估算算法。
   - 实测从 100% 用到 77%：每格 18.8 mAh（理论值 18.7），没有跳格或回升。
3. **编译开关**
   - 默认开启：`STANDBY_SLEEP`、`STATIC_STANDBY`、`STANDBY_KEEP_RAILS`、`STANDBY_CUT_P5`、`GAUGE_CONFIG`。设成 `0` 可以关闭。
   - 默认关闭：功耗日志 `RICKYOS_PLUS_POWER_LOG=1`、待机诊断 `RICKYOS_PLUS_STANDBY_DIAG=1`。
   - 一次性的诊断已经删除：电量计备份读取、唤醒测速探针、唤醒报告。

## 编译注意

- **必须用打过补丁的 IDF：** `RICKYOS_IDF_PATH=<wt-plus>/ports/metalio-e-ink4-plus/.local/esp-idf-v6.1-r`（release/v6.1 9a97f6c5，和设备上的 bootloader 对应）。
- 用原版 v6.1 编出来的包，一开机就会在 `esp_cpu_configure_region_protection` 卡死并反复重启。Plus 没有开启 OTA 回滚，这种包一旦发出去，用户只能用数据线重新刷机。
- 本分支的 `build_rickyos_plus.sh` 会检查 `project_description.json` 里的 git_revision，不是 9a97f6c5 就直接报错。
- 新工作区要先恢复源码，并准备资源：
  - `restore_rickyos_plus_sources.py`
  - `RICKYOS_RESOURCE_PYTHON=.local/assets-venv/bin/python bash scripts/prepare_rickyos_plus_resources.sh`
- 编译结果：
  - 本分支 `db52b003` 编出的包是 4,950,384 B，sha256 前缀 f984f46d。
  - `ab4be0cd` 只改了测试脚本，不影响固件。
  - `run_plus_port_tests.sh` 全部通过。

## 已验证与未验证

- **实机已验证**（维护者那台 Plus）：
  - 30 分钟待机约 6 mA；
  - 短按电源键能唤醒，画面正常；
  - 唤醒后声音正常；
  - 放电 100%→77% 的电量走势正常；
  - 合并版开机正常，电量计配置保留，显示 96%。
- **未验证：**
  - 整夜待机的掉电量（预计每 8 小时 2～3%）；
  - 低电量那一段和关机点（EDV 3.3 V）；
  - 其他电池或电量计固件版本不同的机器上的首次写入；
  - 从 1.2.5 在线升级到含本改动的包。

## 发版前建议

1. 在一台装着 1.2.5 正式版的设备上实际走一遍在线升级，确认能正常开机。
2. 进入待机，等 1 分钟后短按电源键唤醒，重复几次，确认唤醒都正常。
3. 唤醒后播放一次声音，再翻几页书。
