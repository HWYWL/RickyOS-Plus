# Plus 原生动效移植 · 2026-10-24

> 后续集成：当前工作已转入 `codex/plus-integration-20261024`，并纳入图库与待机分支。
> 本文以下保留合并前的实验历史；当前集成范围与测试状态以
> [plus-integration-20261024.md](plus-integration-20261024.md) 为准。

## 当前交付边界

- 工作树：`codex/plus-reader-compact-20261024`，基线 `dc151595`（正式 Plus 1.2.6）。
- 本地实验，包含同工作树已有的紧凑阅读区；未合并其他并行分支。
- 未提交、未推送、未发布；经用户授权已刷入下述 Plus 测试机，不改公开 OTA。
- 仅 Plus 实际构建输入：`firmware/rickyos-plus-1.2/upstream/` 和本 port components。
- Pico 当前已验收的涟漪/主 Tab 动效包不变。
- 指定干净补丁 IDF：`9a97f6c54ec638111ce55cd36581b3c192f15207`，构建前后校验通过。
- 本文补充并更新早期 `native-page-turn-20261024.md`；该早期记录中的“两种样式、Plus 不走 lookahead、两处分配”仅描述第一轮，非当前状态。

## 用户入口

设置 → 显示：

1. **翻页动效（实验）**：关闭 / 掠页 / 柔弧 / 涟漪。默认关闭。
2. **主界面切换动效**：独立开关，默认关闭；控制首页、书库、存储、应用、设置五个根 Tab。

阅读涟漪优先使用本次点按的逻辑触点（10-bit 归一化坐标）；方向和触点打包为一次无锁 32-bit 请求，
渲染任务原子消费。按键/滑动/自动翻页无触点时使用页边中点。旋转/跨章/跳页不沿用旧触点。
只有前后成功的相邻同章节纯文字页才启用，含图、CSS 灰面板、首次打开、周期/手动清理仍走原路径。

## 与 Pico 不同的底层处理

- Plus 的 E0470A14-AF / ESP32-S31 采用 8-bit LCD 输出及 C LUT 变换；
  四像素输出字节是 MSB-first，内容边界的掩码不能照搬 Pico 的 lane 排列。
- Plus 原有提前备帧把下一相位生产与当前扫描队列尾部重叠。本次单独增加
  `turn_producer_tick`，不把 ISR 当前相位当成生产中的相位。
- 仅在两名 feeder 完成上一生产任务后，切换生产相位和热 LUT 窗口，再唤醒下一轮。
  ISR 仅读已提交的内部行队列；不会读 LUT bank/window 或 PSRAM。
- 保留 192 行预填、双队列、失败后的 feeder join、独占队列 reset 和 fatal quiescence guard。
- 保留阅读正文黑到黑重驱、未变化页眉页脚 HOLD、原全刷恢复与失败不发布 back buffer。
- 主 Tab 波前在唯一一次原生提交之前布置，不在新画面出现后追加滤镜/黑白帧；
  动效仅延迟内容区域，页眉/底栏不随内容做空间延迟。
- 主 Tab 沿用 Pico 已验收的完成时刻对齐、9 档空间延迟和全局空闲前缀省略。
  仅移动外围 HOLD，不删除有效驱动动作或内部 HOLD；保留最终三个稳定 HOLD。
- 不改 VCOM、充电参数、电压、PCLK/CKV、最小扫描节奏或校准波形源文件。

## 刷新策略与内存

- Tab 开关关闭：原有内容 8 / 小控件及导航 32 次的清理计数。
- Tab 开关开启：只在根 Tab 启用差分灰阶行程估算，内容达到 6 次全黑白等效行程或 64 次提交、
  导航达到 12 次等效行程或 128 次提交才清理；不是光学残影检测，也不是承诺永不全刷。
  手动全刷、Unknown 恢复、清理到期仍有效；只有成功物理提交推进 ledger。
- 不新增 framebuffer/线程/队列。原 PSRAM 相位 bank 与 16×16 延迟网格仍为临时分配：
  37 相位 + 原生尺寸共 41,156 B，64 相位上限 68,804 B。
