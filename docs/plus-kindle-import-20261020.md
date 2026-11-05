# MOBI / AZW3 接入进度 · 2026-10-20

开发分支 `codex/plus-kindle-formats`，从已推送的 Plus 性能检查点 `bcf66632` 创建。
用户指定设备暂时被占用：**本轮只做主机和模拟器，不连接串口、不刷机、不操作设备 SD 卡**。
用户于 2026-10-20 授权先行提交并推送本功能，交接目标为私有仓库
`chinoryunqin/RickyOS-dev:codex/plus-kindle-formats`；不合并正式版、不发布固件，
性能审查分支保持原提交。用户样书、截图、固件和本地运行数据均不上传。

## 实现范围与设计依据

现有 Plus `Epub::load()`（还原源码 `lib/Epub/Epub.cpp:528`）负责缓存与排版，
`readItemContentsToStream()`（`:997`）负责资源读取；保留原书路径作为进度、书签和统计身份，
首次打开生成设备本地派生 EPUB 缓存，再进入现有阅读界面，不把后缀识别当作格式支持。

格式参考 [foliate-js 的 MIT 实现](https://github.com/johnfactotum/foliate-js/blob/78914aef4466eb960965702401634c2cb348e9b1/mobi.js)，
许可证与固定参考提交保存在 `components/rickyos_application/port/LICENSE.foliate-mobi`
和 `kindle-source.json`。不引入 libmobi/LGPL 依赖，不包含 DRM 解密、密钥处理或 KFX 支持。

初期支持未加密、可重排的 MOBI6 / KF8，UTF-8 / Windows-1252，未压缩 / PalmDOC /
HUFF-CDIC。其他编码、加密或超限结构必须安全拒绝，不能白屏、截断后冒称成功。

## 已实现与验证边界

已实现格式层 `KindleDocument.{h,cpp}`：PDB 记录边界与头、EXTH 元数据、混合 MOBI/KF8
边界选择、PalmDOC、有限递归 HUFF/CDIC、FDST 和 INDX/TAGX/CNCX 读取。
按记录向输出回调解压，不在设备内存中保存整本正文。明确检查文件/记录/输出长度、
字典容量/递归深度、压缩引用、取消与 IO 失败；PSRAM 分配可失败并保留容量余量。

自建测试文本验证精确输出；ASan/UBSan 检查 PalmDOC/HUFF、短读、取消、释放。
两本用户样书仅在本机读取，不修改、不提交：

| 样书类型 | 压缩/版本 | 解压输出 | 目录 | KF8 重组索引 |
|---|---|---:|---:|---|
| MOBI | PalmDOC / 6 | 136,001 B | 32 | 无 |
| AZW3 | HUFF-CDIC / 8 | 654,547 B | 49 | 14 骨架 / 81 片段 |

AZW3 头部正文长度为 648,422 B，FDST 另有 6,125 B 的 CSS 流；总解压输出不能只按
正文长度校验。两本样书的完整正文均已与独立参考实现解压、KF8 重组后的正文比对一致
（忽略 HTML 排版空白）。导出的每张 JPEG/PNG 与源记录逐字节一致，没有二次压缩。

已接入 `KindleImport` 和现有 `Epub` 阅读引擎：

- 首次打开在本书缓存内流式生成 ZIP32/stored 派生 EPUB。中央目录临时落盘，不在 RAM
  堆积整包；每次只保留一个来源章节与一个 KF8 重组章节，每个上限 2 MiB。
- HUFF/CDIC 字典在文本解压后立即释放，不与双章节缓冲同时驻留。所有大缓冲位于
  可失败的 PSRAM 分配；桌面有单独的 host 分配路径，不把主机内存结果当实机预算。
- 保留原书路径作为书库、进度、书签、最近阅读、统计身份；仅 ZIP 内容读取走派生路径。
- MOBI 旧式 HTML 规范化、中文/CP1252、实体、分页标签、KF8 分段拼接、CSS flow、
  图片引用、NCX 目录和文件位置链接转为 XHTML/本地锚点。
- 书库、存储打开、Wi-Fi 上传后的索引失效和格式标签接入 MOBI/AZW3；未加到 Pico/C3。
  普通目标不开 `RICKYOS_KINDLE_IMPORT`，也不增加 `Epub` 的派生路径成员。
- 背景封面扫描不触发整本导入；首页/统计的 `load(false)` 只读现有缓存，不进行转换。
- 导入中止或短写不发布未完成的归档；旧完整归档、阅读进度及用户封面保留，支持重试。
  源文件改变后，重建该书的排版/CSS/派生图片，不保留旧 `img2_` 图片冒充新资源。

缓存标记 `kindle.v2` 是 16 字节：`RKI` + revision=2、LE32 源大小、LE32 全文件 CRC32、
4 个保留零字节；标记最后写入。`kindle.epub` 可删除重建，原书从不修改。未改变现有
`book.bin`/`section.bin` 布局或版本。第一版局部预览的 `kindle.v1` 不再作为有效标记。

主机检查包含 ASan/UBSan、按分配点失败、短读、加密/不支持压缩/坏记录表拒绝，
导入取消、短写后的完整重试、源变更失效，以及进度/用户封面的保留。
每个派生 XML 均解析通过，所有目录和正文内部链接目标有效，资源路径均存在。

真实阅读引擎的桌面模拟器已打开两本原格式样书，检查中文、MOBI 图片、跨章、
两种目录列表和指定章节跳转；未连接或刷写真机。S31 完整固件构建已通过，尚非发布包。

边界：只面向未加密、可重排 MOBI6/KF8。拒绝 EXTH 标记的固定版式、其他字符编码、
超过 128 MiB 的容器/解压文本、单章超过 2 MiB、超过 4 MiB 的压缩字典等超限数据。
图片当前仅接入 JPEG/PNG；GIF、SVG 包装、内嵌字体、音视频、KFX、DRM 不在本轮支持
范围。混合 MOBI6/KF8 容器路径已实现，但还缺真实混合样本验收；目录层级暂展平。
脚注定位和返回链已转为锚点，但复杂脚注的实际弹窗/返回体验仍需单独验收。

## 检查入口

从 Plus 目录执行 `python3 tests/test_plus_kindle_document.py`，只使用仓库内自建数据。
用 `--samples` 传入用户的本地样书，可运行额外索引与解压检查；不会把样书复制进仓库。
原始派生文本留在被忽略的 `.local/kindle-{mobi,azw3}.raw`，不作为提交内容或公开测试样本。

模拟器已验证重启后从首页继续进入 AZW3 的 spine=4/page=0，旧进度在 v1→v2
派生缓存重建后保留。仍需实际点选复杂脚注/返回、字号重排、更多混合格式样书和
原有 EPUB/TXT/Markdown 的完整人工回归；不能把内部链接目标检查等同于脚注交互验收。
模拟器不能证明真机速度、内存余量或面板灰阶；用户释放设备后再单独申请真机验证。
本地预览没有安装完整 SD 字体，个别生僻字（例如本书的“纣”）显示为缺字框；
正文差分未缺少这些字符。应在完整字库条件下复查，不把字库覆盖问题冒称已解决。

本轮最终构建/检查（未安装）：

- 43 项 Python 套件和 3 个顶层 native 测试均通过；样书差分另行通过。
- 原生 S31 应用 4,885,024 B，app 分区 6,553,600 B（约 25% 余量）。
- BIN SHA256 `0a59fd97028bb762a33360a6e639088163c3cb392a6dcf3949ac5268670b8f38`。
- ELF SHA256 `59c43fff183c4d5aadc73a65cbcbfcd89b91dcb42dd87e6d2b9cb60a79f184ec`。
- 仍用本地 `1.2.2-dev` 标识，不是正式版或 OTA 发布；应用二进制不提交。
- 来源校验 2,844 文件、107 个已记录 Plus 适配通过；旧性能提交 `bcf66632` 不变。

独立预览准备：`python3 scripts/prepare_plus_kindle_preview.py --samples <本机样书路径...>`，
更新已有预览加 `--resume`。在 `.local/plus-kindle-simulator` 构建 `simulator_rickyos`，
运行该副本 `tools/rickyos-browser/server.py --port 8785 --state-dir <Plus目录>/.local/kindle-preview-state`。
预览使用实际 Plus 阅读源码与 684×1216 桌面 HAL，不模拟 S31 面板、PSRAM 或 USB。
不占用已有 8769 预览，不采集电脑麦克风。当前地址 `http://127.0.0.1:8785/`。

`tests/test_plus_kindle_import.py --samples <路径...> --reference <固定版本 mobi.js>`
会在临时副本上测试重建失败和重试，原样书只读，不会进行测试性改写。
不传参数时只用自建测试文本，已加入 `tests/run_plus_port_tests.sh` 的 43 项 Python 回归。
