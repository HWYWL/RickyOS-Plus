# Read Pico 1.2：区域灰阶刷新第一轮

2026-10-11；本地 `codex/rickyos-1.2-design`，基于 `24c05ff7`。
对应 [WhiteOS / kiikoread 研究](whiteos-kiikoread-research-2026-10-11.md)。
这是体验候选，不是正式发布；本轮没有刷机、提交或推送。

## 已实现及边界

- 保留直接 16 级灰阶、现有字体和设计，不恢复 1.5 秒后转灰。
- `ActivityManager::renderDirectGray()` 不再按每次重绘累计整屏 GC16。
  页面仍完整绘制、发布完整 FUI 交互表；驱动根据目标与最后成功的 **4-bit** 底图计算真正变化。
  相同画面不通电、不扫描、不累计清理次数。
- 内容区、≤64×64 的小变化、导航区分别累计清理次数。
  内容变化 8 次、小变化／导航变化 32 次，清理对应区域。
  清理范围覆盖自上次清理以来积累的变化，不只清理最后一次小包围盒。
  这些是待真机验收的实验参数，不是通用残影保证。
- 普通变化合并为一次 held-diagonal GL16 提交。只有区域清理与另一区变化同时发生时，
  最多分为两次区域提交；不是每个控件都额外多刷一次。
- 导航区使用现有布局的几何，由 renderer 统一旋转到面板坐标。
  驱动的差分列扩展至 32 px，导航区也按这个边界扩展，避免内容清理越界驱动底栏。
  包含布局留出的屏幕边缘／按键提示留白，不改变触摸热区。
- 手动全刷、未知底图和失败恢复仍走整屏清理。
  区域账本在全部提交成功后才前进；任一阶段失败，下一帧重建整屏基准。
  取消也清除一次性刷新配置，不能让 UI 配置污染下一张阅读页。
- 增加重绘请求代次：即便主循环已消费 `requestedUpdate`，绘制中被新请求取代的帧仍会取消。
  只有普通触摸／释放而没有新重绘时不取消唯一前台帧。
- 本轮没有新增按下即推屏的独立反馈，没有缩短 DU 波形，没有局部绘制跳过整页 UI 构建。
  `UiListActivity` 原有的“一次释放触发一次动作”保持不变，避免重新引入两次点击问题。
  阅读正文的波形和前后页缓存也没有改动。
- 未修改 CPU、Flash、PSRAM 或 PCLK 频率；没有导入第三方固件代码／二进制。

## 代码入口与资源

- `lib/hal/NativeGrayRefreshPolicy.h`：无分配的差分／区域账本；52 B 固定状态，
  最多两个区域步骤，复用 epdiy 已有 front/back/difference 缓冲。
- `scripts/patch_rickyos_epdiy.py`：维护驱动扩展与生成的策略头，不把 SDK 的派生文件当第二套源代码。
  覆盖全新 SDK、重复构建、修改后拒绝静默跳过；诊断关闭时 Screen repair 也能编译。
- `lib/GfxRenderer/GfxRenderer.cpp`：逻辑导航矩形到四种物理方向的转换。
- `lib/hal/HalDisplay.*`：保持 HAL 边界；非 Read Pico 为 no-op。
- `src/activities/ActivityManager.*`：请求代次、灰阶入口和阶段日志。
- `src/components/RickyDesignTokens.h`：实验清理间隔。

没有增加整屏／控件 heap 缓存，也没有新建任务。SDK 原来已有的工作区修改保留。
本次 Read Pico 构建静态 RAM 为 85,808 B，相对上一候选 85,704 B 增加 104 B；
这不是运行时内存余量证明。

固件 `.bin` 6,405,840 B，OTA 槽 6,553,600 B，原始余量 147,760 B。
SHA-256：`991719ba8580046f388eaac313828dac21ba72cf60d932f29f864a3700fe7004`。
本地候选保存在忽略目录 `build/artifacts/rickyos-1.2-gray-regions/`。
正式版本号、OTA 清单、安装网站和远程分支未修改。

## 诊断读法

只在私有 `rickyos_readpico` 环境启用 `FREEINK_READPICO_DIAGNOSTICS=1`，正式推广前需关闭或 A/B 测试日志开销。

- `[PERF] render wake`：最早合并重绘请求到任务唤醒、等待 RenderLock 的时间。
  不是完整的“手指按下到第一个像素变化”测量，也不包括发出请求之前的目录扫描。
- `Direct gray ... paint/submit/total`：页面绘制和提交时间；原生画布 begin 的初始化开销不在 paint 内。
- `[EPDF] UI damage`：内容／导航的实际变化包围盒、步骤数、分类耗时。
- `[EPDF] frame`：差分、扫描、复制、队列陈旧状态；`convert` 是原有 1-bit 展开诊断，
  不能拿它当原生 UI 绘制耗时。
- `[PERF] gray heap`：internal、DMA、PSRAM 的 free / 最大连续块。

## 验证

- `pio run -e rickyos_readpico`：通过。
- `pio run -e simulator_rickyos`：通过。PlatformIO 切换平台可能移除另一环境的构建目录，
  所以先保留硬件候选，再构建模拟器。
- `test_native_gray_refresh_policy.py`：3 项通过，含原生驱动／高层提交、重复帧不通电、
  区域 GC16 不驱动未变行、失败恢复、取消、OOM、PMU 并发及全新／重复 SDK patch。
  macOS 没有 GNU linker `--wrap`，SDK 的 Linux-only 资源／队列分配故障测试未运行。
- `test_native_grayscale.py`：通过，含真实 renderer 四方向几何、灰阶、取消、OOM、图像及待机。
- `test_rickyos_direct_gray.py`：4 项通过，包含通知已消费后的过期帧取消，以及保持手动全刷。
- 隔离模拟器 `check_design_scenarios.py`：空书库、长昵称、6 本书与全部／在读／未读筛选通过。
  `check_interior_preview.py`：首页、设置及子页、存储／文件浏览、阅读菜单、弹窗返回和横屏通过。
  菜单帧 16 种灰度像素，测试正文 13 种；已查看设置／书库截图，无新增裁切或图标布局改动。
- 产品 `test_rickyos_*.py`：216 项，204 通过、11 跳过、1 个已有失败。
  `test_rickyos_clock_digits.test_header_matches_generator`：当前 Pillow/FreeType 输出数字 1 的 advance=52，
  已提交字体为 51；与这次显示调度无关，没有重生成字体来掩盖差异。
- `git diff --check`、修改的 C++ 文件格式检查通过。

尚未在这次候选上做物理面板的响应／残影验收，也未构建所有其他硬件目标。
模拟器只能证明布局、交互和灰度像素，不证明波形效果或实机速度。

## 下一次刷机验收

1. 连续切换首页／书库／应用／存储／设置至少 40 次，查看普通操作与区域清理时底栏是否保留。
2. 快点、慢点、连点设置控件；一次释放只能执行一次动作，不能出现需点两次或旧页面短暂回放。
3. 在书库筛选／翻页与设置行间反复切换，比较残影；记录 `paint`、`classify`、`diff`、`scan`，
   每类至少 10 次，区分普通帧和清理帧，再比较中位数与长尾，不能只凭一次点击。
4. 测试手动全刷、屏幕修复、四个方向、阅读菜单进入返回、待机唤醒，核对无旧区域污染。
5. 检查 PSRAM／内部／DMA 最大连续块；含图阅读后再返回菜单，确认没有因缓存使扫描资源不足。

接下来依据这些日志选择局部 UI 绘制、独立短反馈或缓存调度的下一轮，不先靠超频隐藏瓶颈。
