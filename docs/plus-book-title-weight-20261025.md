# Plus 首页／书库书名字重 · 2026-10-25

用户确认：首页和书库手动全刷之后，封面下书名仍明显比 Pico 细。
这是光学反馈，不能仅以两台字库文件相同推断两台看起来相同。本轮在 Plus
本地 `codex/plus-reader-ink-20261025` 上保留 ink1 正文波形，单独加强书名的真实字体字重。

## 查到的原状态

- Pico 1.2.7 与 Plus 1.2.8 的 BODY 都是 `notosans_cjk_12_rickyos`，源文件
  SHA-256 都是 `01a55168acde0de7b30fbfc17fe739c2dc83502ac9ec6031002acbccfb1c06e1`。
- 首页最近书籍与书库网格／列表原先都画 BODY Regular；不是 Plus 单独换成了细体。
- 首页继续阅读优先 14pt Medium，但只覆盖 UI 子集。包含「活」「房」「械」等字时整串退回 BODY Regular。
  《阅读与生活》《随身书房》《超神机械师》《银河英雄传说》在两台源码上都会回退。
- 设备面板／灰阶响应差异仍可能影响观感；这轮采用可检查的真实 Medium 字形，而不是宣称已经测得两台光学墨色一致。

## 实现

1. 新增独立 `BOOK_TITLE` 字体 ID `0x524406`，Noto Sans CJK SC Medium（真实 500 字重）。
   12pt、36px 行高、29px ascender 与原 BODY 相同；含 6971 个实际 glyph，完整覆盖 BODY 已支持的字符。
2. 首页最近阅读、书库网格／列表的书名用新 Medium。首页主书名保留原较大 14pt Medium；
   该子集缺字时整串退到完整 12pt Medium，超出完整字库的字符仍按原规则整串退 BODY，不逐字混杂。
3. 作者／进度仍用原 caption／secondary；普通菜单、状态栏、封面模板和阅读正文字体不重新绑定。
   未添加 SyntheticBoldScope、横向描边或像素膨胀。
4. 布局测量与实际绘制使用同一 book-title 字体选择；书库为真实 caption/body/book-title 的最大行高预留空间。
   CJK 的 glyph advance 与原 Regular 一致，长书名仍走原有截断／折行与交互区域。
5. 新增一个开机字体注册节点，复用原 fontMap API；字库常量和压缩表驻 Flash。
   复用现有 FontDecompressor，不新增整字库 RAM 缓存或 framebuffer，单组仍最大 16384 B。

源字：notofonts/noto-cjk 的 `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Medium.otf`，
固定 SHA-256 `ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7`，
SIL OFL 1.1，沿用 `upstream/scripts/fonts/NotoSansSC-OFL.txt`。
生成脚本 `scripts/build_plus_book_title_font.py` 从当前 BODY interval 取完整字符集合，不读取用户书名或 SD 内容。
转换器保留 BODY 相同的 2-bit 抗锯齿（4 个字形覆盖度），在既有 16 级 UI 画面内绘制；
不冒称新字库自身含 16 级覆盖度。完整 4-bit 字库会挤占 OTA 余量，因此保留真实 Medium 和完整字符范围，未改变分区／升级安全余量。
压缩 bitmap 522606 B、67 组；逐 glyph 检查像素与压缩解码一致。其他七套字库源文件保持。
固定源仍为 2844 个 pin 文件，231 项适配；新字库通过 source record 加入汇总补丁，不强制提交 ignored snapshot。

## 验证与体验包

- 源码、八套打包字库、指定干净 IDF `9a97f6c54ec638111ce55cd36581b3c192f15207`、配置／触摸／时钟门禁通过。
- 完整 `run_plus_port_tests.sh` 退出 0，保留原环境 skip；没有跳过或改动供数并发测试。
- 真实 firmware uzlib / FontDecompressor 检查新增 6971 glyph 的所有压缩像素、指标和 Unicode；覆盖原 BODY 全字符。
- 实际字体选择函数经 ASan/UBSan：动态中文、传统字、较大 headline、emoji／字体缺失整串回退；两处首页和两处书库绘制角色分离。
- 22 个代表 CJK glyph 的栅格 coverage 总和 Medium=15154，Regular=12105；两者 advance 相同。
  这是数字字形覆盖量，**不是屏幕反射率或「实机加黑 25%」的测量**。

体验包 `RickyOS-Plus-1.2.8-ink2.bin`：5,728,256 B。
SHA-256 `ac3902314373e8a3d570bc216407c4f0d63ea474960a8afbd76421104e8f74bc`；
ELF SHA-256 `341d9ff356791c917813fa6825cdc130a6b48ff82c072e4471b5a32e87128b5c`。
芯片 32、checksum、image digest 通过；保留原 OTA 512 KiB 余量后另余 301056 B。
文件、源码差异及生成／测试副本在 `output/firmware/plus-book-title-20261025/`。
对比 PNG 从实际 firmware 解码的两套 bitmap 绘制，同字号同基线，不模拟面板光学响应。

当前未刷入、未提交、未推送、未公开发布；电脑未识别到 Plus，已请用户重新连接。
验收：首页最近阅读、继续阅读、书库网格／列表的书名字重、长书名及作者进度留白；再比较实机黑度。
