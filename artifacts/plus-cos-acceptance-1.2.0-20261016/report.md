# Plus 1.2.0 联网复验：COS 直连（2026-10-16）

在同步到设计 1.2.0（e5e1dccc）后的代码上，复验 28344e84 的发射功率兼容修正。验收固件 = 正式源码 + `RICKYOS_PLUS_RADIO_COMPARE/TCP_TRACE/RADIO_AUTOSTART/NETWORK_ACCEPTANCE=1`，`RICKYOS_PLUS_TX_POWER_COMPAT=1`，4,823,856 B，仅写 ota_1，无人值守运行。每一步都冷重连已保存网络（RSSI −55/−56）。

| 阶段 | 入网 | 请求 | 大小 | CRC32 一致 |
|---|---:|---:|---:|---|
| 屏保 COS 列表 | 6.9 s | 1,134 ms | 3,965 B | 是 |
| 字体 COS 列表 | 9.8 s | 858 ms | 9,024 B | 是 |
| RickySans_12.cpfont（全程 COS） | 7.3 s | 6,846 ms（259 KB/s） | 1,763,452 B | 是 |
| 原生 NTP 校时 | 6.2 s | 541 ms | — | 成功 |
| 再次屏保列表 | 6.2 s | 562 ms | 3,965 B | 是 |

结果：4/4 下载通过，无超时、无回退 Fastly，临时文件全部清理。每次联网时发射功率由 80 限到 20（quarter-dBm，即 20 → 5 dBm），释放后恢复 80，自动重连状态复原。与 28344e84 在 12d1d95e 上的结果一致（列表 0.6–0.8 s，字体 6.7 s）。

验收后已写回正式 1.2.0（应用 SHA256 6a0134c6…，ELF 216b697ae…），启动至 InxRecent。

仍未验证：远距离、其他路由器/热点、用户界面里的下载流程（本次走同一 HttpDownloader 路径，但不经过界面）。
