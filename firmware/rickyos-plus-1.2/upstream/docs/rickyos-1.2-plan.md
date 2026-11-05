# RickyOS 1.2 规划与视觉规范

状态：2026-10-09 起草，供接手的 agent 直接开工。分支 `rickyos-1.2`，仅在本地，没有推送。

1.2 的目标只有两个：

1. 系统变轻：只保留 RickyOS 用得到的东西。
2. 整体视觉提升一档。

RickyOS 27 是完全重写，1.x 只是过渡版本。所以 1.2 每一项都按"最小改动、最大观感收益"来取舍，不做框架级重构。

---

## 一、当前进度

| 提交 | 内容 | 结果 |
|---|---|---|
| `54ec892b` | 微信读书：书架先响应，封面在静止 1.5 s 后再下载，有输入就中断下载 | 用户已上机验证："退出快多了" |
| `b1b0b479` | RickyOS 成为唯一产品：用 `unifdef` 折叠产品条件分支，删掉执行不到的旧代码 | 151 个文件，净删约 4,500 行 |
| `148b4f02` | 删除 OPDS、SD 插件、XTC 和旧版 `HomeActivity` | 82 个文件，净删约 9,300 行 |

- **固件大小：** 6,021,040 → 5,823,152 字节，省了约 198 KB。分区使用率 88.8%，剩余约 715 KB。
- **测试：** 302 项通过，12 项跳过（原本就跳过）。
- **构建：** `rickyos_readpico` 和 `simulator_rickyos` 都能编译。
- **真机冒烟测试还没做。** 下一次插上设备时先做这件事，清单见第四节。

### 已知问题：C++ 主机测试有 40 项要重新对齐（接手后第一件事）

`test/` 下的 CMake 测试（`ctest`）在折叠前只有 4 项失败（911 项中），折叠后有 44 项失败。多出来的 40 项都不是固件回归：这些测试按原版（非 RickyOS）配置写期望值，而固件一直是带 `RICKYOS_PRODUCT` 等宏编译的，折叠后 RickyOS 的行为变成了无条件的。已经确认的几类：

- `HyphenationEval.*`（法、德、俄等）、`HyphenationFallback.*`：RickyOS 只保留英文断字（`HYPHENATION_ENGLISH_ONLY`）。→ 删掉这些用例，或者只测英文。
- `WeReadStoreTest.*`：导出目录不再是 `/WeRead`，改成 RickyOS 的存储布局。→ 期望值改用 `WeReadStore::kExportDir`。
- `SdCardFont*Test.*`、`SectionMemoryTest.*`、`FontPreloadFlow`：中文版的缓存格式、版本号和字号表。→ 按 RickyOS 的值重新对齐。
- `TimeUtils.LegacyOffsetMigration…`：RickyOS 的时区迁移行为。→ 核对后改期望值。
- `InxNavigation.*`、`InxRecentThumbnails`、`ThemeCheckboxRows`、`CoverGridCompatibility`、`UnifiedKeyboard`、`NativeGrayscale`、`FirmwareUpdateRefresh`：高 DPI 度量、5 个 Tab 等 RickyOS 布局。→ 与 Python 侧一样：只测原版的用例删掉，其余重新对齐。

**做法：**

1. 在 `test/CMakeLists.txt` 里统一加上产品宏，和 Python 的 `run_cpp` 保持一致：`RICKYOS_PRODUCT=1`、`ENABLE_CHINESE_VERSION=1`、`CROSSMUX_UI_PROFILE_HIGH_DPI=1`、`HYPHENATION_ENGLISH_ONLY=1`。
2. 逐个修改上面这些测试的期望值。
3. 对照基线：折叠前的提交是 `54ec892b`。那 4 项原本就失败的测试，在新旧两套里都会出现。

另外，本次已经删掉 `test/opds_filename` 和 `test/plugin_manifest_classification` 两个测试（对应的代码已删除），并给 `test/ble_input/stubs/CrossPointSettings.h` 补上了手势字段。现在全部测试都能编译。

### 折叠时用的宏

宏列表在 `unifdef -m` 里：

```
-DRICKYOS_PRODUCT=1 -DENABLE_CHINESE_VERSION=1 -DCROSSMUX_UI_PROFILE_HIGH_DPI -DHYPHENATION_ENGLISH_ONLY
-DFREEINK_DEVICE_{EEGO_A4,WAVESHARE_EPAPER_397,MURPHY_M4,METALIO_EINK4,PAPERMONO,X4PRO,X4CLASSIC,X4,X3,STICKY,MURPHY,M5PAPER,M5,LILYGO,DELINK,PAPERS3,ONEPAGE,WS397}=0
```

