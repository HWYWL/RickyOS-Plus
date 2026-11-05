# Plus 阅读状态栏刷新实验 · 2026-10-23

最新状态：用户已试用并确认包含本改动的 `8f5e0edd…b506eb0` 整包可以发布，明确豁免本次 OTA 实测；详见 [正式发布记录](plus-1.2.6-release-20261023.md)。以下「未发布/待办」保留为此前实验阶段的历史记录，不能抹掉黑白通道未覆盖和未执行 OTA 的事实。

分支 `codex/plus-reader-status-refresh`，基于已推送的 `codex/plus-1.2.6-release@79acaf6d`。仅 Plus，未改变已发布 Pico 或正式发行目录，未自动刷机、提交或推送。

## 原因和范围

`components/rickyos_display/src/driver/MetalioPlusDriver.cpp` 的 `refreshTurn()` 原先每次调用 `epd_hl_update_screen_full(... MODE_GC16_FAST ...)`。其阅读波形为解决淡墨，黑到黑会有五相擦除、五相补黑；整屏更新也会重新驱动未变化的状态栏。这不是简单少调用一次 `renderStatusBar()` 能解决的：绘制端还要保留页码、章节、时钟和电量的新像素，显示端则要区别正文与未变的界面元素。

## 实现

- EPUB 共用阅读路径（包含转换进入的格式）在普通 FAST、无精排底色清理时，将实际正文矩形作为一次性提示传给 renderer → HAL → Plus 驱动。
- renderer 复用原有 UI 区域映射，正文矩形在当前屏幕方向下只旋转一次；支持横竖屏及反向，不硬编码底栏高度。
- 新 `epd_hl_update_screen_reader()` 使用原有差分缓冲区。正文仍执行既有补墨；正文外旧值等于新值的像素只在驱动输入中改写为白到白（本阅读波形该状态所有相均为 HOLD）。正文外有变化的像素保留真实转移。因此页码/进度/时间/电量仍更新，不冻结整个状态栏。
- 仅一次 `epd_draw_base()`，不为底栏新增第二次波形，不改脉冲时间、灰阶曲线、充电、电量、休眠或网络参数。不新增截图/帧缓存分配；仅增加 16 字节的全局一次性正文矩形。
- 成功后 back buffer 仍拷贝实际 front 像素，不把 HOLD 代用值当成屏幕白色；失败不发布新 baseline，驱动进入 Unknown，下次强制恢复。
- 提示在 commit/cancel/resync/BW/overlay/UI 或显式 profile 切换后失效，不带到弹窗、首页、待机或下本书。
- 手动/周期清理、首帧/失效恢复和精排底色需要的 GC16 全刷保持原样；这些场景状态栏仍可闪。夜间黑白/旧双平面回退路径未新增保持策略，不能宣称所有模式完全不刷状态栏。

## 已验证

- 实际高层 C 提交与实际波形联编，ASan/UBSan：256 转移 × 3 个正文布局；未变界面零驱动、正文黑色补墨保留、页码等改变转移不删、一次提交、全刷不屏蔽、无效布局回退、失败 baseline 不变。
- 实际 renderer/HAL 兼容派发，ASan/UBSan：四种方向逐像素核验正文边界、裁剪、沿用 UI 几何，旧模拟器 HAL 无此接口时安全 no-op。
- 实际驱动回归：一次性提示、显式清理覆盖、取消、区域 UI、含图回退和故障恢复通过。
- 全套 `tests/run_plus_port_tests.sh` 返回 0，保留既有硬件/环境跳过项。最终小改仅补齐 overlay 提示清理/代码格式，实际驱动及两个新回归再次通过。
- 来源记录：2,844 文件、191 适配核验通过，未改应用/SDK 固定基线，未把忽略的 upstream 快照强制加入 Git。
- 编译仅允许干净补丁 IDF `9a97f6c54ec638111ce55cd36581b3c192f15207`，不能用其他 IDF 替换。

## 已构建的体验镜像

最终构建脚本返回 0，前后 IDF/来源检查通过。日志 `.local/reader-status-build-final-20261023.log`，全套回归日志 `.local/reader-status-tests-20261023.log`。

- 文件：`/Users/rick/Documents/ChatGPT/rickyos/output/firmware/plus-1.2.6-reader-status-20261023/RickyOS-Plus-1.2.6-reader-status.bin`。
- 4,966,944 字节；ESP32-S31 / chip ID 32；ESP 应用描述符 1.2.6。
- SHA-256：`8f5e0edd913d568846dd182f35f6c746e3b8c86125d281c91333cc4cbb506eb0`。
- ELF SHA-256：`a65d97389194fb2b1f65e0c3008804b09816830520a1807add16da75d3567b32`。
- 同目录 `candidate.json` 明确记录为本地未提交、未硬件接受、未 OTA 验收、未发布。
- 原整合候选 `80340b6e…128f589` 的已拷贝 BIN 未改动，不将原验收转移到新镜像。

## 真机验收待办

1. 换接 Plus 并核对 S31 身份；不要向当前可能连接的 Pico 写此镜像。
2. 普通纯文字书连续翻至少 20 页：相同标题/电池图标应保持，页码进度应更新；观察正文浓度，不应为保底栏重新出现淡墨。
3. 跨章节、等时间跨分钟、打开/关闭弹窗后继续阅读，检查标题/时间没有冻结、没有残留菜单。
4. 四种方向、不同阅读模式、图片/精排底色页、手动全刷和周期清理、待机唤醒恢复分别检查。
5. 主机通过不等于光学或速度验收；使用同一本书同一页与 `80340b6e…128f589` 原候选对比，记录新 BIN 摘要，不沿用旧验收。

Plus 正式版发布前的 1.2.5 → 新版实际 OTA 门槛仍未完成，不因本实验放宽。

## 后续安装（用户指令「刷」）

已识别 ESP32-S31 / `1c:29:04:d0:2a:50`、16 MiB Flash、干净安全状态、9a97f6c5 bootloader 和有效分区/OTA 选择。只写 app0 `0x10000`，同一 `8f5e0edd…b506eb0` BIN；写入与读回摘要通过，bootloader、分区表、NVS、OTA 选择逐字节未变，SD 未操作；已 watchdog 重启。

24 秒捕获确认应用 1.2.6、ELF a65d97389、IDF 9a97f6c5，进入 InxRecent 并打开 EpubReader，未见 panic/重启循环；保留既有 `GYR SDK IMU not found`，不宣称零错误。

重要：捕获中的实际连续翻页为 `refresh mode=1` 的黑白 DU 路径，没有正文矩形 reader 提示；不能据此声称新的 native-gray chrome 保持路径已经实机触发。该路径的视觉效果仍待用户反馈及开灰阶后的覆盖；黑白路径未增加保持策略，如用户仍反馈闪烁，需针对该路径另查，不能拿主机测试替代。

证据：`.local/reader-status-device-20261023.json`、`.local/reader-status-flash-20261023.log`、`.local/reader-status-boot-20261023.log`。本分支仍未提交/推送/发布，`hardwareAccepted` 和 `ota_tested` 不改为 true。
