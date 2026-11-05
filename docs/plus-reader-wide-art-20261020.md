# Plus 章节插图接近满屏宽 · 2026-10-20

用户要求：参考厂长的章头图布局，放大章节图片的横向占比。改自己的排版规则，不复制竞品代码。
本轮仅 Plus `codex/plus-online-fix-1.2.2`，不动 Pico、线上发布或远程分支。

## 代码依据与边界

- `upstream/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:1105` 原来把图片限制在
  `viewportWidth` 和父块的左右缩进内；宽高计算在分页之前。
- 原 `:1248` 的 `xPos = (viewportWidth - displayWidth) / 2` 已用有符号坐标。
  不应只在绘制时放大，否则旧高度仍参与分页，图片会盖住后续正文。
- `upstream/src/activities/reader/EpubReaderActivity.cpp:1789` 的 `textMargins()` 同时包括
  正文边距、阅读框架和面板朝向留边，不能为图片直接把整个正文边距改为零。
- `ImageBlock.cpp:410` 校验的是最终屏幕坐标，支持相对于正文为负、但最终仍在屏幕内的图片。

## 新行为

普通 / 沉浸阅读的大图向正文左右留白等量延伸，保留面板朝向可见区及两侧 8 px 安全边。
通过 `horizontalOutset()` 取左右可放开空间的较小值，非对称边距也不越界。
例：684 px 屏幕、正文左右各 32 px，正文仍是 620 px；章节图可到 668 px，
最终 x=8，宽高遵循源图比例。图片内容本身的白边 / 透明边不自动裁掉。

有阅读边框的模式不越过边框；古籍纵排仍沿用原先图片替代文字逻辑。
长图先受页面可用高度限制，不能以裁图、变形来保证横向占满。

大图识别是保守尺寸规则，而不是图片内容识别：来源至少 240×80，现有布局宽度至少为
正文区的一半，高度至少为正文宽度的 1/12。小图、脚注图标、细分隔装饰、40% 出版者标记
保留原尺寸。规则可能需要后续按真机样书反馈调整，不宣称支持所有 EPUB CSS 排版。

`PlusReaderImageLayout.h` 是无分配的纯计算函数，源比例运算使用 int64 并检查目标宽度溢出。
重排在分页与 ImageBlock 创建之前执行，使用同一目标宽高预留后续正文的位置。
不增加帧缓冲 / 任务 / 图片完整副本；native 图片仍写入既有 4bpp 页面，JPEG 来源解码预算不变。

## 缓存契约

Plus `ReaderRenderSpec` 增加 `u16 imageOutset`，参与页缓存 key 和 Section 参数匹配。
Plus 的 section 完整版本 81、partial 201；在 `verticalText` 后写入这两个字节，
`HEADER_SIZE` 同步增大，页数与五个 LUT 的相对 seek 不变。
ImageBlock / PageImage 的序列化字段不变。共享 Pico/C3 仍保留 79 / 203 和原 header。
详见仓库根 `docs/file-formats.md` 的 Plus-specific version 81。

旧完整 / partial 章节缓存按需重建，不删除原书、书籍元数据、整张卡缓存或阅读进度文件。
重新分页后的实际位置与章跳转仍需真机回归；不把不删进度文件等同于进度恢复已验收。

## 与速度实验隔离

默认阅读表恢复完整 48 相、只保持未变化灰阶的对角线；37 相短表仅在显式设置
`RICKYOS_PLUS_READER_SHORT_WAVEFORM=1` 时编译。正常排版包不开该标志或灰阶校准标志。
GC / UI GL 原表与已有白底维护脉冲保留，不启用失败的条带扫描、不改电压、PCLK 或 CKV。
本轮排版不能视为翻页提速或图像浓度校准已经完成。

## 验证

- `test_plus_reader_image_layout.py`：生产尺寸函数，ASan/UBSan；横竖比例、源极值、
  防裁切/溢出、小图保留、非对称边距、负相对坐标到正确绝对位置；核对实际分页调用顺序。
- `test_plus_reader_image_cache.py`：编译生产 header 写入和校验逻辑；两平台变体，
  参数更改、旧完整/partial 拒绝、逐字节截断拒绝，header 长度与共享格式未变。
- `test_plus_reader_waveform.py`：完整 / 短表分别与独立模型逐相比较所有 256 种转移。

主机测试与构建不代替光学验收。真机应检查《间客》《超神机械师》章头横图、
《银河英雄传说》竖长图，小脚注不被放大，下一页正文不重叠，换字号/朝向/阅读模式后重排正确。
保持最近邻 / 平滑选项原样，与同设备原包对比；Pico 竞品照片不当作 Plus 的计时基准。

## 本地构建与安装记录

最终完整 Plus 回归通过（38 个 Python 入口与 3 个顶层原生测试程序），包括既有图片
反色、完整渐进 JPEG、透明 PNG、页缓存、USB、输入和外设检查。
S31 固件编译、触控后端链接与单一时钟校验通过；既有 SDK / 未使用变量警告仍在。
来源核验为 2,844 pinned files / 102 Plus adaptations。尚未提交、推送或发布。

归档 `.local/reader-wide-art-20261020/`：

- 应用 `1.2.2-dev`，4,861,008 B，低于 `0x640000` 应用分区。
- BIN SHA256 `65e7f85ca512a1405359321f3b1006912dd1c1760674eaa3178f65232dffb9e7`。
- ELF SHA256 `f876b5af301a9f509e0129043e9f34e8deeef8357242eafb4501e2ffdf685a4f`。
- 编译与回归日志分别在 `.local/build-reader-wide-art-20261020.log` /
  `.local/tests-reader-wide-art-20261020.log`。

设备重新出现后，按用户此前“改好直接刷机”的要求安装此布局候选。
重新读取 Plus `1c:29:04:d0:2a:50` 的分区 / OTA / app 头，验证分区 MD5、OTA CRC、
序号 1 / app0，并确认旧 app 头与此前恢复的 `d52b220d…` 完整应用一致。
只向 app0 `0x10000` 写入新应用，设备端摘要通过。未通过刷机工具写 bootloader、
分区表、OTA 选择区、NVS、SPIFFS 或 SD 卡。

随后显式 USB Serial/JTAG 重启，日志确认 ELF `f876b5af3…`、四种波形、原 17 us 行时序、
48 相启动与首页刷新、`Entering activity: InxRecent`，观察窗口无 panic / abort。
串口已关闭。记录在 `.local/device-installations/reader-wide-art-20261020/`。

这只完成安装与启动确认；章节图片宽度、排版和阅读位置仍待用户真机验收，
不将启动的 715 / 708 ms 刷新计时当作阅读翻页提速结果。

用户随后回复“可以了”，确认本次大图宽度调整可用，并另提待机 Home 快捷键问题；
后者的修复和验收见 `plus-standby-home-lock-20261020.md`。不据此宣称翻页提速已完成。