以下几个宏刻意没有折叠，因为模拟器构建没有定义 `FREEINK_DEVICE_READPICO`：

- `FREEINK_DEVICE_READPICO`
- `SIMULATOR`
- `CROSSPOINT_EMULATED`
- `CAP`

注意：zsh 不会对变量做分词，传文件列表时要用 `while read` 循环。

### 测试辅助的变化

`test_reading_ui_regressions.py` 里的 `run_cpp` 现在总会加上 `RICKYOS_PRODUCT=1`、`ENABLE_CHINESE_VERSION=1`、`CROSSMUX_UI_PROFILE_HIGH_DPI=1`。

---

## 二、精简：剩余工作（优先级低）

用户的原话是："OPDS、插件、XTC 去掉，其他保留"。这部分已经完成。剩下的都是工程整理，不影响固件大小，可以放到视觉工作之后再做：

1. **精简 `platformio.ini` 的构建配置。** 目前约 42 个 env，只保留以下几个，删掉其余 39 个：
   - `rickyos_readpico`
   - `simulator_rickyos`
   - `rickyos_native_gray`（实验用）
   - `[rickyos]` 和它依赖的基础段

   **注意：** `[env:readpico]` 和 `[env:simulator_readpico]` 可能被 `extends` 引用，删之前先 `grep extends`。
2. **删除其他板子的 HAL 代码和对应测试。** 每删一批都要跑全量测试，再编译设备版和模拟器版。
3. **清理未使用的界面文案。** 共 144 条，约 4.5 KB，收益小，可以不做。
4. **以下两项会改变功能，用户还没批准，动手前先问：**
   - 删掉英文网页，约 75 KB；
   - 删掉 Ubuntu 字体，约 100 KB。

---

## 三、视觉提升（1.2 的主体）

**完整的设计规范在 [`docs/design-system.md`](design-system.md)，实现时以它为准。** 本节只讲工程安排和来龙去脉。

### 3.0 设计包已到（2026-10-09），以它为准

用户交付了完整设计包，原件在 `~/Downloads/RickyOS_视觉方案_交付/`，备份在 `/Users/rick/rickyos/design/1.2/`：

- `RickyOS_视觉方案.md` / `.pdf`：规范正文。
- `方案.md`：首页 peek2 的方向说明。
- `三项设计决定.md`：已锁定的三项决定，不是候选项。
- `RickyDesignTokens.h`：Token 头文件。
- 各屏 PNG：`01_home` 到 `05_empty`，加 `board`；每张都有对应的 `*_1bit.png`，表示交互帧。

**本节和下面 3.2 到 3.7 有冲突时，以本节和设计包为准。**

**Token 头文件**已放到 `src/components/RickyDesignTokens.h`，命名空间 `ricky::ds`。用 C++20 语法检查通过，所有 `static_assert` 都成立。它还没有接入任何页面，用来取代原先的 `RickyTokens.h`。相对设计包原件改了两处：

1. `component::kAppTile` 的圆角从 24 改为 `radius::kControl`（12）。头文件原来写的是 24，与《三项设计决定》第 3 条冲突，以锁定决定为准。
2. 一处过时的行号注释。

**头文件比上面 3.2 多出来的内容：**

- 完整的间距阶梯 `space::k1` 到 `k16`。
- 布局尺寸：状态栏 48、页眉 96、子页眉 88、Tab 栏 112、列表行 96、按钮 88、最小触控区 88。
- 组件配方 `component::*`：卡片、弹窗、按钮、芯片、选中行、应用瓷砖、封面托盘、进度条。
- 图标尺寸与线宽 `icon::strokeFor`：24→2、32→3、40→3、56→4。
- 生成封面的模板选择 `coverTemplateFor`：6 种模板，取 FNV-1a 哈希的高 16 位。
- `kAllowSyntheticBold = false`：禁止假粗。
- `refresh::kDitherInChrome = false`：界面外框禁止抖动。

**灰阶在交互帧里的去向，以头文件为准：**

