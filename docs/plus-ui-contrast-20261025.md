# Plus 五个主 Tab 对比度与电池对齐体验改动

日期：2026-10-25。分支：`codex/plus-ui-contrast-20261025`。
基线：`e26f4277be15525fcf6d7c47f1f735ed1e28512a`。本次尚未提交或推送；用户后续授权后已刷入 Plus，等待光学验收。

## 用户问题与代码证据

- 用户照片 `IMG_2155.HEIC` 中，首页右上电池图标低于百分比文字；用户明确反馈五个主 Tab 都显得灰蒙，而非仅首页。
- 恢复后的 Plus 源码 `upstream/src/components/themes/BaseTheme.cpp` 的 `drawBatteryLeft/Right` 原先统一使用高分屏 `+8` 偏移，而主 Tab 使用 caption 10 数字字体、其他页使用 Noto Sans 12。实际字体墨迹区间分别为 `[6,22)` 和 `[8,28)`；20 px 图标应分别使用 `+4`、`+8`。
- `components/rickyos_platform/CMakeLists.txt:7` 明确选择灰阶预设 2；因此更改默认预设不会改变 Plus。本次修改其设备专属分支。
- `upstream/src/components/RickyDesignTokens.h:53` 起为共用界面灰阶，`upstream/lib/GfxRenderer/GfxRenderer.cpp:22` 起为命名灰色填充。
- `components/metalio_board/display/ricky_native_turn.inc:39` 的 UI 动效收尾原先让黑色与其他灰阶同一时刻完成；阅读正文另有独立处理。

## 实现

1. 电池按当前数字字体的实际墨迹中心对齐。使用固定的数字与百分号样本，避免电量位数变化时图标跳动；未注册字体保留行高回退。不新增堆内存。
2. Plus 预设 2 的次级文字、弱化文字、线条分别由 `2/5/7` 调为 `1/4/5`，浅底仍为 12；命名灰色填充由 `7/4` 调为 `5/3`。0 为黑、15 为白。其他设备和其他灰阶预设保持原值。这些是共用 UI 样式，涵盖五个主 Tab；不对图片像素或文字抗锯齿覆盖率做全局变换。
3. 37 相 UI 动效中，目标纯黑的有效脉冲提前 4 相结束，留出 7 相 HOLD；其他灰阶仍留 3 相 HOLD。有效脉冲的个数、顺序及内部 HOLD 不变，总扫描数不变。阅读波形、供电参数和面板时间单位未改。此项需要真机判断墨色、灰边与残影，主机测试不能证明光学改善。
4. 新增源文件已登记在 `port-changes.json`，并记录于 `upstream-plus.patch`；恢复检查为 2844 个固定源文件、221 项源适配。

## 验证

已通过：

- `tests/test_plus_battery_alignment.py`：读取实际字体数据，编译实际对齐函数，检查中心误差不超过半像素、位数变化稳定及回退行为。
- `tests/test_plus_ui_contrast.py`：实际共用样式与填充常量检查，验证 Plus 加深、其他设备和黑白端点保持不变。
- `tests/test_native_page_turn.py`：5 项通过；全部 256 个灰阶转换保持脉冲顺序、数量及内部 HOLD，黑色末相为 29，其他灰阶为 33；覆盖阅读动效、旋转、内存不足回退。
- `tests/test_plus_regional_gray.py`：实际驱动区域提交、图片页极性、中断叠层及恢复，ASan/UBSan 通过。
- `tests/test_plus_reader_waveform.py`：默认 48 相正文与可选 37 相实验的全部 256 个转换、GC/GL 和幂等性通过。
- Plus 完整构建通过，补丁 IDF `9a97f6c54ec638111ce55cd36581b3c192f15207` 校验通过；字体资源已重新生成并验证。

本地产物：`/Users/rick/Documents/ChatGPT/rickyos/output/research/plus-ui-contrast-20261025/RickyOS-Plus-1.2.7-uicontrast.bin`。
大小：5,020,976 字节。
SHA-256：`bcb41d5c335c6521d9fd72dc1b06b7a0ce221f622fd46703cbb118d7e7816713`。
构建记录：同目录 `build-record.json`。