- S31 热 LUT 窗口改为固定 2,304 B HP DRAM，阅读实际使用 2,048 B，Tab 使用 2,304 B。
  每条 32-bit 填充项只有低字节是动作，窗口去掉 padding；PSRAM 原相位 bank 和原校准动作不变。
  只预留这一个窗口，不常驻整屏或相位 bank；两处 PSRAM 分配全部成功才启动动效。
  临时 bank/延迟网格在两 feeder 结束后释放；HP 窗口属于唯一序列化 renderer、生命周期常驻。
  该常驻预算是为避免读书 heap 碎片导致逐页随机 OOM；每相位热表比原先缩小 75%。
- 灰阶压力 ledger 增加两个 64-bit 累计量，仍为固定尺寸、无 heap。
- 阅读比普通扫描增加最多 7 个起始延迟 tick，Tab 最多 8；是否更快不能只看 host 测试。
  真机耗时、功耗、灰阶和残影尚待验证。

## 可复现检查

```sh
python3 tests/test_native_page_turn.py
python3 tests/test_main_tab_motion.py
python3 tests/test_plus_main_tab_pipeline.py
python3 tests/test_native_gray_refresh_policy.py
python3 tests/test_plus_regional_gray.py
python3 tests/test_plus_phase_lookahead.py
bash tests/run_plus_port_tests.sh
python3 scripts/check_rickyos_plus_sources.py
```

Host 测试覆盖三种阅读样式、两方向/四旋转、原生步幅、不对齐内容边界、每像素完整相位序列与 HOLD、
两处 PSRAM OOM、固定窗口复用、origin 生命周期；实际 driver 测试覆盖单次提交前设置动效、取消和手动全刷不泄漏、
原 chrome 与图片页 polarity。LCD 并发测试覆盖实际双 feeder/ISR/原子环实现，提前备帧和同步回退、
每行相位/掩码、慢 producer、超时、失败回收、再次提交及生产 fatal 保护，启用 ASan/UBSan。
这些都不替代硬件时序或光学验收。

本轮最终结果：整套 `run_plus_port_tests.sh` 退出 0（原有硬件/环境限定 skip 保留，不能视为通过）；
上述六项动效/策略/事务测试及扩展 LCD 并发脚本通过。源审计 2844 个固定文件 / 196 处适配通过，
`git diff --check` 无错误。构建及最终增量链接后均重新校验 clean IDF、网络内存配置与时钟 owner。
实际镜像解析确认 ESP32-S31、16 MB flash、IDF `9a97f6c5`、校验和及 validation hash 有效。
完整日志随测试包保存在工作区 `output/firmware/plus-native-motion-20261024/`。

## 固件与待验收

- application：`RickyOS-Plus-native-motion-test.bin`（仅 Plus，非完整安装镜像）。
- 大小：4,994,544 B；应用分区 6,553,600 B，剩余约 24%。
- SHA-256：`96a8c2d45d91518f111ec73fa437c3b3c3205cd1e29eb2d78dc6bfa3228b252a`。
- 构建版本仍为 1.2.6，本地体验包不可当正式发布包。
- 已确认启动、SD 字体读取和首页显示提交成功；动效物理效果、连续翻页、菜单/休眠/唤醒、OOM 和音频并发尚待 Plus 人工验收。
- 刷入前确认设备芯片和身份、暂停其他刷机任务，检查现用 OTA 槽；不按 Pico 的串口/分区猜测。

## 本次真机安装

- 用户明确确认 Plus 已连接并授权刷入体验版。
- 自动识别 ESP32-S31，MAC `1c:29:04:d0:2a:50`；与既有 Plus 测试机记录一致。
- 实读并校验分区表、OTA 选择记录、原 app descriptor、16 MiB flash 及补丁 IDF bootloader。
- 当前启动 app1，应用地址 `0x650000`；只写入上述 SHA-256 对应的 4,994,544 B 应用。
- esptool 写后校验及第二次设备摘要比较均通过；bootloader、分区表、OTA 选择数据、NVS 写前后相同。
  未读写 SD 文件，也未改公开 OTA 源。此验证是串口安装，不是 OTA 验收。