- `gray::oneBitFallback` 规定：ink、ink-2、mute、line 去墨；只有 tint 去纸。
- 这和 3.2 第 4 条原来的写法（line 去白、mute 视情况）不同，3.2 那条作废。
- 设计包里的 `*_1bit.png` 也没有完全照这条规则：首页和空首页底部的座右铭、空首页的虚线"放书"框，在 1-bit 图里消失了。**实现时按头文件来**，文字不能在交互帧里消失。

**字体：头文件里的 `type::Style` 是最终方案。** 按 150 DPI 换算，`px = pt × 150 / 72`。

| 样式 | 像素 | 字重 | fontconvert 磅值 | 字符覆盖 | 现状 |
|---|---|---|---|---|---|
| display | 48 | Bold | 23 | 只含数字和日期 | 没有，需要新做 |
| title | 33 | Bold | 16 | 界面文案 | 没有，需要新做 |
| headline | 29 | Medium | 14 | 界面文案 | 没有，需要新做 |
| body | 25 | Regular | 12 | 全集（6474 字） | 已有：`notosans_cjk_12_rickyos` |
| caption | 21 | Regular | 10 | 界面文案 | 已有：`notosans_cjk_10`，常用字全集 |

- 界面文案的字符集：从 `lib/I18n/translations/chinese.yaml` 统计出 842 个汉字，再加上 ASCII 和标点。
- 粗略估算：16pt Bold 和 14pt Medium 两档合计大约 300 到 450 KB，未压缩，2-bit。现在剩余约 715 KB，放得下，但余量不多。**生成之后要实测大小。**
- 去掉其他界面语言**省不出空间**：`[rickyos_flash_trim] custom_i18n_builtin_langs = zh_cn` 早就只把中文和英文编进了固件，语言选择里也只有这两项。其他语言只剩翻译源文件、`Language` 枚举里的名字，以及 9 种外文键盘（`KeyboardLayoutSet.h`，设置里的"键盘布局"页）。要清理的话：删掉 33 个 YAML，键盘表只留 `QwertyEn`，删掉 `KeyboardLayoutsActivity` 和它在设置里的入口，再改 `test_rickyos_flash_trim.py` 和 `test/inx_navigation` 里相关的用例。这一步对 flash 没有收益，做之前跟用户确认。

**用户 10-09 的答复**（全部以 `docs/design-system.md` 为准）：

1. **封面书名：** 按建议做。字都在 Medium 子集里就用 Medium，缺字就整串退回 Regular。不做大的 Medium 全集。
2. **座右铭：** 就是已有的"给自己的话 / 首页短句"（`STR_RICKY_HOME_PHRASE`），在用户资料里修改。不用新做。
3. **统计卡里的"目标"：** 用户觉得可以再改一改，具体改法还没定。数据已经有了：`todayReadingMs`、`currentStreakDays`、`goalReadingMs`；每日目标由 `dailyGoalTarget` 设置。先按设计稿做，再请用户看。
4. **1-bit 规则：** 同意按头文件来，文字在交互帧里变墨，不能消失。
5. **去掉其他语言：** 用户同意去掉，但见上面字体段落里的说明。固件本来就只编进了中英文，所以去掉省不出空间，剩下的只是源文件和外文键盘。这一点已经告诉用户。

**和固件现状的对照：**

| 设计稿内容 | 固件现状 | 结论 |
|---|---|---|
| 设置页 6 张卡：显示与待机 / 阅读 / 网络与同步 / 字体管理 / 书库与文件 / 系统 | 已有，`STR_RICKY_CAT_*` 六类 | 一致 |
| 书库筛选：全部 / 在读 / 未读 | 已有文案 `STR_RICKY_BOOKS_READING` / `UNREAD` | 一致 |
| 问候语：昵称 + "今天也读几页。" | 已有 `STR_RICKY_HOME_GREETING`、昵称编辑 | 一致 |
| 空首页的传书方式：浏览器、WebDAV / Calibre、热点、TF 卡 `/books`、微信读书 | 都已支持（`WebDAVHandler`、Calibre 无线、热点、`RickyStorageLayout::BOOKS`、微信读书） | 一致 |
| Tab 上方的座右铭"慢一点，读几页。" | 没有 | **新功能。** 首发可以先用固定文案；做成用户可编辑的话，可以复用修改昵称的输入框 |
| 今日阅读、目标、连续阅读天数的卡片 | 有 `ReadingStatsStore` | 先核对它有没有"每日目标"和"连续天数"；没有的话按示例数据处理，**问用户** |
| 设置页"关于本机"显示 `RickyOS 27.10` | — | 这只是示例数据，固件里显示真实版本号（1.2.x） |
| 首页小狗从卡片后探头、真 Logo | 已有小狗和 Logo 素材 | 需要按 peek2 的位置重新出图 |

