# Plus 正文灰阶收敛时序 A/B · 2026-10-24

## 授权与边界

用户已验收 compact-LUT 动效包，继而要求先优化 Plus 正文灰阶收敛，改好直接刷机。
仍为 `codex/plus-reader-compact-20261024` 本地实验，不改 Pico、不推送、不发布 OTA。
既有未提交动效/紧凑阅读区保持；不合入其他分支或发布电量改动。

上一包 `RickyOS-Plus-native-motion-compact-lut.bin` 保留不覆盖，SHA-256：
`714aa889fb91098074d369372d74900de6d0bb983dc2a200d22df7f075186133`。

> 后续已与图库／待机分支集成，白底正文保留本记录的验收波形，背景使用独立表。
> 最新分支与整包验证见 [plus-integration-20261024.md](plus-integration-20261024.md)。

## 已确认与待确认

- 当前 Plus 正文先在 4-bit frame 中完成绘制，然后唯一原生提交；不是先显示 B/W 再补 AA。
- 生产 reader-turn 表的白→黑在相位 12 结束，白→12 灰在相位 19 结束，部分过渡到相位 33。
  这些是电驱动指令，不是测得的屏幕亮度；用户实际看到的过冲需光学 A/B 验证。
- 本轮不会声称“把字藏到刷新结束后一次显示”：EPD 驱动过程仍可见，延迟 CPU 提交无此能力。

## 本轮实现

- 应用只在原生普通阅读事务、`FAST_REFRESH && !panelTurn` 时发送一次提示，
  `!pageHasImages && textAntiAliasing` 为真才启用。图片、CSS 面板、夜间、周期/手动清理原路径不变。
- 提示通过 `HalDisplay::setNextGray16ReaderTextFinish()` 转到 Plus driver；其他设备是 no-op。
  与 `nativePageTurn` 设置独立：动效关闭也能尝试文字收敛，AA 关闭则沿用已验收动效。
- 每一个 changing old/target 对，目标为黑/灰时，把提前结束的序列后移到第 33 相收尾。
  只重排序列外侧 HOLD；不删减、不重排有效动作或内部 HOLD。保留末尾三个稳定 HOLD。
- 目标白色，以及全部相同 old/target 单元完全不变：旧文字仍按原时序清白，
  0→0 的 5 白+5 黑保墨重驱不动。没有承诺灰边自身的物理过冲已消失。
- 复用原相位 bank、16px delay grid 和 2,304 B 固定 HP window。动效开启没有新增分配；
  动效关闭/AA 开启时新增使用现有两块临时 PSRAM（37 相 bank + 网格共 41,156 B）。
  不能放到 16 KiB 渲染栈中；不常驻整表或新增 framebuffer，待两 feeder join 后释放。
- 关闭动效：delay grid 全零、仍 37 tick；开动效：原空间 delay，仍 44 tick。
  不改 UI 完成对齐、lookahead 双队列/192 行预填、phase minimum、PCLK/CKV、VCOM、充电参数。
- 新提示提交时由唯一显示 owner 一次消费；取消、profile/页面切换、休眠和失败恢复清除，
  不泄漏到下一次图片/UI/清理。失败不发布光学 back baseline；PSRAM OOM 在驱动前走原表。
- 变化的阅读页眉/页脚字形会使用同一事务表；既有 chrome HOLD 掩码继续屏蔽完全未变化的 chrome。
  不能把本实现描述为新增了一个正文局部扫描或第二次灰阶刷新。

## 检查

- `native_turn_host.c` 新增生产 MY_MODE_READER_TURN 全 256 old/target 对、4 样式（含关闭）×4 朝向：
  逐相比较移动前后序列，保留所有有效 pulse/内部 HOLD、目标白/相同对逐字节不变；
  实际 compact feeder 输出、37/44 tick 和两处 OOM 通过 ASan/UBSan。