- watchdog 硬件复位后被动串口观察 20 秒，确认加载 app1、检测到 Metalio Plus、原生面板初始化成功、
  SD 字体被读取并进入 InxRecent 首页，首次区域灰阶提交返回成功。
- 日志中 `SDK IMU not found` 为独立的 IMU 探测信息；不将它描述为本次动效故障或宣称全部外设正常。
  该启动观察窗口未见 panic/assert/重启循环。
- 安装报告：测试包目录 `installation-plus-20261024.json`；启动日志：`plus-boot-20261024.log`。

## 阅读动效入口修正（第二轮）

用户反馈主界面切换可接受，但阅读动效不明显/未生效。第一次复现采集只捕捉到开书，
后续采集仅有启动及电量信息，未得到连续翻页证据；不能据此声称已证明用户的具体配置或唯一根因。

源代码确认一个独立缺陷：Plus `nativeGray16Page()` 原先纯文字只在文字抗锯齿开启时进入原生提交，
动效只能在原生提交中消费，导致 AA 关闭时设置了涟漪也不可能触发。
修正为有效动效样式 1..3 也可选择原生 4-bit 事务；`readerTextWeightCurve()` 仍尊重 AA 关闭，
正文覆盖度映射仅 0/15，不擅自开启文字抗锯齿。不影响动效关闭的原路径、夜间模式、其他灰阶设备、
图片/灰面板、周期/手动全刷及主界面动效。缓存仍维持原有资格门槛，不随此修复额外开启。

新增日志仅在动效选项开启且有翻页请求时打印 style/AA/原生路径、相邻页/朝向/清理条件，
不打印书名、正文、坐标或用户数据。用于下一次实机确认是否还有其他 guard 阻止触发。

- 扩展生产 gate 测试覆盖 AA 开关 × 样式 0..4，夜间模式、非 16 灰和全部字重仍有验证；
  NativeTurn、driver、reader chrome、主 Tab 实际提交流程回归通过。
- 修正版：`RickyOS-Plus-native-motion-reader-fix.bin`，4,994,896 B。
- SHA-256：`c135ca9bbcf2cdefe8505de7f15c2fe19f15d411a75b9faea0221af5bf0dac87`。
- 沿用 clean IDF `9a97f6c5`，源审计 2844 文件 / 196 适配通过。
- 第一份包和安装报告保留；修正版未推送、未发布。

修正版已安装同一 Plus app1 / `0x650000`，写后校验及 bootloader/分区表/OTA/NVS 保持检查通过，
watchdog 重启并正常到首页。报告为 `installation-plus-reader-fix-20261024.json`。
修正版补测 native gate、reader cache、JPEG/idle、四旋转 geometry、native phase、driver/chrome 及主 Tab 事务通过；
前述整套回归结果针对第一轮整包，不能冒充修改后重新跑了整套。

实机 `plus-reader-fix-live-20261024.log` 已捕获：

```text
reader style=3 aa=0 native=1 dir=1 previous=1 images=0 inverted=0
gate fast=1 panel=0 adjacent=1 orientation=1
NativeTurn: style=3 orientation=0 right=1 phases=37 ticks=44 scratch=41156
lookahead ... frames=44 cycle=598ms
PlusDisplay: turn GL16 body=60/33/1082/618 result=0x0 ms=782
```

因此已经证明当前 AA 关闭时阅读涟漪请求到达实际生产扫描并成功返回，原 gating 会拒绝这组设置；
尚不将日志成功等同于用户已认可动画视觉效果。主 Tab 没有更动。

## 连翻欠载复现与第三轮候选