### 3.1 依据

- **视觉方案：** 用户提供的 `~/Downloads/RickyOS_视觉方案.pdf`。
  - 其中引用的 `RickyDesignTokens.h` 和各屏 PNG（`01_home.png` 到 `05_empty.png`、`board.png`、`*_1bit.png`）不在这台机器上，**需要向用户要**。
- **另一位老师的评审：** 已对照代码逐条确认，问题都属实：
  1. 界面只用黑白两色，圆角、斜线、文字边缘有锯齿。
  2. 中文粗体是"假粗"（同一字形画两遍）。
  3. "灰色"是抖动出来的网点。
  4. 层级平，所有元素差不多大。
  5. 圆角、选中样式、图标风格都不统一。
  6. 封面是占位图。
  7. 边距不均。
- **老师的五条建议，即 1.2 的工作项：**
  1. 打开已有的 1.5 s 灰度沉淀（3.3）。
  2. 内置真正的中粗、粗体中文子集字库（3.4）。
  3. 设计 Token：一种选中样式、两种线宽、两种圆角、8 px 网格（3.2）。
  4. 统一图标（3.5）。
  5. 生成带灰度纹理的封面（3.6）。
- **已有的设计资料：** `/Users/rick/rickyos/design/README.md`（1.1.1 时期的三项效果图：待机时间、应用图标边框、界面锯齿）。

### 3.2 设计 Token

取值来自视觉方案 PDF 第 3 节，不要另造新值。

| Token | 值 | 说明 |
|---|---|---|
| 面板 | 684×1216，约 297 ppi | 竖屏 |
| 字号 | display 48 / title 33 / headline 29 / body 25 / caption 21 | 单位 px，指字身高度 |
| 页边距 / 栏距 | 40 / 20 | 内容宽 604 = 684 − 2×40 |
| 网格 | 8 px | 间距只用 8 的倍数，例外只有 20 和 4 |
| 卡片圆角 | 24 | |
| 控件圆角 | 12 | 列表行、按钮、输入框、弹窗 |
| 芯片 / Tab | 胶囊（圆角 = 高 / 2） | Tab 胶囊 88×48，墨色 |
| 线 | 细线 2 px，强调线 4 px | 只允许这两种 |
| 灰阶 | ink=0，ink-2=4，mute=8，line=11，tint=13，paper=15 | 0 为黑，15 为白；全局只用这六个灰阶 |
| 选中 | 反白 | 承载面变成墨色，内容变成纸色。整个系统只有这一种选中方式 |

**落地做法：**

1. **Token 头文件用设计包里的 `src/components/RickyDesignTokens.h`**（`ricky::ds` 命名空间，见 3.0）。`InxTheme` 和各页面从这里取值。
2. **用 Token 改写 `InxTheme.h` 里 `makeValues()` 的度量。** 例如：
   - `listRowRadius` 6 → 12
   - `popupCornerRadius` 6 → 12
   - `headerHeight`、`menuRowHeight` 对齐到 8 的倍数
3. **全局搜索硬编码的数字**：圆角、线宽、边距，在 `src/activities/**` 和 `src/components/**` 里逐个换成 Token。
   - 搜索关键词：`drawRoundedRect(`、`fillRoundedRect(`、`drawRect(`、`fillRectDither(`。
   - 每改一页，在模拟器里截图对比。
4. **灰阶在两种刷新下的表现：**
   - 黑白快刷时（交互帧）按阈值处理：0、4 当黑，11、13、15 当白，8 按所在位置决定。
   - 沉淀为 16 级灰度后显示真实灰阶。PDF 里的 `*_1bit.png` 就是交互帧的样子，可以拿来对照。
   - 因此 `tint`（13）做底色，在快刷帧里是白色，不会出网点。**不要再用 `fillRectDither` 表现灰色。**
5. **字号映射：** 目前界面中文字库只有 8、10、12 三档（`CJK_UI_*_FONT_ID`，`fontconvert` 2-bit）。
   - 先把这三档的实际像素高度量出来，再决定 display / title 是新增字号，还是只在少数地方（首页问候、页面大标题）用阅读字库代替。
   - 新增字号会占 flash，加之前先估算大小（见 3.4）。