- 实际 driver：空间动效开/关均能消费提示，profile 切换/取消不泄漏，Unknown/清理仍走原波形。
- 应用实际 arming block 的 AA×动效×图片×panel×刷新矩阵，以及 HAL 在 Plus 与非 Plus 的行为通过。
- 实际 LCD 双 feeder/ISR 测试增加了 compact completion-only 37 tick 的并发与故障回收检查。
- 完整 `run_plus_port_tests.sh` 退出 0，原有硬件/环境 skip 保留，不视为实机验收。
- 2844 pinned source / 196 adaptations 审计通过；干净补丁 IDF `9a97f6c5`、时钟 owner、网络内存检查通过。
- ELF 中 hot window 为 `.dram0.data`、2,304 B、地址 `0x2f01fe80`，热 converter/edge helper 在 IRAM。
- `git diff --check` 通过。没有通过增加 sleep、增加扫描相数或多推一张 B/W 图实现。

## 本地体验包

- `RickyOS-Plus-reader-text-finish-test.bin`，4,997,872 B；仍为本地 1.2.6 标识，非发布包。
- SHA-256：`600c987210ee3e12c076052785b53328cc99096ed05f76db50584a71873a09de`。
- 对比重点：正文是否从“粗字再收灰边”更趋整块稳定；是否出现空心轮廓先显、主观响应变慢、墨色变淡、残影增加。
  相数不增加不等于感知延迟不变；本轮把部分黑笔画启动后移，必须由用户比较后决定是否保留。
- 上一动效包是回退基准；不可把本轮 host 通过或串口安装通过当作光学验收、OTA 验收。

## 安装记录与待验收

- 核实 ESP32-S31、16 MiB flash、Plus MAC `1c:29:04:d0:2a:50`；安装器独占目标串口。
- 只写当前 app1 / `0x650000`，写后 MD5 校验通过，实际镜像 SHA-256 与上述包一致。
- bootloader、分区表、OTA 选择、NVS 与写前一致；没有操作 SD，也没有改公开更新源。
- 报告：`installation-plus-reader-text-finish-20261024.json`；复位后确认 bootloader/应用
  IDF `9a97f6c5`，面板和 SD 字体初始化、进入首页及首页灰阶事务成功。
- 独占日志：`plus-reader-text-finish-live-20261024.log`，记录窗口延长为十分钟，避免用户操作晚于采集结束。
- 已请用户开启阅读抗锯齿、保持原动效选项，同章节连翻比较。尚未宣称实际正文效果改善，
  尚未以启动成功代替 `ReaderFinish` 真正触发、连续翻页稳定性或光学验收。
- 整套回归后额外补测 reader 事务物理失败：提示清除、back baseline 不发布，后续 GC 恢复；ASan/UBSan 通过。

## 第六轮：保留收敛改善，调整黑笔画稳定尾部

用户反馈第五轮“好很多”，但感觉“有点变淡”。日志确认本次真实正文触发：
`style=2 aa=1`、ReaderFinish 连续 12 次、44 tick、每次 result=0、单次面板事务约 752～754 ms，
未见供数欠载、Scratch OOM 或恢复回退。这证明提示确实工作且此批扫描完成，
不能据此推断真实反射率、将变淡唯一归因为某一物理机制，或与不同样式的旧日志直接作速度对比。

只在 `ricky_reader_lock_text_finish()` 调整一项：目标纯黑（to=0、from!=0）从相位 33 改到 31 收尾，
末尾 HOLD 由 3 增为 5；灰色目标仍到相位 33 收尾。按本批约 12.84 ms/tick 的扫描节奏，
黑笔画多两相、约 25.7 ms 的稳定尾部。总相数依旧 37/44，不增加延时或面板驱动次数。

- 黑笔画原全部有效 pulse、数量、相对顺序和内部 HOLD 不变，只比第五轮提前两相。
- 灰边、目标白、原 0→0 重驱、UI 及 AA 关闭路径完全不动。
- 不改字重、字体覆盖曲线、VCOM、电压或充电参数；没有添加额外黑脉冲。
- 只是小幅 A/B，不宣称多两相 HOLD 必然提高最终墨色；需用户比较是否更深，闪动是否反弹。
- 逐 old/target 相位模型同步更新为黑目标 last=31、灰目标 last=33，5 个黑尾 HOLD 验证通过。
  NativeTurn sanitizer、实际 driver、生产波形原动作、AA/图片/cleanup arming 回归通过。
  此窄改后未重跑整套 suite；整套 suite 通过记录针对第五轮，同一供数代码未改变。
