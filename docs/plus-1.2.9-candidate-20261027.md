# Plus 1.2.9 候选（2026-10-27）

分支 `codex/plus-release-1.2.9`。正式包必须用下面这组参数构建（翻页动效参数是编译期开关，默认构建不等同于验收版本）：

```sh
RICKYOS_IDF_PATH=/your/patched-esp-idf-v6.1 \
RICKYOS_PLUS_UI_WHITE_MAINTENANCE=1 RICKYOS_PLUS_READER_BLACK_RESTORE_PHASES=4 \
RICKYOS_PLUS_READER_RETAINED_BLACK_PULSE=17 RICKYOS_PLUS_READER_QUALITY=1 \
RICKYOS_PLUS_READER_EARLY_ERASE=1 RICKYOS_PLUS_READER_FINE_RIPPLE=1 \
RICKYOS_PLUS_READER_MOTION_PERIOD_US=0 RICKYOS_PLUS_READER_RIPPLE_SPREAD=7 \
RICKYOS_PLUS_READER_SWEEP_SPREAD=3 RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES=0 \
bash ports/metalio-e-ink4-plus/scripts/build_rickyos_plus.sh
```

不带 `RICKYOS_PLUS_FIELD_DIAG`（每 5 秒写 SD 记录）。诊断时加 `RICKYOS_PLUS_READER_TIMING=1`（分段计时）。

## 翻页提速（实机串口计时）

- TPS 上电：去掉两段固定 50 ms 等待，改为逐毫秒探测/查 PWR_GOOD，约 100+ ms → 41 ms。
- 动效错开：涟漪 15 → 7 拍，掠页/柔弧 4 → 3 拍（用户先后认为 5/2 过快、10/3 偏慢）。
- 上一页驱动期间按下的翻页（驱动结束 150 ms 内到达）不放动效。
- 下一页预渲染不再被单纯的触摸或翻页保护打断（按键、更新、切换仍会打断）。
- 结果（t4/t10）：涟漪约 845 ms、掠页约 770 ms、无动效约 735 ms；最初涟漪约 960 ms。
- 每相约 12.8 ms，已贴近 12 ms 下限；48 相波形本身约 620 ms，不改波形就无法再大幅缩短。

## 左侧残影（新字左边的淡副本）根因

竖屏最左一条 = 每帧最后扫描的行。面板经 2×8 行 bounce 缓冲送数，每块约 136 µs。
诊断计数显示迟到的补数全部发生在第 470～590 行，且 **100% 与每 10 秒的 `[MEM]` 堆统计日志重合**：
`largest free block` 查询在堆锁内遍历堆（含 16 MB PSRAM），锁会关中断数百微秒，EOF 中断进不来，
DMA 把 16 行之前的数据再送一遍，即“右边那个字的淡副本”。

- 修复：渲染/驱动期间（`RenderLock::peek()`）推迟内存统计。修复后 121 次刷新 0 次迟到。
- 同时保留：`CONFIG_LCD_RGB_ISR_IRAM_SAFE=y`、GDMA 中断优先级 3、GDMA 总线优先级 5；补数迟到改为仅在发生时 `LOGW`。
- 排除过的方向：缓冲加大（16 行超过单描述符 4095 B）、帧尾后才启动下一相供数（`RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME=1`，每相慢约 2 ms 且无效，保留为可选对照，默认关）。
- 以后若再出现，看串口 `refill stale=` 警告；任何会长时间关中断的代码都不要在驱动期间运行。

## 退出阅读的内存

实测（t10）：内部内存退出后回到进入前水平（相差约 0.4 KB），PSRAM 总量全部归还；
PSRAM 最大连续块由 12.06 MB 降到约 10.75 MB，主要是保留的阅读字体源（供界面缺字回退）。