## 真机验证与合入注意

- 用户随后明确要求刷入。已在 Plus 写入并校验，启动日志确认 `1.2.7-uicontrast`、ELF `29f06ad07…`，进入 `InxRecent`，30 秒采集中无崩溃标记；光学效果仍等待用户验收。
- 刷入前实际固件为另一任务的 `1.2.7-btdiag1`。其完整 app0 已在私有目录保存并逐块校验，可恢复。本包基于正式 1.2.7，不包含该任务新增蓝牙功能，也未合入单独 WPA3 分支；刷入前已向用户说明切换体验包。仅写入 app0，校验确认其余 Flash 区域未变化，安装器未访问 SD。
- 安装记录：`/Users/rick/Documents/ChatGPT/rickyos/.local/device-installations/plus-ui-contrast-20261025.r_ka4w1c`；恢复包和启动完整日志仅保存在本机。
- 合入当前实际测试分支后，应先关闭前光，在五个主 Tab 来回切换，观察初始及多次操作后的黑色、灰色、边框；核对百分比与电池图标中心。再打开纯文字书和含图页，确认阅读与图片显示没有回归。
- 若黑色仍逐次变浅，应进一步记录动效开关、相同页面重复操作及全刷前后差异，区分 UI 调色与未变化像素保持策略；不要通过抬高整幅图片黑位掩盖问题。

## 第二档：用户要求进一步加强

第一档刷入后用户反馈“感觉好一些”，要求进一步加强。第二档仅调整 Plus 共用 UI 调色：次级文字 `1 → 0`，弱化说明 `4 → 3`，线条 `5 → 4`，命名灰色填充 `5/3 → 4/2`。浅底保持 12，主黑、纸白、字形抗锯齿覆盖率及图片像素不变；电池对齐和第一档动效收尾时序保持不变，便于对比。次级文字与主文字墨色合并为纯黑，层级由字号、位置与弱化说明保留。

已通过实际 Plus 样式测试，同时检查其他设备预设不受影响；源快照重新登记为 221 项适配。版本 `1.2.7-uicontrast2`，构建及真机安装记录另存本地。

第二档构建通过：补丁 IDF 与 2844/221 源适配检查通过。产物 5,020,880 字节，SHA-256 `ce80f9f3197c27c94e32be4f8c2fb8c3315ecb0db3c9388d208d7eef3b534bb3`，ELF `f678f61bf6175be457e8cd4cb668afa2bd48d48482057d8e78135ada47a7c5f6`。本地目录：`/Users/rick/Documents/ChatGPT/rickyos/output/research/plus-ui-contrast2-20261025`。

第二档已刷入、回读校验通过，启动日志确认版本与 ELF，进入 `InxRecent`，30 秒启动采集中未出现崩溃标记。用户尚未反馈第二档光学效果。安装及第一档恢复记录：`/Users/rick/Documents/ChatGPT/rickyos/.local/device-installations/plus-ui-contrast2-20261025.8ainoqbw`。

## 第三档：仅将灰色线条调浅一级

用户反馈第二档其他部分已更好，仅希望 UI 灰色线条略浅。依据 `RickyDesignTokens.h:55` 的共用 `kLine`，以及 `RickyDesignUi.h:75/84/151/167` 的卡片边框、分隔线和封面边线调用，本轮只把 Plus `kLine` 从 4 改为 5。文字 `0/3`、命名填充 `4/2`、浅底 12、电池布局及动效时序均保持第二档；其他设备预设保持原值。实际样式检查通过，源快照重新登记。体验版本：`1.2.7-uicontrast3`。

第三档完整构建通过，补丁 IDF 与源适配检查通过。固件大小 5,020,880 字节；SHA-256 `4a89ff5a9becf817578b337365765fc687512c00ed7b7a7d1daee0779cbf7dd5`。已刷入并回读校验通过，启动进入 `InxRecent`，30 秒采集中无崩溃标记，等待用户对线条深浅反馈。恢复与安装记录：`/Users/rick/Documents/ChatGPT/rickyos/.local/device-installations/plus-ui-contrast3-20261025.rk67dors`。

