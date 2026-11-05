# Plus 图片阅读质量与竞争方案复核 · 2026-10-20

分支：`codex/plus-online-fix-1.2.2`，正式代码基线 `683bc1c2`。
承接[图片页反色修复](plus-reader-images-20261020.md)。本轮候选已于 2026-10-20 按用户要求刷入 Plus，
未提交 / 推送或发布；新候选的图片画面与交互仍待用户验收，不继承上一份固件的验收结论。

## 竞品不是都在整页两遍上屏

`wegooo-cell/read-pico-reader` 的当前对外名称是 kiikoread，厂长与 KiikoRead 不是两份独立实现。
2026-10-20 核对远程 HEAD 仍为 `2087afda444b17fb320562fc31ab9424b661e946`。

- [app_book.c:2881](https://github.com/wegooo-cell/read-pico-reader/blob/2087afda444b17fb320562fc31ab9424b661e946/main/apps/app_book.c#L2881)：
  插图采样写入与文字共用的灰阶画布，而不是先出整页 B/W 预览。
- [app_book.c:3228](https://github.com/wegooo-cell/read-pico-reader/blob/2087afda444b17fb320562fc31ab9424b661e946/main/apps/app_book.c#L3228)：
  图片页正文按设置走 GL16 差分或 GC16；可能另刷页脚 / 全屏进度，因此不能说每次翻页只有一次驱动调用。
- [display.c:323](https://github.com/wegooo-cell/read-pico-reader/blob/2087afda444b17fb320562fc31ab9424b661e946/main/display.c#L323)：
  主页有区域同帧策略，水波纹直接输出目标灰阶；其他暂存灰区的路径仍可能补 GL16。
  不能从某个 UI 分支的两次调用反推其图片阅读一定显示黑白中间页。
- [book_cover.c:173](https://github.com/wegooo-cell/read-pico-reader/blob/2087afda444b17fb320562fc31ab9424b661e946/main/book/book_cover.c#L173)：
  渐进 JPEG 仍采用 DC-only 1/8 预览，不能通过照搬解决《银河英雄传说》的细节损失。
- WhiteOS 远程 HEAD 为 `9883f4a6a42db32b5639e51d9da84050291f4a89`。
  [公开仓库说明](https://github.com/446599/whiteos/blob/9883f4a6a42db32b5639e51d9da84050291f4a89/README.md)
  列出的主要是网站、固件镜像、清单及许可；网页翻页示意也明确不证明实体屏波形、速度与残影。
  本轮没有得到可对应当前固件的阅读显示实现，故不能确定其提交次数，不推测闭源实现。

“精排 EPUB”不是另一种固定文件格式。样书中的图片、透明装饰、字体和 CSS 必须分别验证；
同画布灰阶并不自动意味着支持所有 CSS、WebP、浮动布局或任意 SVG。

以上仅只读研究，无竞品代码复制。新渐进 JPEG 使用独立的 stb_image MIT 许可实现，
不是从个人学习许可项目中移植代码；厂商面板波形 / VCOM / 供电参数不照搬。

## 已实现的候选

### 图片、文字合成后再提交

- `upstream/src/activities/reader/EpubReaderActivity.cpp:2323` / `:2832`：Plus 普通含图页
  不再以“全部 JPG”为原生 16 级路径前提，PNG 与混合图片页也进入同一个目标帧。
- 沿用原生路径已有的 B/W 绘制 / 显示跳过分支，再完成页面与状态栏，提交一次原生目标。
  并非删除旧底图调用后把未完成的蒙版发给驱动。
- 图片即使在文字 AA 关闭时仍用原生灰阶；字形覆盖曲线单独量化为黑白，保持用户的文字 AA 选择。
- 夜间反色模式保持既有 B/W 契约；提交失败仍有完整 B/W 重画回退，取消不发布半成品缓存。
- 原有相邻页 PSRAM 缓存、前向章节优先、取消代际和失败恢复保留，未新增驻留整屏缓存。

### 透明 PNG 与格式识别

- `upstream/lib/Epub/Epub/blocks/ImageBlock.cpp`：原生目标下先确保解压，再选择实际解码器，
  绕过旧 4 级抖动 `.pxc`；不依赖清空整张卡缓存。
- `ImageDecoderFactory.cpp`：对已存在文件最多读取 8 字节识别 JPEG / PNG，兼容旧缓存里后缀写错的图片。
  尚未解压的文件仍可按后缀懒探测；不读取整文件判断格式。
- `PngToFramebufferConverter.cpp`：允许原生目标上的双线性采样；同时插值预乘灰度与 alpha，
  避免透明像素的任意 RGB 在缩放边缘形成黑 / 白圈。
- 继续流式逐行 PNG 解码，最多四条来源行（上 / 下两行的灰度与 alpha），不分配完整 PNG 位图。
  最近邻与平滑选项仍按用户设置生效，没有静默覆盖已有偏好。

### 完整渐进式 JPEG

- Plus 专用 `port/PlusProgressiveJpeg.cpp`，stb_image 仅编译 JPEG / 软件解码部分；
  标准 JPEG 保留现有 JPEGDEC。不是 S31 硬件 JPEG 接入。
- 完整读取渐进扫描与 AC/DC 系数，得到来源分辨率的灰度样本，再按最近邻或双线性缩放到绘制尺寸。
  不把 87×125 预览当成 694×1000 的精细图片。
- 大对象只分配 PSRAM，每次解码预算上限 10 MiB、单图来源上限 2,097,152 像素，
  每次分配保留至少 512 KiB PSRAM；实际分配失败可恢复，不强占内部 DMA RAM。
- 16 KiB 输入预读，减少 128 字节回调造成的小 SD 读取；不同时保留旧压缩图 PSRAM 副本。
  新开销仅在解码期间存在：完整渐进系数无法用几个扫描行恢复，不能放在渲染任务栈。
- 在渐进扫描行、最终 IDCT 行、读取与缩放行上检查取消并沿用让出机制，取消后释放所有临时对象。
  vendor 文件只添加可选检查点，原版权 / MIT 双许可文本、来源 pin、原始和补丁后 SHA 均保留。
- 超过来源 / PSRAM 预算时明确记录日志并保留历史 DC 预览作为降级，不以缺图替代原本能读的插图；
  损坏 / 取消不触发该降级。不能把低内存的降级结果宣传为完整精度解码。
- 额外检查真实 JPEGDEC 探测 / 降级时，UBSan 发现其短 Huffman 码使用 `cc >> (iBitNum-5)` 的
  负位移（来源 `jpeg.inl:926,1115`）。`prepare_plus_jpeg.py` 校验原始两文件 SHA，
  在私有构建副本对这两处增加 `iBitNum >= 5` 守卫；CMake 编译该副本，不修改共享 `.local/dependencies`。
  保留原 Apache-2.0 版权与许可，不屏蔽 shift sanitizer 来掩盖问题。

## 验证

- 完整 Plus 应用构建与触控 / 单一时钟链接校验通过，IDF 仍为 `9a97f6c5`；既有警告保留。
- `test_plus_reader_native_images.py` 执行生产原生判定、文字 AA-off 曲线、格式签名选择及 OOM，
  同时核对真实阅读方法不提前输出 B/W、正常路径一次 native commit、失败回退和夜间边界。
  提交顺序检查不是光学波形验收。
- `test_plus_image_quality.py` 编译实际新 JPEG 引擎、实际 PNG 转换器和固定 PNGdec：
  合成两像素棋盘能恢复细节；每个 JPEG 分配点失败、每个让出点取消、短读、超限、透明缩放与页边界通过。
  并执行实际 JPEG 转换器的完整解码 / PSRAM 失败降级与取消，检查文件回调只关闭一次。
  ASan / UBSan 开启并要求 UBSan 出错立即停止。仅 PNGdec `inflate.c` 的未对齐整数复制及
  JPEGDEC 主机优化读取不检查 alignment；没有屏蔽应用转换器、分配器或 vendor 的其他 sanitizer 检查。
- 用户《银河英雄传说》的 8 张渐进 JPG 均用实际新引擎完成主机解码。
  与 Pillow / libjpeg 灰度 + 双线性缩放到 600×1000 的独立参考比较：
  MAE 依次为 `0.093, 0.194, 0.027, 0.867, 1.639, 0.720, 0.875, 0.238`（8-bit 0–255）。
  这验证解码 / 采样细节，不等于面板 16 级量化后的观感或速度；样书及输出只保存在私有 `.local`。
- `test_plus_image_page_polarity.py` 的实际 SDK facade + Plus 驱动检查保留，覆盖旧平面通道、夜间及转回任意原生灰度。
- 缓存、USB、外设等完整 Plus 回归记录在 `.local/tests-reader-quality-final-20261020.log`。

## 本地候选与真机验收

归档 `.local/reader-quality-20261020/` 包含匹配 BIN / ELF / MAP，不覆盖上一份反色修复测试包。

- 版本 `1.2.2-dev`；应用 4,859,072 B。
- BIN SHA256：`d52b220d792b2a270ae3975a09a3119b5d8be9c683557bebb936d2fe989516bc`。
- ELF SHA256：`d73f1268d8465bfe97b819e7a642b346948357685e47c0b72c8f932f3e0b3da0`。

### 测试机安装记录

- 确认 ESP32-S31、MAC `1c:29:04:d0:2a:50`；实读分区表与 OTA 记录，app0
  为 `0x10000` / `0x640000`，当前选择序号为 1，校验有效。
- 备份当前完整 app0（6,553,600 B），SHA256
  `c3e894c5fb056545e4557d758146dc3ee19162e27611cf84ca765e67775b5421`。
  已核对其应用前缀与用户刚测试的反色修复候选逐字节一致。
- 仅向 app0 的 `0x10000` 写入上述 4,859,072 B；esptool 设备端摘要校验通过。
  未通过刷机工具写启动加载器、分区表、OTA 选择区、NVS、SPIFFS 或 SD 卡。
- 校验后显式执行 USB Serial/JTAG 硬重启；启动日志确认 `1.2.2-dev`、ELF `d73f1268d…`、
  app0、Plus 屏幕初始化与进入 `InxRecent`。22 秒启动观察无 panic / abort，不等于阅读功能验收。
- 私有记录 `.local/device-installations/reader-quality-20261020/`：分区 / OTA 实读、
  `pretest-app0.bin`、`backup.log`、`flash.log`、`boot-confirmed.log`；不提交设备私有文件。

已完成安装与启动确认，画面与交互尚待验收。用同一 Plus、SD、书籍页、字体和刷新间隔检查：

1. 《间客》《超神机械师》PNG / 混合页是否不再出现独立 B/W 中间页；下一页文字是否正常。
2. 《银河英雄传说》插图细线、灰阶与页边，分别比较最近邻 / 平滑；低分辨率原图本身不能凭空补细节。
3. AA 开 / 关、夜间模式、菜单进退、锁屏恢复、连续跨章、连续翻页与反向翻页。
4. 分开记录解码 / 绘制 / 提交耗时和首个完整画面、稳定画面的时刻，不以一次 API 调用推断没有光学闪变。

保留边界：夜间旧图像路径、书架 1-bit 缩略图、WebP、完整 SVG / CSS、交错 PNG、
超预算渐进大图及面板波形残影不宣称在本轮全部解决。光学灰阶驱动本身仍可能短暂变深或闪动。

### 本轮用户实测反馈与进一步诊断

用户反馈仍“先黑色，再变灰”。因此本轮不能把这一体验问题记为通过；主机提交顺序测试通过
不代表消除了可见的光学中间状态，不能以“只有一次提交”否定用户反馈。

只读核对发现：`MetalioPlusDriver.cpp:106–116` 将普通原生阅读提交交给 GL16，
周期清理交给 GC16；`refresh():218–236` 根据既有光学状态决定是否先恢复基线。
`my_waveform.c:19–36,154–160` 当前沿用完整 48 相表（未在这轮改动）。
按 `my_waveform_gl.h` 的 `[phase][target][old/4]` 解码：

- 白 15 → 浅灰 12：phase 0–20 hold，21–38 action 1（dark），39–45 action 2（clear），46–47 hold。
- 白 15 → 灰 8：phase 0–22 hold，23–40 dark，41 hold，42–45 clear，46–47 hold。
- 灰 8 → 同灰 8 也有反向再回写的脉冲，不是完全 hold。

动作含义与 `output_common/lut.c` 的 from-white/from-black 单色表及
`output_lcd/render_lcd.c` 的 DARK/CLEAR 编码一致。这说明一次灰阶提交本身也可先压深再回浅，
是用户现象的原因候选；不把指令序列当作已经测得的反射率轨迹。

诊断期间没有再次刷机、调整波形 / VCOM / 电压 / 时序。第一次 25 秒串口窗口无数据，
后续窗口见私有 `gray-transition-live.log`；没有匹配翻页事件时，不能以空日志排除额外刷新或
确定此次选用了哪种波形。已请用户提供书名、是否每次出现及短视频，需同步普通连续翻页与周期清理。

### 连续含图页反馈后的局部对照候选

用户进一步确认：《间客》《超神机械师》的含图页每次均出现，不只首次打开或间隔清理。
不能继续以周期清理解释全部现象；也还不能仅凭出现频率排除额外软件提交。

检查到 `EpubReaderActivity.cpp:3003–3007` 的普通阅读提交注释声称采用 held-diagonal 表，
但 Plus 的 profile 1 实际只选择原 GL16；`my_waveform.c` 并没有派生保持对角线的阅读表。
全屏扫描按变化行和跨行合并的列掩码选择像素，因此与变化正文共用行列的、未变的章头图片
仍可能命中非零的同灰阶转移。该差异与新图片白→灰的压深过程是两件事。

本地候选增加独立的阅读差分表，使用已有 EPDiy `MODE_GL16_FAST` 标识（type 6），
**这里的名称不表示更快：相数、时序及每个变化像素的完整转移均与原 GL16 相同**。
只把 old==target 的 16 条对角线全部设为 hold。普通阅读 profile 1 选此表；
未知光学状态、手动/周期清理仍采用原 GC16，UI 和旧 overlay 路径仍采用原 GL16。
原表、电压、VCOM、供电时序、温度参数、页面刷新间隔均不改。

额外资源为固定 3,072 B 静态 LUT 和小型只读描述符，不新增堆分配/帧缓冲。
`test_plus_reader_waveform.py` 编译实际装配函数，对完整/裁剪配置逐相、逐 old/target
验证：仅对角线变化、原 GC16 未改、原 GL16 的既有白脉冲保留、初始化幂等。
驱动测试验证普通翻页仅一次阅读模式提交，未知状态和清理模式不被绕过。

**范围限制：白→新灰阶依然含 dark/clear 阶段，不能宣称消除了用户报告的先黑后灰。**
该候选只用于区分“同图反复被驱动”和“新图像素本身的灰阶波形”两个因素。
完整测试、构建与真机验收状态分别记录，未验证的候选不作为线上修复发布。

用户随后补充：《银河英雄传说》的 JPEG 插图也出现同样过程，因此不能把问题限定为
PNG、章头图或进阶 JPEG 的解码回退。三个样本共同的原生灰阶显示/驱动路径优先排查。
这增加了波形光学过程假设的可信度，但仍不是已采集到相应翻页提交时序的证据。

对照候选已完成全部 Plus 主机回归（含 ASan/UBSan 的新波形/驱动测试）和 S31 固件构建，
源记录核验为 2,844 文件 / 98 适配；不据此宣称真机画面通过。
归档 `.local/reader-held-diagonal-20261020/`，应用大小 4,859,184 B：

- BIN SHA256 `1fa401e70f975266b1dd06485e6dafb99c21a2ac98e4596ad15fd1fa539e8f24`
- ELF SHA256 `6dab78e0f19e689d660a4455f280b8317152955f69a0a525069c52374a769a4e`
- 构建日志 `.local/build-reader-held-diagonal-20261020.log`
- 回归日志 `.local/tests-reader-held-diagonal-20261020.log`

**本轮未刷入该候选**，设备仍为上一轮已安装的 `d52b220d…` 应用。
未提交/推送/发布，不把诊断对照版替换线上固件。

### 对照候选刷入（用户随后明确要求）

用户明确要求刷入后，实读 Plus `1c:29:04:d0:2a:50` 的分区表与 OTA 记录，
校验分区 MD5、OTA CRC、序号 1 / app0，确认目标仍是 `0x10000` / `0x640000`。
再次核对归档 BIN SHA256 为上述 `1fa401e7…`，仅写 app0 应用，esptool 返回
`Hash of data verified.`。未通过刷机工具写 bootloader、分区表、OTA 选择区、
NVS、SPIFFS 或 SD 卡。

显式执行 USB Serial/JTAG 硬重启；启动记录确认 app0、`1.2.2-dev`、ELF
`6dab78e0f…`、四种装配波形、Plus 屏幕初始化和 `Entering activity: InxRecent`。
首次自动断言漏掉活动日志的冒号，已按实际日志格式独立复核成功，不能将其误报为启动失败。
原始记录在 `.local/device-installations/reader-held-diagonal-20261020/`。

安装/启动确认不代表先黑后灰已解决；三个书籍样本的光学效果仍待用户对照反馈。

### 对照实测无明显改善；首次匹配到阅读刷新日志

用户对 held-diagonal 候选反馈“没啥变化”，因此本轮视觉目标未通过，不作为成功修复。
随后保持现有固件、未复位/刷写，进行 45 秒只读串口监听，实际捕获 3,817 B。
私有记录：`.local/device-installations/reader-held-diagonal-20261020/live-image-turns.log`。

- 14 条阅读 `refresh` 与 14 条 `Rendered page`，顺序上一一对应。
- 13 次 mode 6，均 `full=0 clean=0 previous=2 result=0x0`，驱动耗时 851–860 ms，
  中位数 855 ms；1 次 mode 2 周期清理，886 ms。
- 阅读区间没有 mode 1 黑白刷新；窗口前部另有两个阅读菜单区域刷新，不算翻页。
- 这些记录支持当前普通翻页使用单次灰阶驱动，并非每页被周期全刷覆盖；不等于高速摄影证据。
  日志不含逐页书名/图像标志，也不能声称已分别采集三本书的每张含图页。
- 普通阅读的 48 相扫描约 682–691 ms，其余为驱动前后处理；整页 `Rendered page`
  计时还包括布局/字体/图像工作，不能都算作波形时间。
- 同窗口一次 `SCT Parameters do not match` 发生在字体设置返回后，与后续成功刷新分开记录，
  不解释为此次光学变化的已证实原因。

mode 6 保留原 GL16 的所有 old!=target 转移，因此白→灰先 dark 后 clear 的序列仍在；
用户无改善与这一边界一致。下一步需要针对目标面板验证新像素灰阶过渡，不能再把仅保持
对角线、缩短解码或一次提交的验证当作消除光学压黑的修复。

只读检查厂方公开仓库 `CloudZao/Metalio-E-INK4-Plus`：该波形装配文件当前最后提交为
`2970d347fd091c510e8f6fd6fa4465ea502cbd56`（2026-09-30）；
公开 GL/GC 头文件 Git blob 分别为 `df246594ec7be6045a458b8cb420385cabe3657f`、
`dffab53de7f62030fc51f7e1eae84cff3818263a`。这次检查未发现可直接替换的新增面板校准表，
不借用其他型号 LUT，也不盲删压黑相/修改 VCOM 后宣称问题已解决。

### 用户要求继续实现：阅读专用短波形候选

按用户要求直接修改普通阅读的实际驱动过程，而不是再次只改未变化像素。
采用本工程原有 `my_waveform_trim` 的参数组合：erase_max=11、sat_cut=5、
white_sat_cut=0、hold=3，从完整原 GL16 派生 37 相阅读表；保持对角线继续为 hold。
例：白 15→浅灰 12 的 dark 从 18 相缩到 13 相，clear 尾仍为 7 相。
这会实际改变驱动能量/灰阶响应，**不是已校准的等色替换，也不保证不再先黑后灰**。
用 Plus 实机对照灰阶深浅、文字、残影和响应后才能决定保留或回退。

仅 profile 1 普通阅读选择该表。周期/手动清理、未知基线恢复仍保留原完整 GC16；
UI 原 GL16、帧缓冲16级、图片解码、刷新间隔、电压/VCOM/扫描行时序不改。
短表固定 2,368 B，不新增堆/帧缓冲。初始化暂借既有 3,072 B GL 工作表，
派生后完整恢复原 UI 表；未对已裁剪表二次裁剪。

为避免首次启用裁剪函数额外链接 17 KiB 静态临时序列，装配函数改为两遍计算，
局部 scratch 内容 132 B（不含编译器栈帧开销），输出算法和次序不变。
独立 Python 参考模型按源 GL 表逐个 old/target 推导脉冲，ASan/UBSan 执行实际 C
装配后逐字节比较全部 256 个转移；验证短表 37 相、18→13 dark / 7 clear、
对角线保持、原完整 GC/GL 未改，以及完整/裁剪配置下初始化幂等。

短波形版全部 33 项 Python 入口和 3 个原生测试程序通过；S31 构建通过，
触控链接、时钟对象归属及 2,844 文件 / 98 适配核验通过。
初次链接发现原 CMake 只编译装配文件、没有编译此前未启用的裁剪函数，补入
`display/waveforms/my_waveform_trim.c` 后完整构建通过，并增加来源列表回归断言。
这不代表光学效果通过。

候选归档 `.local/reader-short-waveform-20261020/`，应用 4,859,888 B：

- BIN SHA256 `ea5dbc6c203d64ec1fda759003b63f584e21d8f395e47420da77a1bd9825954d`
- ELF SHA256 `b104425af9a7a2b0c2bba68083759818c41304d87a05d574b9e097ca3b5a957a`
- 成功构建日志 `.local/build-reader-short-waveform-20261020-final.log`
- 回归日志 `.local/tests-reader-short-waveform-20261020.log`

上一版 `1fa401e7…` 和图像质量版 `d52b220d…` 的应用/匹配 ELF 均保留用于回退。

短波形候选已安装到测试 Plus（MAC `1c:29:04:d0:2a:50`）。再次实读分区与 OTA 记录，
核验分区 MD5、OTA CRC/序号及 app0；仅向 `0x10000` 写应用，设备端摘要通过。
显式 USB Serial/JTAG 硬重启；日志确认 ELF `b104425af…`、app0 和进入首页。
刷机工具未写启动加载器、分区表、OTA 选择区、NVS、SPIFFS 或 SD 卡。
记录目录 `.local/device-installations/reader-short-waveform-20261020/`，
其中 `boot-and-first-turns.log` 同时观察启动与随后用户操作。
启动时 GC16 为 48 相是预期行为，不能据此误判普通阅读短表没有启用。

35 秒启动/阅读观察确认用户进入 `EpubReader` 后实际执行 `mode=6` / `frames=37`：
一条扫描 cadence 12 ms 的提交为 604 ms；后续 cadence 约 14.2–14.4 ms 的完整普通提交
为 695–704 ms，扫描 527–533 ms。周期清理仍为 48 相，记录到 886 ms。
上一版捕获普通提交 851–860 ms，可见相数缩减机制已在设备上生效；不同会话/页面不是
严格配对性能基准，不宣称所有书籍平均翻页改善同一比例，更不等同于压黑观感通过。
窗口末端有一条扫描完成、提交日志尚未输出，统计时不补造该条耗时。
本次观察无 panic/abort 横幅；灰阶对比度、残影和先黑后灰的用户验收仍待反馈。