- clean IDF、源审计、网络内存、时钟 owner、镜像检查及 diff-check 通过。

候选 `RickyOS-Plus-reader-black-settle-test.bin`，4,997,888 B，SHA-256：
`ce84e3f1ab58c536e4c1f81525596bd5eaf854185f98e19ec4829f3616064098`。
第五轮和第四轮包均保留，可分别回退；不发布或推送。

第六轮已安装到同一 Plus app1 / `0x650000`，MD5 校验和 bootloader/分区表/OTA/NVS 保持检查通过，
没有操作 SD。报告 `installation-plus-reader-black-settle-20261024.json`。
重启后应用 ELF `632b104da...` 与本包一致、原生面板和 SD 字体初始化及首页提交成功。
独占日志 `plus-reader-black-settle-live-20261024.log` 已开启 600 秒窗口；
已请用户保持相同 AA、动效和前光设置，对比墨色及闪动是否反弹，尚待光学反馈。

## 第七轮：黑尾从 5 相到 7 相的小步 A/B

用户反馈第六轮“有一些改善”，希望墨色再深一点。本轮仍不改字体/字重/覆盖度或电压，
只将目标纯黑 last 从相位 31 提前到 29，留下 7 个 trailing HOLD；灰目标 last=33 不变。
相对灰边提前 4 tick、按最近实测节奏约 51 ms，比第六轮多 2 tick 稳定尾部。
有效 pulse 数量和顺序、内部 HOLD、目标白、0→0 保墨重驱、UI、图片及 37/44 总 tick 全部不变。
这不是额外加黑脉冲或再推 B/W 帧，仍需光学比较；更长 HOLD 不等于可保证单调提高黑度。

第六轮日志记录 15 次 ReaderFinish（包含一次空间 style=0 / 37 tick，证实 AA 收敛不依赖动效），
此批 native 事务均正常返回，未见欠载或临时表 OOM。人工反馈是唯一墨色改善依据，不声称有光度测量。

- 真实波形 256 对 × 4 样式 × 4 朝向的 native sanitizer 测试模型改为 black last=29，
  验证 7 个尾 HOLD、所有原 pulse/内部 HOLD、保墨与清白不变；通过。
- 实际 driver 单次提示/失败回收、生产原波形、AA/图片/清理资格矩阵回归通过。
  本轮未重复整套 suite；没有更改 feeder、ISR、队列、相位节奏或应用输入。
- clean IDF `9a97f6c5`、2844/196 源审计、网络内存、时钟 owner、镜像及 diff 检查通过。
- 候选：`RickyOS-Plus-reader-black-settle-7-test.bin`，4,997,888 B。
- SHA-256：`fe0737cb4265dc5ae38c5178f2d5e402374b2e8f52ca49b3c8601e57ae709eec`。
- 第六轮包 `ce84e3f1...` 继续保留；如先粗后细、空心轮廓或残影变差，回退它作 A/B。

第七轮已写入同一 Plus app1 / `0x650000`，校验和启动通过，应用 ELF `e064f4e89...` 与本包一致。
bootloader、分区表、OTA 选择和 NVS 未变，未操作 SD。安装报告
`installation-plus-reader-black-settle-7-20261024.json`；600 秒独占日志
`plus-reader-black-settle-7-live-20261024.log` 已开启，当前已确认首页提交成功。
已请用户同设置比较正文墨色和闪动/轮廓/残影，尚待本档实机视觉反馈。不推送、不发布。

## 当前停留版本：用户要求先保留第七轮

用户反馈：“感觉好一些了吧，反正应该还算能对付。先就这样。”
记录为**主观改善、暂时保留，继续使用观察**，不夸大为墨色已校准、彻底无闪动或全部问题解决。
停止进一步参数微调，第七轮 `fe0737cb...` 是当前设备体验包；第六轮 `ce84e3f1...`
及动效基准 `714aa889...` 保留可回退。当前改动仍未提交、未推送、未发布，用户没有授权正式发布。
已结束本轮独占串口采集，避免阻塞其他测试任务。