第二轮设备实测并非完全通过：用户连续翻页后出现白屏狂闪。
`plus-reader-fix-live-20261024.log` 先记录多次 style=3 / 44 tick 成功提交，
随后含图页/周期清理之后又有成功纯文本涟漪，最后持续出现 `line buffer underrun`、
reader 提交 `0x400`，然后原安全恢复执行黑白全刷。这才是本次狂闪的直接可观测原因。
不能以早先的一次成功代替连续翻页验收。

第三轮候选的窄范围改动：

- 仍保持 AA 与动效独立，不改主 Tab 效果。
- 热 LUT window 的容量不变（阅读 8 KiB / Tab 9 KiB），增加 `MALLOC_CAP_DMA` 限定。
  补丁 IDF 的 `components/heap/port/esp32s31/memory_layout.c:50` 中，普通 RAM 有 DMA 能力，
  RTC RAM 也满足 INTERNAL/8BIT、但没有 DMA 能力。此限定保证热表不落 RTC。
  “旧欠载唯一根因就是 RTC”仍是待验证假设，不当作已证明事实。
- 高速 RAM 不足仍在物理驱动前走原刷新，不启动半个动效；未新增内存容量、帧缓冲、线程或队列。
- 原有 sticky first-fault 信息现在也在 lookahead 错误路径输出（含生产 tick/current），
  native begin 输出热表地址与 CPU MHz；不在 ISR 做日志。
- host ASan/UBSan 检查加入 DMA 能力断言、三处 OOM 和 lookahead 故障回收；通过。
- 原电压、波形、行周期、192 行预填及失败后 Unknown 恢复不改。

候选 `RickyOS-Plus-native-motion-hp-ram.bin`，4,995,120 B，SHA-256：
`cc05fb6ffda9aa74b75195ce52089342c4561feae725edce344e3616bca7c666`。
最终使用原始 Ninja 构建成功、clean IDF `9a97f6c5` 校验通过；未采用 strip-debug 临时链接尝试。

电脑磁盘不足曾使几次 LTO 链接失败。只删除此次失败前缀 `ccv89GcQ.ltrans*.o` 的 60 份临时目标（约 62 MiB）
及三份无进程占用的 PlatformIO 下载缓存（约 127 MiB，可重新下载）；未删源码、工具安装目录、用户文件或固件。
没有删除重复 application objects；该清理方案仅调查，未执行。空间恢复后正常链接完成。

## 动效随机消失：实机确认与第四轮候选

第三轮后用户先反馈点击无响应。串口实际停在 `CHIP_USB_UART_RESET` / ROM 下载模式，
并有 esptool 握手数据；同期电脑另有 Pico 任务，但未证明哪一个进程复位了 Plus，不能直接归责。
未改写固件，核对 Plus MAC 后用 watchdog 恢复，随后仅对 Plus 用 TIOCEXCL 独占记录。

同一份 `plus-hp-ram-exclusive-live-20261024.log` 证实用户的“时有时无”：
相同有效 reader gate 有时成功运行 44 tick，有时 `Scratch unavailable; ordinary refresh`。
因此第三轮的 HP/DMA 内存限定造成了可观测的逐页申请失败，不能将普通刷新成功算作动效通过。

第四轮只改 hot working set，不更改光学波形、帧周期、队列或抗锯齿设置：

- 单一 renderer 在链接时预留 `DRAM_ATTR` 9×256 B，检查 ELF 中处于 `.dram0.data`、HP 地址 `0x2f01fe00`。
  不再逐页 malloc 8/9 KiB；PSRAM bank/grid 仍可失败并在上屏前安全退出。
- 每相位把原 bank 的 256 个动作字节去 padding，C converter 直接查 byte LUT。
  保留四个 word 展开、S31 MSB-first、内容边界 HOLD 和原全部相位；没有减少灰阶。
- 热 converter 与边界混合 helper 明确在 IRAM；ELF 符号检查确认，避免 LTO 把新增热路径放到 flash。
- Host sanitizer 检查所有 old/target 项、全部原相位、三样式/四旋转/两方向及不对齐边界；
  LCD 并发测试另外运行真实 compact converter 两 feeder 路径，以及原 fault 注入 lookup 路径。
