# WhiteOS / kiikoread 显示与响应对比研究

日期：2026-10-11（北京时间）。本轮是源码与公开发布资料研究，不是三套固件的实机速度排名。
没有改动产品代码、SDK、构建配置或设备，没有提交／推送本研究记录。

## 比较基线与证据边界

- RickyOS：本地私有 `codex/rickyos-1.2-design`，`24c05ff7ffb06254879d9928d8531db6839e7ad1`。
  `freeink-sdk` 有之前已有的未提交修改；本报告的驱动观察描述当前工作区，不能声称纯远程 checkout 完全相同。
- kiikoread：公开 main `bb50077d51eb266e1add0cf050e79c2ac7ba95c6`；发布版本
  [v0.3.3-rc90](https://github.com/wegooo-cell/read-pico-reader/releases/tag/v0.3.3-rc90)，
  tag 指向 `d9ce21755bc5a32009120e2ababaeda7e0781bc9`。
  本轮核心显示出口、键盘、点击反馈和 sdkconfig 文件在这两个快照间相同；
  `main/apps/app_book.c` 不同，因此最新阅读资源保护不能自动归于已发布二进制。
- WhiteOS：最新公开 [v1.0.2](https://github.com/446599/whiteos/releases/tag/v1.0.2)。
  [公开仓库说明](https://github.com/446599/whiteos/blob/main/README.md)显示其主要提供网站、镜像、清单和许可，
  未找到该产品完整固件源码。网站动画不证明面板波形或耗时；帧缓冲截图不等于光学照片。
- 官方更新记录与源码是事实；社区对“丝滑”的反馈由用户提供，未在本轮独立量化。
  未运行第三方脚本，未下载／刷入它们的固件，未登录微信读书。

## 一、最值得借鉴的不是框架名称，而是刷新工作分级

kiikoread 的
[app_loop.c:146](https://github.com/wegooo-cell/read-pico-reader/blob/bb50077d51eb266e1add0cf050e79c2ac7ba95c6/main/app/app_loop.c#L146)
区分整页、区域、强制清理和无需推屏。点击反馈单独走区域差分出口。
[ui_click_feedback.c:46](https://github.com/wegooo-cell/read-pico-reader/blob/bb50077d51eb266e1add0cf050e79c2ac7ba95c6/main/ui/ui_click_feedback.c#L46)
计算真正变化的包围盒，只保留一个不超过 64×64 的控件像素缓存，灰底与底栏文字不被反馈一起二值化。

RickyOS 当前 `src/activities/UiListActivity.cpp:63` 特意不进行按下重绘，以免慢刷新吞掉释放，
导致需要点两次。滑动／选择在同文件 `:74`、`:84` 仍调用普通 `requestUpdate()`。
`src/activities/ActivityManager.cpp:170` 的主界面直接灰阶路径重新绘制页面，然后提交一帧。
驱动可以跳过相同像素，但这不等于上层不重画，也不等于已支持独立局部短反馈。

推断：局部反馈与页面提交分离，是改善“按了没有反应”观感的高价值候选。
不是简单恢复旧的按下整页重绘；必须确保释放事件不丢、一次手势只触发一个动作。

## 二、保留导航、把清理限定在变化区域

kiikoread
[display.c:161](https://github.com/wegooo-cell/read-pico-reader/blob/bb50077d51eb266e1add0cf050e79c2ac7ba95c6/main/display.c#L161)
从最后成功的 back framebuffer 保留底栏；内容区切换与选中横条分开输出。
`:240` 的普通模式统计内容区变化，第三次做区域灰阶整理、第十二次做区域 GC16 清理。
局部按钮反馈不积累整页清理次数。该策略依赖当前 Read Pico 的旋转和对齐边界，并非通用坐标模板。

RickyOS `src/activities/ActivityManager.cpp:188` 与 `src/components/RickyDesignTokens.h:239`
按成功直接灰阶提交计数，每 8 次或强制请求进入 GC16 路径。
SDK 当前 `freeink-sdk/libs/display/EpdiyLcd/src/EpdiyLcd.cpp:359` 强制清理走整屏更新。
这不意味着每次切 tab 都全刷，但小操作没有单独的区域清理账本。

建议实验：底栏、内容和局部反馈分区；保留我们现有设计，不照搬它的横条外观。
清理次数、残影程度和作用范围分别管理；不能只是把 8 改成 12 后宣称解决了残影。

## 三、灰阶本身不是问题，重复绘制与错误基准才是问题

RickyOS 当前默认直接灰阶，`src/components/RickyDesignTokens.h:233`，
不是默认等 1.5 秒从黑白“变灰”。驱动已有 held-diagonal 文字转页表与差分提交，
`freeink-sdk/libs/display/EpdiyLcd/src/EpdiyLcd.cpp:350`；相同灰像素保持的思想并非我们完全没有。

kiikoread 的
[display_main_waveform.c:53](https://github.com/wegooo-cell/read-pico-reader/blob/bb50077d51eb266e1add0cf050e79c2ac7ba95c6/main/display_main_waveform.c#L53)
也保留不变灰度，而不是随意压缩所有灰阶脉冲。普通／水波纹保留灰阶，快刷有黑白取舍。

局部灰阶不能直接复用我们目前的 begin 接口：`lib/GfxRenderer/GfxRenderer.cpp:1863`
会清空整张画布。需要明确的保留底图区域事务，使用最后成功提交的 4-bit 基准，不能用 1-bit
popup proxy 代替完整灰底，也不能在扫描中途假定目标图已经显示。失败后基准未知应可靠重建。
黑白控件可实验短 DU；有灰阶变化的控件用局部 GL16。短 DU 不是整屏灰阶通用加速表。
区域缩小也不意味着硬件扫描耗时按面积等比缩短，需同时记录扫描范围、相位与实测耗时。

## 四、缓存与资源调度比“再加缓存”重要

kiikoread main 的
[app_book.c:2291](https://github.com/wegooo-cell/read-pico-reader/blob/bb50077d51eb266e1add0cf050e79c2ac7ba95c6/main/apps/app_book.c#L2291)
在 worker 中预绘制下一页；`:2304` 分配前保留 1 MiB PSRAM 余量，图片页不盲目预绘制。
`:2528` 命中后复制成品页面。`:5189` 无触摸时每轮处理一个待加载封面，用行区域推屏。
这些是可观察的机制，不证明所有 SD 操作都异步；其目录扫描仍有同步遍历代码。

我们已有前后两页缓存，`src/activities/reader/EpubReaderActivity.h:66`，
空闲 120 ms 后预绘制，`src/activities/reader/EpubReaderActivity.cpp:2233`。
不应称为“我们没有预加载”。先测命中率、取消率和 RenderLock 持有时间，再决定调度。
书库索引无效时 `src/activities/library/LibraryListActivity.cpp:84`、`:144` 在进入流程持锁重建；
正常已有有效索引时不重扫。后续可研究先展示上次索引、分批协调变化，不能跳过真实删除／插卡校验。

资源原则：优先复用现有前后缓冲与区域账本，不新增整屏缓存。若需要按压恢复缓存，
限定到单一小控件、给出上限和释放时机，OOM 只取消装饰反馈，不影响功能。
新增缓存必须同时证明最大连续块、内部 DMA 余量和插图／网络共存安全。

## 五、带宽优化是独立实验，不直接照搬超频

kiikoread 的
[sdkconfig.defaults:7](https://github.com/wegooo-cell/read-pico-reader/blob/bb50077d51eb266e1add0cf050e79c2ac7ba95c6/sdkconfig.defaults#L7)
显式使用实验性 120 MHz Flash／PSRAM；源码注明温漂导致访问错误的风险，温度补偿没有开启。
它的 `main/display.h:25` 默认面板 PCLK 18 MHz。
我们 `freeink-sdk/libs/hardware/BoardReadPico/include/BoardReadPicoPins.h:136` 同样为 18 MHz，
而 `platformio.ini:1012` 记录保守 80 MHz PSRAM 策略，并已采用 64 KiB cache／64-byte line。

不能把二者差距简单说成“它用 IDF，我们用 Arduino”。两者 Read Pico 显示都走 epdiy／LCD_CAM；
先修刷新组织、缓存调度和渲染访问，再考虑工具链或高频配置。
公开源码里的时序或约数不是本轮实测；不能据此宣称提速倍数。

## 六、WhiteOS 可以学什么、不能确认什么

- [1.0.2 更新说明](https://github.com/446599/whiteos/releases/tag/v1.0.2)公开了三档涟漪速度、
  触点起始动画、默认抗锯齿、阅读设置与工具菜单改进，以及批注响应修复。
- [站点说明](https://github.com/446599/whiteos/blob/main/README.md)区分真实帧缓冲与合成界面，
  明确网页动画不证明实体刷新。可以学习操作路径、字号层级、手势后的最终清晰排版，
  不能从网页动画反推出异步任务、波形长度或 PSRAM 频率。
- 两种水波纹不是已经证明相同的底层算法；WhiteOS 当前产品完整源不可见，保留未知。
- 我们可在默认无动画的阅读基础上研究可选转场；动画是过渡表现，不等于页面完成更快。
  本轮不自动加入 PDF、Anki、微信读书等功能，也不复制品牌图稿。

## 推荐验证顺序（尚未实施）

1. 在现有诊断上补齐输入时间、绘制、SD／字体、锁等待、差分、扫描、copy、首次可见反馈、
   最终稳定画面、缓存命中／取消与 heap 最大连续块。不要只有总耗时一项。
2. 同一 Read Pico、同一 EPUB／字体字号、同一 TF 卡与温度，分冷开书、暖缓存翻页、跨章、
   书库换页、连续按键、带图／大库、联网和待机恢复；每组至少 30 次，记中位数及 P95。
   对竞争固件的光学速度需要真机视频／一致测量条件，不能用网页或模拟器排名。
3. 第一候选只做局部反馈、固定导航和区域清理，默认整页继续直接灰阶；不改变品牌视觉规范。
4. 第二候选测缓存调度和封面分批；用户输入应取消旧工作，但已启动的物理波形须安全完成。
5. 最后再做波形／带宽实验，分别测灰阶、白底、连续残影与功耗；不默认采用 120 MHz。

验收同时看首反馈和最终画面，不用“很快亮一下”掩盖仍然长时间不能操作。
目前未取得三套固件同条件的光学基准，因此以上是有源码依据的候选方向，不是已验证性能增益。
