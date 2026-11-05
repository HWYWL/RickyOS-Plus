# Plus 阅读性能：进度与审查交接 · 2026-10-20

## 去哪里看、当前是什么状态

- 私有协作仓库：`https://github.com/chinoryunqin/RickyOS-dev`。
- 本次推送分支：`codex/plus-online-fix-1.2.2`。前一已推送检查点 `490cfc7a`。
- 只针对 Metalio E-Ink4-Plus / ESP32-S31 / E0470A14-AF。Pico 和普通 E-Ink4 不使用本候选。
- 构建入口是 `ports/metalio-e-ink4-plus/`，不是仓库根部通用应用。
- 本记录随代码一起提交，供协作者审查；不创建 Release、OTA 清单或修改公开仓库。
- 用户要求先推送给他人查看；本次不继续改刷新时序、加新功能或重新刷机。

## 本次提交内容

| 项目 | 实现与证据 | 尚未完成 |
|---|---|---|
| 相位准备与扫描重叠 | 复用两个生产线程、原队列及 ready 数组，保留 192 行预填与 48 相表；普通阅读提交约 0.76 s | 默认关闭的实验；光学质量、音频并发和长时稳定性仍需验收 |
| JPEG 平滑缩放写入 | Plus 原生竖屏改为列遍历；200 组真实回调差分用例像素一致 | 没有同页同缓存状态的前后实测，不能量化独立图片提速；PNG/完整渐进 JPEG 未改 |
| 空闲预渲染重试 | 输入在回调内取消后，重新等待安静窗口；切换/排队刷新/禁用时不重试 | 快翻仍大量未命中，图片无命中；不能说缓存已完全修复 |
| 重复状态栏绘制 | 跳过随后会被 native begin 清空的 B/W 绘制；回退仍重画完整页面 | 最后一次状态栏仍花约 110 ms，需进一步拆查 |
| 计时与回归 | 可选逐页/缓存日志；并发失败清理、生产回调差分、真实调度循环测试 | 主机模拟不代表面板光学、功耗或长期稳定性 |

具体过程、失败实验、取消边界和安装过程：

- [相位准备重叠实验](plus-reader-speed-lookahead-20261020.md)。
- [绘制/预取优化与 101 次翻页实测](plus-reader-paint-speed-20261020.md)。
- [前一已验收检查点](plus-tested-checkpoint-20261020.md)，包含反色/图像质量、宽图及锁屏 Home 修复。

两份实验日志中的“未提交/未推送”描述的是当时阶段；本次交接提交包含它们及相关代码。
后续用本分支 `git log` / 提交散列确认已同步进度，不把历史描述当作当前 Git 状态。

## 实测摘要与限制

设备运行本候选时，用户完成 101 次可完整配对的逐页计时，含 94 次普通刷新和 7 次周期清理。
以下均为普通刷新中的**页内绘制＋提交**，不含章节加载/分页或输入排队：

| 类型 | 次数 | 正文准备均值 | 页内总均值 |
|---|---:|---:|---:|
| 文字缓存命中 | 13 | 9.7 ms | 894.2 ms |
| 文字缓存未命中 | 74 | 244.9 ms | 1126.0 ms |
| 含图缓存未命中 | 7 | 1230.0 ms | 2115.1 ms |

普通提交本身均值 764.1 ms（755–771 ms）；7 个含图页总耗时 1895–2568 ms。
总计 15 次缓存命中（含 2 次周期清理）。快翻测试比例不代表正常阅读命中率。
本轮全部完整提交返回 `result=0x0`，未见 panic/abort/欠载信息；用户尚未明确评价本轮灰度/残影，
不声称完成光学或长期稳定性验收。两次旧分页缓存版本 79 被拒绝，后续进入对应章节并继续翻页；
Plus 当前分页格式为 81，本轮没有再变更格式或用户阅读进度。

上轮短测提交 763.3 ms，与本轮基本相同。本轮没有继续压缩驱动时序。
更早的旧调度 25 次提交平均 852.9 ms，与上轮 6 次的差异约 10.5%，但不是同页配对，
不能作为本轮整页或相对竞品的提速百分比。

## 复现与审查入口

从仓库中的 Plus 目录执行；先按 [README](../README.md) 准备固定依赖。

```sh
python3 scripts/restore_rickyos_plus_sources.py
python3 scripts/check_rickyos_plus_sources.py
bash tests/run_plus_port_tests.sh
```

应用设计固定来源 `a42d939944c4495a8731b194171e365c85e5a000`，SDK
`96de1be6ce08eb732909e6e8149af8f892b9a2c5`；2,844 个来源文件、102 项 Plus 适配。
`firmware/rickyos-plus-1.2/upstream/` 是忽略的还原目录；其改动随受版本管理的
`upstream-plus.patch` 和 `port-changes.json` 保存，不能因 Git 状态没有列出这些源码就认定未保存。
不要直接修改根目录同名文件来修本固件；那不是此候选的编译输入。