### 3.3 灰度沉淀：视觉提升最大的一步

**机制已经写好，只是关着。** 代码在 `src/activities/ActivityManager.cpp:84-89`：

- `kGraySettleDelayMs = 1500`
- `graySettleAllowed()` 目前返回 `false`

开启后的行为：操作时仍然是黑白快刷，保持跟手；输入静止 1.5 s 后，用 16 级灰度把当前画面重画一次，走无闪烁波形（`settleToGray()`）。界面文字和圆角就会和阅读页一样平滑。只有声明了 `wantsGraySettle()` 的页面才会这样做。

**关掉的原因：** 10-06 真机上偶尔出现"两屏叠在一起"，要全刷才能恢复，当时没找到原因。

**假设（未验证）：** 10-07 修过 epdiy 的扫描行队列（scan-line queue），这个问题可能已经一并解决。

**接手步骤：**

1. 把 `graySettleAllowed()` 改成返回 `true`，编译 `rickyos_readpico`，刷机。
2. 真机压力测试，每项至少重复 50 次，观察有没有叠影：
   - 各页面快速来回切换；
   - 沉淀进行中立刻按键或滑动（沉淀被打断）；
   - 打开和关闭控制中心；
   - 进出阅读页；
   - 待机与唤醒。
3. 串口用 `cap2.py` 抓日志，端口 `/dev/cu.usbmodem101`，留意 `Idle render` 相关日志。
4. **如果没有复现：** 保持开启，进入 3.2 的 Token 工作。灰阶 Token 要靠沉淀才能真正显示出来。
5. **如果复现了：** 不要合并 native-gray 分支来解决。
   - 那条路每帧都做 GL16 加电源轨断电，没有快速反馈，切页和每 8 帧还要全刷，延迟明显变差。
   - 先查沉淀和下一次黑白快刷之间，面板状态与驱动缓冲是否同步（`isGrayscale16Active`、被打断后的恢复路径）。
   - 实在不行的保底：沉淀被打断时，下一帧做一次局部 GC16。

### 3.4 中文粗体子集字库

**现状：** 中文没有粗体字库，"粗体"是同一字形画两遍。

**做法：**

- 只把界面实际用到的字做成 Noto Sans SC Medium（或 Bold）子集，阅读正文不需要。
- 字符来源：
  - i18n 中文文案（`lib/I18n` 下中文 YAML 里的全部字符）；
  - 数字、标点；
  - 首页问候与座右铭的固定文案。
- 生成命令参照现有头文件里记录的写法，换成粗体字体文件，再加上 `--chars-file` 之类的子集参数：

  ```
  fontconvert.py notosans_cjk_12_rickyos 12 instanced_fonts/NotoSansSC/NotoSansSC-Regular.rickyos.otf --2bit ...
  ```

  `fontconvert.py` 支持什么参数，请先看脚本本身。
- **大小估算：** 一两千个字、2-bit，单档约 60 到 120 KB。现在剩余 715 KB，可以放 1 到 2 档。
- **接入：**
  - 在 `main.cpp` 里用 `EpdFontFamily` 的 bold 槽位挂上新字库，和 `cjk12FontFamily` 放在一起，这样现有的 bold 绘制路径会自动用上真粗体。
  - 去掉假粗的双绘逻辑：搜索在 x+1 处再画一次文字的代码。
- **用在哪里：** 页面标题、继续阅读的书名、选中项、Tab 胶囊。正文和说明文字保持 Regular。

### 3.5 图标统一

- 生成脚本在 `scripts/build_rickyos_*_icons.cjs`。
- 依赖 sharp 0.35.4。上个会话装在临时目录，换机器或换会话需要重装：

  ```
  npm i sharp@0.35.4
  NODE_PATH=... node scripts/...
  ```
- **规则：**
  - 同一种线宽：在 24 px 网格上 2 px，大图标按比例放大到 4 px。
  - 圆角端点。
  - 选中时整体反白，和 3.2 的选中规则一致。
- 应用图标边框**已锁定**：2 px 墨色描边，圆角 12，纸色底；图标 40 px 居中，使用 Lucide 描边风格；选中时整块反白。不用圆形外框，不用旧的 28 圆角，不用加粗描边表示焦点。见《三项设计决定》第 3 条。
- 控制中心图标在 1.1.3 刚改过：待机用 moon，夜间用 contrast，保持不变。

