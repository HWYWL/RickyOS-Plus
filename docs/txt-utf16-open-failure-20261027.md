# TXT UTF-16 打开失败与 E3 提示（2026-10-27）

用户提供《我在废土世界扫垃圾》TXT及 IMG_2170。照片提示“索引失败：SD 卡读写出错（E3）”。
原文件4462366B，带FF FE BOM，为合法UTF-16 LE；能严格解码2231070个字符，最长行333字符。
这不是超长行或巨大文件才触发的问题。不能只凭E3诊断用户卡坏。

## 已确认的代码路径

- `upstream/lib/Txt/TxtEncoding.h:8`：只有Unknown/Utf8/Gbk。
- `TxtEncoding.cpp:110`：UTF-8与GBK检测都失败后，仍回退Utf8。
- `Txt.cpp:33`：章节扫描从首个非ASCII字节调用上述检测；UTF-16 BOM被当作UTF-8。
- `TxtToHtml.cpp:182`：UTF-8路径拒绝FF等非法首字节，转换失败。
- `Epub/Epub.cpp:1065`：TXT读取委托streamTxtToHtml；转换返回false。
- `Epub/Epub/Section.cpp:363`：建索引默认错误类别为Io，前置转换失败会被显示为E3。

## 离线实证

直接链接本Plus快照的`TxtEncoding.cpp`和`TxtToHtml.cpp`，宿主仅提供Print sink、
文件回调及文件名工具桩；没有真实SD、没有注入读写失败，开启ASan/UBSan：

- 原文件：encoding1/Utf8，convert_ok0，input_read_error0，首块8192B后即失败。
- 无损转UTF-8后：encoding1，convert_ok1，完整读入6414387B，产生6656676B HTML。
- Unicode正文及CRLF逐字符一致，原文件不改。实际用户SD及固件阅读验收尚未进行。

原/转换文件SHA256及复现程序、输出记录位于本地
`output/research/plus-memory-20261026/txt-utf16-user-report-20261027.json`及`txt-utf16-repro/`。
转换副本保存在Downloads/RickyOS-TXT-UTF8，未上传到网络，也未将用户图书提交到仓库。

## 当前处理范围

可立即使用转换后的UTF-8副本；q9动效包没有修改TXT编码支持。
固件侧仍需独立修正UTF-16兼容和错误分类，不能仅给detect新增枚举就宣称支持：
章节扫描必须按UTF-16码元识别换行/标题，正文需流式处理LE/BE与代理对，原始字节
偏移映射要匹配目录/进度，旧chapter/HTML/map缓存的版本与校验也需一起设计。
不能以“SD读写失败”掩盖编码错误，也不能在检测到UTF-16后静默按GBK/UTF-8解码。