## 第四轮：日期等纯黑文字的实际墨色

用户反馈日期和其他黑色文字仍不够黑。先查 UI 颜色链路：`RickyHomeUi.cpp:292` 的日期调用 `design::text`，默认 `kInk=0`；`RickyUiTarget.h:74` 的 DarkGray 映射也因当前 `kInk2=0` 落在纯黑。因此本轮不是再调灰色线条、字重或字体覆盖率。

驱动证据：`my_waveform.c` 中 `MY_TRIM_SAT_CUT=5`，UI 从完整 GL16 裁掉黑色饱和余量；实际波形报告显示白到黑由完整表的 18 个 darken 动作变为 13。`MetalioPlusDriver.cpp:341` 的普通 UI 更新使用 `MODE_GL16_INV`。新函数 `ui_restore_black_margin` 只给该 UI 表的 15 个“非黑到纯黑”转换补回 2 个原有余量中的 darken 动作，写入现有尾部 HOLD，仍留至少 3 相稳定时间。37 相总扫描长度不变，未变像素保持不驱动，灰阶/白色转换不变。

必须在阅读与纸色表复制完成之后再调整 UI 表，避免 `s_turn_data` 拷贝时把加深也带入正文。实测字节级对照确认其他六个模式完全不变。无额外表、帧缓冲或堆分配；未改电压、VCOM、相时间。此为有限的光学体验调整，不能把软件脉冲检查当作实机黑度或长期残影结论。

验证通过：新 `test_plus_ui_black_margin.py` 对照开关 0/2，检查全部 7 个实际波形、UI 的全部 256 个转换、模式隔离及重复初始化；ASan/UBSan 通过。`test_plus_reader_waveform.py` 验证独立阅读期望模型及 UI 限定差异；`test_native_page_turn.py` 5 项全部通过，动效保持有效脉冲顺序、数量与内部 HOLD。上一轮线条灰阶 5 及文字样式保持不变。体验版本 `1.2.7-uiink4`。

第四轮完整构建通过：5,021,024 字节，SHA-256 `02507ce4c9ca4aa0625e11263ce5b31223fc379a02c03935eac759c6978e5fb7`，ELF `7e804e521a973b3a00c8fb6c59c2a0c2da81764c4b276295e1307cff59b37f7c`。安装前电脑没有枚举到已核对的 Plus USB 串口，身份保护中止于串口检查，未执行任何写入。已请求重新连接，待上机对比。

用户重新连接 Plus 后，第四轮 `1.2.7-uiink4` 已成功刷入并回读校验；app0 外 Flash 摘要保持不变，启动进入 `InxRecent`，版本与 ELF 相符，30 秒日志无崩溃标记。等待用户对日期等纯黑文字墨色反馈。安装和恢复记录：`/Users/rick/Documents/ChatGPT/rickyos/.local/device-installations/plus-ui-ink4-20261025.id3ybftt`。

## 第五轮：恢复首页、书库与底部 Tab 的文字层级

用户反馈首页和书库的文字过多纯黑，底部 Tab 选中态也缺少深浅区别。核对规范与实际调用：

- `docs/design-system/mockup-src/gen.py:66/74/76/83/93/104/108` 明确把星期、作者、统计说明、元信息和书库数量归为 `ink2`；日期、标题、书名使用主墨色。
- `RickyHomeUi.cpp:292` 起，主日期用 `design::text`，说明类文字用 `secondaryText`；书库 `LibraryListActivity.cpp:1518/1567/1676/1741` 也保留了次级语义，页面并未丢失角色标记。
- 先前本分支为进一步加深，把 Plus `kInk2` 改成 0，导致所有 DarkGray/secondaryText 与主黑重合。这是本次层级消失的直接代码原因，不能仅靠调面板黑色驱动解决。
- `InxTheme.cpp:521` 的主 Tab 已有“选中 ink、未选中 ink2”，因为两者都为 0 而失去差别。`2e4b1123` 的新版精修明确取消胶囊，改成短墨色标记；本轮保留该新版，不恢复旧效果图的胶囊。