### 3.6 生成封面

- **适用范围：** 没有封面的书（TXT、无封面的 EPUB），用生成封面替换现在的占位图。
- **样式：**
  - 底色用 tint 或 line 灰阶，叠一层细纹理（细斜线或网格，2 px 线、8 px 间距）；
  - 书名用 3.4 的粗体，作者用 caption 字号；
  - 圆角 24（大卡片）或 12（小封面）。
- **按书名生成：** 用书名哈希在 3 到 4 种纹理里挑一种，同一本书每次的样子都一样。
- 书名字体：见 3.0 的冲突说明。宽度 ≥ 160 且书名 ≤ 5 个字时竖排。得意黑只作可选项，首发不需要。

### 3.7 各屏要点（依据视觉方案第 4 节）

- **首页：**
  - 已定方案 peek2，原样保留。
  - 问候带昵称，例如"Ricky，今天也读几页。"。
  - "继续阅读"是最大的主卡，小狗从卡片后面探头。小狗不拿书，也不做"正在读书的狗"。
  - "最近翻过"是一行较小的封面。
  - 底部是 Tab，Tab 上方一行座右铭占位："慢一点，读几页。"
- **书库：** 大标题加筛选芯片（当前芯片反白），三栏封面栅格，焦点封面用墨色托盘承载。
- **阅读：** 顶栏安静，页脚显示进度。默认阅读页不出现小狗；"小狗伴读"阅读模式是读者自选的例外（见设计规范 13.3）。
- **设置：** 资料卡用真 Logo，下面是 2×3 的设置卡，选中时整卡反白，再往下是列表行加箭头。
- **空首页：** 小狗仍按探头规则引导。"开始传书"入口只列固件已经支持的方式。

**用户的长期偏好：**

- 照着宣传图做，但去掉装饰性的说明文字。
- 没有中文输入法，就不做搜索。

**说明文字已锁定**（《三项设计决定》第 2 条）：

- 保留：分区标题、状态词、单位和简短说明、空状态的标题加不超过 2 行正文。
- 去掉：重复解释控件的句子、装饰性的长说明、和问候或座右铭抢视线的旁白。

### 3.8 建议顺序

每一步都要模拟器截图、真机看效果，再进入下一步：

1. **灰度沉淀开关试验（3.3）。** 只改一行，加上真机测试，半天。结论决定后面灰阶 Token 能用到什么程度。
2. **Token 文件，加上 `InxTheme` 度量改写（3.2）。** 统一圆角、线宽、边距、选中样式。
3. **粗体子集（3.4）。**
4. **按屏落地（3.7）：** 首页 → 书库 → 设置 → 空首页 → 阅读页边角。
5. **图标（3.5）和生成封面（3.6）。**

### 3.9 不要做的事

- 不要合并 `rickyos-1.2-visual` 或 `codex/rickyos-native-gray`。
  - 前者基于 1.1.3 RC 加 native-gray，已经过时。
  - 后者每帧走 GL16，延迟明显变差。
  - 两者的诊断结论都写在 3.3 里了。
- 不要动阅读排版引擎和 crossmux 框架。那是 RickyOS 27 的范围。

---

## 四、真机冒烟测试清单

`rickyos-1.2` 首次刷机后按下面的清单检查：

- 开机进首页；书库、应用、存储、设置各页都能打开。
- 打开一本 EPUB 和一本 TXT：翻页、目录、书签都正常。
- 网页传书：页面能打开，能上传一本书；设置里的 Wi-Fi 管理正常。
- 微信读书、屏保、待机唤醒都正常。
- "应用"里没有 OPDS，设置里没有插件入口。
- `.xtc` 文件在存储里显示为普通文件，不会被当成书打开。

---

## 五、构建与刷机

```
pio run -e rickyos_readpico                  # 设备版
pio run -e simulator_rickyos                 # 模拟器
python3 -m unittest discover -s scripts/tests -p 'test_*.py'   # Python 测试
~/.platformio/packages/tool-cmake/bin/cmake -S test -B <build> && cmake --build <build> -j8 && ctest -j8   # C++ 测试
pio run -e rickyos_readpico -t upload --upload-port /dev/cu.usbmodem101
```

**模拟器桥接：**

- 端口 8765。
- 按键事件需要 `'duration':120`，键名用大写。

**刷机前提：** 只在用户要求，或者用户说已经插上设备时才刷。
