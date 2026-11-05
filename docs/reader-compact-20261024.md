# 阅读区紧凑排版：2026-10-24 本地体验版

## 范围与基线

- Pico：`codex/pico-reader-compact-20261024`，基于已发布 1.2.6 `ee569c14`。
- Plus：`codex/plus-reader-compact-20261024`，基于已发布 1.2.6 `dc151595`。
- 仅本地修改；未提交、未推送、未发布、未刷机。版本描述仍为 1.2.6，不能当作正式发布包使用。
- Plus 必须使用干净的补丁 IDF `9a97f6c54ec638111ce55cd36581b3c192f15207`；本次严格构建已通过。

## 本轮实现

原正文位置见 EpubReaderActivity.cpp:textMargins()，原绘制见 BaseTheme.cpp:drawRickyReaderChrome()。
普通模式过去的上方预留为用户边距 + 48 + 16；下方至少 96 像素，伴读再加 26。

1. ReaderChrome.h 新增 constexpr CompactLayout，排版和绘制复用同一几何来源，无新增堆分配。
2. 行高取 UI caption、BODY（缺字回退）、数值字体三者最大值，且至少 28，容纳 24 像素图标及其偏移。
3. 页眉上下各 4，正文间隔 8；用户上边距与页眉占用取最大值，不再重复相加。左右边距原样保留。
4. 底部安全间距 12，正文与底栏间隔 8；进度条与文字间隔 8，关闭的条目不占进度条空间。页码/进度文字保留。
5. 原版/伴读使用新底栏；沉浸/古籍仍按原规则隐藏外围栏，古籍整列对齐不变。
6. 自动翻页在文字状态栏全隐藏时仍走原提示布局和原空间预留；非高 DPI 路径保留原行为。
7. Plus 已记录 upstream-plus.patch、port-changes.json，源审计通过（2844 文件、192 适配），未强行追踪 ignored 的 restored upstream。
8. 不修改字号、行距、段距、图像缩放、整页漫画切图、刷新波形、缓存格式、进度文件或发布资源。

## 空间收益如何理解

例如用户边距 25、外围栏实际行高 34：无进度条时，正文总高度比旧版增加 81 像素；4 像素进度条开启时增加 69 像素。
这只是同一几何输入的计算，不是实机测量或“固定多两行”的承诺。实际多几行取决于字号、字体、行距、段落及图片。

## 缓存与阅读位置

viewportHeight 属于 ReaderRenderSpec 的缓存校验字段；旧整章/部分章缓存会按原机制失效并重新分页，无需全盘清缓存或更改二进制格式。
原 visible-text offset 恢复机制保持不动。分页引擎的 TallerViewportRebuildsCacheAndPreservesBodyAndOffsets 和 TallerViewportInvalidatesPartialCache 均通过。

## 验证

- Pico `pio run -e rickyos_readpico`：通过。
- Plus `build_rickyos_plus.sh`：通过，前后 IDF 校验和源文件校验通过。
- Pico 实际分页引擎 ChapterHtmlSlimParserTest：33/33。
- ReadingMode：9/9；ReaderSession：10/10。
- Pico chrome：5/5；UI 图标/正文边界：4 通过、1 跳过（可选图标重生成工具）。
- Pico 面板 5、quickwins 6、页面缓存 4、UI 回归 10 均通过。
- Plus 完整 run_plus_port_tests.sh：退出码 0，含新 compact chrome/margins 和原生灰阶状态栏保持/旋转映射回归。
- git diff --check：两台均通过。
- 两个既有 Pico 旧检查仍失败，已用未修改 HEAD 的源文件复测确认同样失败：test_rickyos_reader_gray16.py 的旧刷新函数文本断言；test_reader_image_layout.py 的旧 97/99 缓存版本断言。后者 C++ 图像边界/比例/ASan 检查通过。未为本轮掩盖断言而修改生产代码。
- 尚未启动模拟器做像素截图对比，尚未进行真机验收。

## 测试包（application only，不是完整安装包）

目录：`output/firmware/reader-compact-20261024`

| 机型 | 文件 | 字节数 | SHA-256 |
| --- | --- | ---: | --- |
| Pico | RickyOS-Pico-compact-test.bin | 5936208 | 3603a27cdeead95790c8f8bd5b1344b55fa92cc728799b49554e8edc8ddacd0a |
| Plus | RickyOS-Plus-compact-test.bin | 4967120 | 996993b6ca2e4ae49145451bb4acfbe1e0499d9ca1f1d96f4740a8920f7f5265 |

## 待验收

同一本书、同一字体字号/行距：普通、伴读、沉浸和古籍；横竖屏；状态栏/进度条组合；TTF、CJK 回退；
连续翻页、跨章、退出重开、书签/查词/脚注定位；末行不裁字、不撞页码或进度条。
确认老阅读位置包含在重排后的对应页，不要求页码保持不变。

偶发的大块留白（大图整张移页、出版物自身留白、章节末尾）不在本轮解决；不要以本包宣称所有留白均修复。