修正：Plus `kInk2` 恢复为预设 2 原有的深灰 2，其他保持第四轮：主黑 0、弱化 3、线 5、浅底 12、纸白 15；命名填充 4/2、真实字体、黑色写入与动效时序均不变。不新增假粗体和额外刷新。规范增加 Plus 本地体验值与最新 Tab 例外，避免后续以旧图覆盖新版。

验证：实际样式测试加入 `ink < ink2 < mute`，禁止再次把角色合并。新 `test_plus_ui_text_roles.py` 执行生产 `RickyDesignUi::text/secondaryText`、`RickyUiTarget::text` 与完整 `drawMainTabBar`，验证首页与书库两种绘制入口、主/次/禁用/反白文字、作用域恢复；遍历五个 Tab、四种宽度、上下位置和黑白／灰阶帧，确认仅选中项为主黑、其余四项为深灰，标记位于对应触控单元。ASan/UBSan 与样式检查通过。体验版本 `1.2.7-uiroles5`。

第五轮完整构建与 IDF/源快照检查通过：5,021,072 字节，SHA-256 `197d44f90df47d8d9ff4df9c6d40a035405055d9738fb84bba6a7afc234532e6`，ELF `4e2a81aeda4f947b23a141ed3c8a45f7226aba8bed70c056b0e3f97f0a19bb01`。已刷入回读校验，app0 外摘要保持不变；日志版本与 ELF 符合，进入首页，采集中还观察到书库、应用、存储和返回首页，无崩溃标记。活动进入与软件检查不替代灰阶视觉验收，仍等待用户反馈。安装恢复记录：`/Users/rick/Documents/ChatGPT/rickyos/.local/device-installations/plus-ui-roles5-20261025.palczae5`。

## 第六轮：系统页的贯穿竖轨与重复箭头

用户照片 `IMG_2157.HEIC` 显示系统页右侧几乎贯穿全屏的竖线，多个条目重复显示箭头。代码依据：`RickyRowList.h` 的 `draw` 末尾在溢出时绘制完整 `body.height` 的灰色轨道和按可见比例缩放的黑色滑块，而且没有遵守 `props.scrollIndicator`；`SettingsActivity.cpp:1319` 根据 `opensPage` 对大多数系统动作自动加箭头。是产品内部列表的收敛未完成，不是本次灰阶面板驱动生成的线，也不是设备损坏。

修正：共享列表取消长轨道，超出一屏时只画独立灰色滑块，长度最多 64 px；一屏放下或关闭提示时不画。滚动位置仍映射首尾范围。系统子页的逐行跳转箭头取消，其他类别保留原有披露规则，标签、说明、设置值、开关、点击 action/valueId 和危险操作确认流程不变；原有 list nav 仍提供滚动和返回定位。无新内存分配。源适配记录增加至 222 项。

新 `test_plus_quiet_settings.py` 执行实际行绘制模板、滚动提示几何和系统箭头策略：覆盖 0/1/8/9/12/24 项、短/长视口、逐个 topIndex、单行与带说明的行、开关、提示开关、首尾定位；检查没有长轨道、超长滑块或边界越界，并核对行 action/valueId、行间距与 list nav；系统箭头关闭且其他分类行为保留。ASan/UBSan 通过。规范已记录该用户反馈后的列表样式。体验版本 `1.2.7-uisystem6`。

第六轮完整构建、补丁 IDF 与 2844/222 源适配检查通过。5,021,152 字节；SHA-256 `b77657bbaf9b86abbf7fff5ce955ac46074dc9f76f5228fdc24b952d05e2937d`，ELF `aadec00f300e5200f39ee66d2b6146a87969d02f010613f2e43157e8a4da36da`。已刷入、回读校验，app0 外 Flash 摘要保持不变；启动进入首页，版本及 ELF 相符，30 秒采集中无崩溃标记，系统页视觉仍待用户检查。安装恢复记录：`/Users/rick/Documents/ChatGPT/rickyos/.local/device-installations/plus-ui-system6-20261025.95xx9sc4`。