本次构建使用 ESP-IDF `9a97f6c54ec638111ce55cd36581b3c192f15207`。
`RICKYOS_IDF_PATH` 选择该依赖的本地目录，`RICKYOS_BUILD_DIR` 可选独立构建目录；
然后用以下标志复现体验候选：

```sh
RICKYOS_PLUS_VERSION=1.2.2-dev \
RICKYOS_PLUS_PHASE_LOOKAHEAD=1 \
RICKYOS_PLUS_READER_TIMING=1 \
bash scripts/build_rickyos_plus.sh
```

`READER_TIMING` 默认关闭，只增加诊断日志。`PHASE_LOOKAHEAD` 自 2026-10-20 起默认打开（用户真机使用一天后确认显示正常），并已扩展到界面区域刷新（GL16）；`RICKYOS_PLUS_PHASE_LOOKAHEAD=0` 可编出旧节奏做对比。
本候选未启用 `GRAY_CALIBRATION`、`READER_SHORT_WAVEFORM`、`TAB_BENCH` 或 `SCREEN_TOUR`；
不要误开失败的光学校准/短波形实验。普通 UI / GC 仍走原路径。
JPEG、空闲重试和状态栏改动为 Plus 专用逻辑，不依赖计时开关。
构建成功不等于已允许正式发布，不要执行 IDF 提示的整包 flash 命令。

主要源码：

- `components/metalio_board/display/output_lcd/{render_lcd.c,phase_lookahead.inc}`：相位所有权、预填、时序和故障退出。
- 还原的 `src/activities/ActivityManager.cpp`：空闲回调内取消后的重试。
- 还原的 `src/activities/reader/EpubReaderActivity.cpp`：计时、缓存入口及重复绘制省略。
- 还原的 `lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp`：保持采样的列遍历。

以上 `components/` 路径在 `firmware/rickyos-plus-1.2/` 下；还原源码路径在其 `upstream/` 下。
新增测试入口为 `tests/test_plus_phase_lookahead.py` 和 `tests/test_plus_reader_paint_speed.py`。

## 已验证、设备固件与本地材料

完整回归通过：41 个 Python 入口、3 个顶层原生程序。并发调度独立重复 3 次通过。
其中首次与构建并发的旧 GC 主机测试失败已保留并说明，修正仅作用于 host 调度/回调模拟；
不能以 host 模拟结果保证真机欠载安全。完整 S31 构建、触控链接、单一时钟、来源校验通过；
SDK 现存警告保留。不是所有机型的 CI 或硬件验收。

已装体验候选 `1.2.2-dev`：

- 应用大小 4,863,376 B。
- BIN SHA256 `ebbd2f2db8cd8415bccc17d7835022a2a09723a146b1e138f276960b6aa69856`。
- ELF SHA256 `27842e50738f2734fe322dddb798707c0ea614ffc7ce9c6bd44d85376fe170b7`。
- 仅 app0 应用分区写入，设备摘要验证成功，显式重启后核对 ELF 与首页。

二进制/ELF/MAP、用户书籍、设备身份/安装记录和原始日志留在本机 `.local/`，不进入提交。
对应目录/文件：`reader-paint-speed-20261020/`、
`device-installations/reader-paint-speed-20261020/`、`reader-paint-speed-live-20261020.log`、
`tests-reader-paint-speed-final-20261020.log`、`build-reader-paint-speed-20261020.log`。
上一体验版与已验收的更早固件归档均保留；需要回退时仍须重新核对设备和活动分区。

## 下一步建议（尚未实施）

1. 查含图缓存未命中的原因，分别覆盖跨章首图、同章前后翻和空闲窗口不足，不先假定是解码器慢。
2. 改善快翻时的缓存覆盖/取消策略，同时验证菜单与退出响应；不引入不可取消的后台长任务。
3. 拆查约 110 ms 状态栏开销，再决定复用范围；保留时间、电量和进度的正确更新。
4. 用同机同书同页、相同缓存状态配对测量，单独记录纯文字/含图/跨章/清理页；补光学、
   录音/音频并发、长时内存与待机恢复验收后，再决定默认启用或正式发布。

## 附：本次用户询问的格式支持

当前 Plus 的 `LibraryBuilder.cpp:191–193` / `FsHelpers.cpp:179–180` 实际书籍识别为
EPUB、TXT、Markdown；`ReaderActivity.cpp:30–39` 进入 EPUB 阅读管线。
没有 MOBI / AZW / AZW3 的直接阅读解析器。`RickyStorageLayout.cpp:21` 虽列出 `.mobi`
与 `.azw3`，但只是旧文件夹迁移名单，不能当成支持阅读的证据。
暂时须先转换成 EPUB，改扩展名不等于转换。本次没有添加格式支持代码。