- 刷机安装器也在 macOS 独占 Plus 串口，关闭即释放，不中断并行 Pico 任务。

候选 `RickyOS-Plus-native-motion-compact-lut.bin`，4,997,488 B，SHA-256：
`714aa889fb91098074d369372d74900de6d0bb983dc2a200d22df7f075186133`。
clean IDF、源审计、时钟 owner、网络内存和镜像摘要检查通过；应用分区仍约 24% 空余。
NativeTurn 与扩展 LCD ASan/UBSan 通过；第四轮完整 `run_plus_port_tests.sh` 退出 0，
保留原有硬件/环境限定 skip，不把 skip 当通过。日志 `.local/plus-native-motion-compact-lut-tests-20261024.log`。

第四轮已刷入同一 Plus 的 app1 / `0x650000`，写后摘要验证与 bootloader/分区表/OTA/NVS 保持检查通过，
未操作 SD，未发布。报告 `installation-plus-compact-lut-20261024.json`。
watchdog 重启，确认原生面板、SD 字体初始化及首页灰阶提交成功；没有把开机成功等同于动效验收。
独占日志为 `plus-compact-lut-live-20261024.log`；已请用户连翻检查。
连续翻页、光学效果及旧欠载是否消失，仍按后续日志和人工反馈核验，尚不宣称问题全部解决。

## 第四轮用户验收及 Plus 正文抗锯齿定位

用户完成连续翻页后反馈“看起来没什么问题了”，此轮动效由用户确认通过。
`plus-compact-lut-live-20261024.log` 的 240 秒采集窗口实际覆盖了主 Tab 操作，
没有覆盖用户随后全部 20 页正文；不要把人工验收虚写成已有完整连翻日志。

用户另反馈正文像先出现文字、再做抗锯齿，明确先定位 Plus 正文，不修改 Pico。
本次只做代码/实际表分析，未改驱动波形、未重刷当前已验收动效包：

- 当前 app `EpubReaderActivity.cpp:3056` 的 native 16-level 分支明确跳过 B/W base；
  `3087` 开始原生 frame、`3097` 绘制完整正文、`3175` 才唯一提交。
  `GfxRenderer.cpp:2037` 的 BW 模式是内存 proxy，并非显示一次 B/W；begin/clear 不启动面板。
- `MetalioPlusDriver.cpp:133` 消费完整 4-bit target，普通 reader profile 只调用一次 `refreshTurn`。
  因此当前 native 文字路径并不是 CPU 先上普通文字、算完 AA 再补一帧。
- `tests/report_plus_reader_waveform.py` 编译生产 `my_waveform.c` / trim 并用 ASan/UBSan
  打印实际 `MY_MODE_READER_TURN` 动作。白→黑为 13 dark、在相位 12 结束；
  白→12 级灰为 13 dark 后 7 white、相位 19 结束；白→14 级灰最后相位 22。
  黑→黑仍是既有 5 white + 5 dark 保墨重驱。均为 zero-based 相位。
- 这些是电驱动指令，不是光学亮度测量，也不能证明某个用户/旧版本/特定页面只可能有这一个原因。
  它解释了当前表为何可能看起来文字先浓再回灰：AA 灰边和黑笔画不是同一时刻停止驱动。
  仅在提交前 sleep 或多缓冲，无法隐藏同一次物理驱动的中间状态。
- 原生文字单次提交回归、AA-off 二值覆盖度测试及生产波形全部 pulse 检查通过。
  下一步应单独 A/B 验证正文灰阶收敛/过冲，而非加入假等待、再次叠加灰帧、关闭 AA，
  或直接照搬 UI 完成对齐并宣称解决；调整电驱动时序需同时核验墨色、残影和速度。

随后用户明确授权正文收敛时序 A/B 并直接刷入。第五轮候选、范围、检查和安装记录见
[Plus 正文时序实验](plus-reader-text-finish-20261024.md)；第四轮已验收包保留不覆盖。
