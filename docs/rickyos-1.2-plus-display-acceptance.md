# RickyOS 1.2 Plus 显示验收记录

2026-10-11，首个完整候选安装于 ota_1，原厂槽、分区表和启动程序保留。日志进入 LanguageSelect，收到触摸滑动事件；这不能替代实物显示和触控位置验收。

用户照片 `/Users/rick/Downloads/IMG_1864.HEIC` 显示 Language/English/简体中文与原厂 Metalio 页面叠加。显示输出已到达面板，首次旧画面清除未通过。

原因：显示适配器首次使用 `epd_hl_update_screen`，其 back_fb 初始化为白色，但实际面板保留原厂图案；差分计算遗漏逻辑白色区域中的旧图案。厂商 `main/display/epd_refresh.cc` 的 Draw 方法先调用 `epd_clear_checked`，成功后使用 `epd_hl_update_screen_from_white`。

修正：未知光学状态先物理清屏，成功后建立白屏基线；已知状态的全刷调用 full-area 更新；快速更新保留差分路径。黑白和原生 16 灰度共用此路径，电源检查失败或清屏失败时保留 Unknown，结束时关闭面板高压轨。

修正版 5,128,816 字节，SHA-256 `b24e988ef3e2ef4131233045136014b7c1c1105575a5663f1b405d3baee2563b`。完整构建及 S31 镜像校验通过，上机清屏和菜单触控仍待验收。

修正版已写入 ota_1 并读后校验，原厂槽 MD5 未变。重启日志确认 ELF 前缀 54cf8d113，进入 InxRecent，读取已保存语言设置并加载 SD EPUB `/CrossMux用户手册.epub`。实物清屏、触控和翻页待用户确认。

用户第二张照片 `/Users/rick/Downloads/IMG_1866.HEIC` 显示主页与 EPUB 正文叠加。首次清屏补丁未解决页面切换，显示验收仍未通过。新增修正：Full 刷新及 Gray 基线离开时执行物理清屏，并从白屏执行 GC16；黑白快速更新仍使用差分 DU。每次刷新记录模式、清屏决策、旧状态和结果，便于对照实物。

远程同步：用户确认目标为 `https://github.com/chinoryunqin/RickyOS-dev.git` 的 `rickyos-1.2`；2026-10-11 fetch 与 ls-remote 均确认最新提交仍为 `f201efa76ad5a2cc5c78f85883cf8204d16d0d24`，与当前移植基线一致，没有新增源码可合并。

2026-10-11，已同步远程 `codex/rickyos-1.2-design` 的 `185bfe78004092174dcbb4e668e3697c4dee7e88`，触控适配修正版已安装到 ota_1。当前候选 SHA-256 `d78e6ce5446b4ffd467e02d47fe386cb3928ebc5801d6436f1bc6ecf2f07ea5f`。

用户照片 `/Users/rick/Downloads/IMG_1867.HEIC` 显示“索引失败：SD 卡读写出错（E3）”，并未显示书籍正文。本轮启动日志 `logs/rickyos-1.2-plus-design-touch-fix-boot.log` 显示打开的是 `/metalio/e-ink/wifi.txt`（24 字节）；TXT 章节索引和 book.bin 写入成功，随后 TxtToHtml 转换连续三次返回失败，Section 构建以 error=3 结束。当前错误日志合并了输入、索引映射和输出原因，尚不能确定底层 SD I/O 故障。未读取或记录该配置文件内容，未删除缓存、格式化 SD 或修改书籍文件。

该照片的错误弹窗背景没有明显旧主页叠影，但不能据此判定阅读正文刷新通过。已请求用户打开实际 EPUB 并检查正文及翻页；正文验收、TXT 失败原因仍待确认。补充串口采集在 `logs/rickyos-1.2-plus-reader-photo-1867.log`，连接时出现一次设备重启，未写入固件。

用户照片 `/Users/rick/Downloads/IMG_1868.HEIC` 显示 CrossMux 用户手册第 10 章正文与主页完整叠加。补充日志确认 EPUB 索引、排版和 16 灰度渲染成功，GC16 全刷返回 0，因此阅读显示仍未通过。

定位：`MetalioPlusDriver::beginGrayscale16()` 直接返回保留上一帧主页的 `hl->front_fb`，没有执行原生灰度 begin 的清空约定。`GfxRenderer::beginGrayscale16()` 清除 B/W 代理；除屏幕捕获模式外并不负责清除本机灰度画布。阅读器随后仅写正文笔画和状态栏，于是旧主页仍存在于送屏数据，物理清屏后也会被重新绘制。修正为 Plus 原生灰度 begin 在现有 PSRAM 缓冲区上填充白色，不新增分配。

回归测试先在原生尺寸画布中放置旧黑白图案，再执行灰度初始化，逐像素检查全白、边界哨兵以及新增黑/灰笔画不保留旧图案；字体像素、指标与 Unicode 共 12,578 字形复核通过。修正版 5,201,248 字节，SHA-256 `687cfaf5cc3ad9668b8bbb8a4a5923d3aa2f24a84844ccf7807df4865a2f7223`。构建、镜像校验、触控后端链接检查通过，实物页面切换和翻页待验证。

灰度画布修正版已写入 ota_1 并校验，原厂槽 MD5 未变；重启日志确认 ELF `2e8c575e7`，进入 InxRecent，EPUB 元数据读取成功，无共享触控总线初始化错误。启动日志 `logs/rickyos-1.2-plus-gray-clear-boot.log`。已请求用户再次打开正文并连续翻页，实物结果待确认。

用户确认灰度画布修正版正文“不叠了”，主页叠加问题通过本轮实物验证。新增反馈：每次按键闪多次，底部三个盖板按键无响应。

原因与修正：Plus 灰度提交此前不接收阅读器 setNextGray16Profile 周期，而且每次灰度提交和离开 Gray 状态都执行三轮物理黑白清屏。现接入一次性刷新配置：普通灰度翻页使用厂商 GL16 差分更新，周期性全刷使用 GC16 全区差分；仅光学状态 Unknown（首次启动或失败后）执行物理清屏与 from-white 初始化。Gray 转 B/W 使用 GC16 全区更新保留旧灰度基线，不再一律多轮清屏。

底部盖板三键使用厂商原始 FT6336U 坐标 Y=1600±30，X=100/200/300±30，在屏内坐标校验前解码。主页键接入现有 HomeButtonInput 的短按/双击/长按设置（默认短按回主页）；其余两键接入 BTN_LEFT/BTN_RIGHT 按下及松开事件。盖板接触保持原始键直到释放，滑动不切换键、不产生屏幕点击；释放事件只产生一次，主页长按不再附带短按。三键命中与屏内/边界拒绝测试通过；新增功能实物验收待完成。

盖板键与刷新周期修正版已写入 ota_1，并验证读后 MD5 和原厂槽不变。候选 5,201,488 字节，SHA-256 `d12aa3996740b61ad234d6a96bd626d07f25067189a1c6a6dd8dfd6a339b270a`，ELF 前缀 `56bb39b54`。启动采集 `logs/rickyos-1.2-plus-keys-refresh-boot.log`。实物盖板键、差分翻页和周期全刷待确认。

新增用户反馈：电量始终显示 0。定位为 Plus 板级配置遗漏 FREEINK_BATTERY_I2C_GAUGE，BQ27220 路径被编译关闭，且 Plus 没有 ADC 电池引脚，百分比读取退回默认 0。修正启用 Plus 计量能力，BQ27220 的 16 位寄存器读取经过已有 Wire1 共享传输和 metalio_i2c1 锁，不另行初始化 I2C 控制器。

Plus 百分比沿用厂商电池电压端点（3300 mV=0%、4330 mV=100%）线性估算，区别于 X4 ADC 电压曲线；并非库仑计校准后的 SOC。2500–5000 mV 范围之外的读数拒绝为无效，失败不填充有效百分比。单调性、范围、端点、中点和非法读数回归通过，未增加动态内存分配；实物电压及 UI 电量待验证。

电池计量修正版已安装并通过应用读后校验、原厂槽一致性检查。镜像 5,202,576 字节，SHA-256 `efe74774b0dba54903855195c31d7dc37244b70e55218178d8cbc3aea76480a1`；启动 ELF `a24f89fdf`。设备实际 BQ27220 电压读取 4361 mV，估算百分比 100%，启动到 InxRecent，屏幕与触控共享总线初始化正常；UI 读数待用户确认。日志 `logs/rickyos-1.2-plus-battery-boot.log`。

用户确认电池计量修正版主页显示 100%，与设备日志的估算百分比一致；本轮电量读取和 UI 更新验证通过。电量随放电变化、盖板三键以及普通差分翻页闪动情况仍待验证。

用户随后确认底部三个盖板按键和普通翻页刷新“也确认正常了”。本轮已通过：正文无主页叠加、盖板主页/上一页/下一页操作、普通翻页刷新、电量读取及 UI 显示。该结果仅覆盖用户已验证的操作；TXT 配置文件索引 E3、长期放电电量变化、其他外围功能仍不据此宣称通过。

用户反馈：Wi-Fi 已连接，手动时钟同步失败；粗糙主要位于主页和菜单。诊断日志 `logs/rickyos-1.2-plus-clock-diagnosis.log` 证实设备取得 IP，SNTP 启动后约 10 秒超时。现有手动同步期限 10 秒，短于 lwIP 单服务器 15 秒接收超时，且 IDF 有最多 5 秒随机启动延迟，可能尚未尝试备用服务器就报失败。该证据说明等待策略不足，尚不能确认首个服务器未响应的网络原因。

修正：ClockSyncActivity 改为异步启动及轮询 HalClock，最多等待 55 秒以覆盖三个服务器接收期限和启动延迟，主循环仍可处理输入、完成系统时钟及 RTC 写入；同步期间允许返回取消。Plus RTC 能力启用 PCF8563 @0x51，读写全部使用已有共享 Wire1 传输及总线锁，时间写入后由 HalClock 读回验证。

灰度修正：主页 InxRecentActivity 和 UiListActivity 的 wantsGraySettle opt-in 原先仅 Read Pico 启用，导致 Plus 主页及列表仍为 B/W；现扩大到 Plus，走既有 ActivityManager 前台原生 16 灰度事务和周期刷新，不额外分配帧缓冲。直接灰度 4 项回归通过，完整构建、21 项来源适配校验和链接触控检查通过；实物灰度质量、RTC 和网络同步待确认。

时钟与界面灰度候选已写入 ota_1 并校验，原厂槽保持一致。镜像 5,204,304 字节，SHA-256 `0c7636805b31ab5f2edbdebd6dc2721cd84a61ab4a0809e9353e0f8e94752cbf`；启动 ELF `ef257a5b8`。日志确认 External RTC found、主页 Direct gray: done、GL16 mode=5 result=0。RTC 原时间没有恢复成功，是否初始时间无效待后续同步及读回日志判断；网络同步及实物灰度质量待用户重试。

### 2026-10-11：校时仍失败后的单一客户端修正

用户反馈 clock-gray 版本手动同步仍失败，尚未反馈主页灰阶观感。上一轮采集在手动 55 秒窗口结束前停止，因此不能据该日志断定最终网络失败原因。

代码核对发现：WiFiSelectionActivity 在发现 RTC 后先阻塞同步 10 秒；TrustedTime 另外注册全局 SNTP 回调、用 configTzTime 重启同一客户端，其 syncNow 还两次读取会自动清零的 COMPLETED 状态。Plus 现改为由 HalClock 唯一管理客户端；TrustedTime 的连接触发和限时同步均委托 HalClock，完成后持久化借阅时间下限。WiFi 连接不再阻塞等待校时。HalClock 使用 esp_netif 同步信号量判定完成，增加全球服务器组的第三个 Aliyun 备用及每 15 秒服务器地址/可达性日志。新增代码使用栈变量和已有状态字段，没有新增应用堆分配。

实际 TrustedTime.cpp 的主机回归测试通过：禁止旧 NTP API，验证初始化不替换回调、连接请求委托、限时参数和成功/失败结果传递。固件构建及 Plus 触控链接检查通过。clock-owner 候选 5,205,456 字节，SHA256 b527582a16591db235dbbd5ef227a1222405527d4ff4b5b11e976f55ed0598dd，备用槽余 37,424 字节。硬件同步结果待核验，以上客户端冲突修复不等于已证明网络失败根因。

clock-owner 实机仍在页面 55 秒窗口内失败，但后台在启动后 310,409 ms 收到 UTC epoch=1791692750（对应实际 UTC 2026-10-11 04:25:50），310,413 ms PCF8563 写入读回验证通过。校时服务持续等待后可以成功；具体服务器/网络延迟原因尚未确定。第一次 owner 修正的条件宏误写，WiFi 仍阻塞 10 秒，已改成实际编译选择 FREEINK_DEVICE_METALIO_PLUS，并补齐 BoardConfig 头。clock-trace 版本增加 lwIP SNTP 原生调试；真实对象文件 nm 检查确认 TrustedTime 无 configTzTime/旧回调/旧状态轮询调用，WiFiSelection 无 HalClock::syncNow 调用，均使用 HalClock::requestSync。构建保护检查通过，触控链接通过。clock-trace 候选 5,211,536 字节，SHA256 b975afd88c2b5ebae88da4b80266af5ed1cd4ab793371a6448063f034ffe2ff2，槽剩余 31,344 字节。

clock-trace 已刷入并读回校验，原厂槽未变化。启动 ELF b9c36a6ce，PCF8563 检出并成功恢复系统 UTC 时间，主页直接灰阶绘制完成。手动 NTP 复测正在采集，未宣称时钟同步问题解决。

clock-trace 原生 SNTP 日志确认：Aliyun 首次解析成功并发包但无有效回复，接着 Tencent 和 cn.pool DNS 解析失败，客户端循环回 Aliyun。后续甚至 Aliyun 解析也失败。设备 WiFi 加入后的旧 10 秒阻塞已消除；问题进一步定位为设备 DNS 不稳定以及首选服务器未回复，不是 RTC 未写入。为避免整轮依赖 DNS，China 组改为优先 119.28.183.184，随后 ntp.aliyun.com、ntp.tencent.com；Global 组不受此顺序修改影响。地址来自 NTP Pool 腾讯服务器列表 https://www.ntppool.org/scores/119.28.183.184 ，腾讯官方公网 NTP 文档 https://cloud.tencent.com/document/product/213/30392 。主机直接 UDP 请求取得 48 字节、stratum=2 的当前时间回复，仅代表主机验证，设备验证待完成。固定地址如未来变更仍有两个域名备用。clock-fallback 候选 5,211,536 字节，SHA256 b6c7ec0140980608c6885efb21be2051bfdac26dad412c27bde04ebbad12d885；余量 31,344 字节。保留底层 NTP 调试以验证设备首次响应。

clock-fallback 在用户当前 WiFi 下仍手动失败，原生日志显示已向数字地址发包但未得到有效回复，说明 DNS 故障不是全部原因。WiFi 日志确认 power-save type=1。对照现有 src/network/WifiPowerSaveGuard.h 的收包停顿处理，clock-radio 现临时关闭 Plus 无线省电；成功、取消、启动失败、限时等待失败、断网均恢复先前模式，后台交换 60 秒上限，手动 55 秒失败/退出会取消。HalClock 的真实主机测试覆盖以上生命周期以及原本已关闭省电的调用方，验证不会擅自恢复到省电。此修正仍是待硬件验证的原因排查，不宣称省电已证明是根因。

clock-radio 候选 5,212,112 字节，SHA256 f932cd492de16ec0ec65c4911477cfd9a9eed2c3673550904ce218f2974c8fba，剩余 30,768 字节。构建、Plus 触控与校时调用对象检查通过。用户回答当前使用 WiFi，尚未进行手机热点对照。

clock-radio 已安装，ELF c033d125e，RTC 重启恢复通过。实机日志在 71,696 ms 显示关闭无线省电 previous=1，向数字 Tencent 和解析出的 Aliyun 发包仍无有效回复；127,364 ms 手动窗口超时，127,365 ms 省电恢复 result=0。用户当前 WiFi 的 DNS 和 NTP 响应问题仍存在，不能宣称已解决或直接归因路由器。已请求手机热点对照，暂不继续刷固件。主页/菜单灰阶观感仍未由用户确认。

用户暂时无法切换网络，并认为当前网络无问题。保持当前网络继续排查，不能把无回复直接归因路由器。clock-https 增加现有 wolfSSL + OTA 站点信任根的备用校时，NTP 持续 20 秒未完成且已有可信运行时间时，向固件已有的 https://chinoryunqin.github.io/RickyOS-site/ota.json 发验证证书的 GET；不跟随重定向，流式丢弃正文（上限 4 KiB），12 秒中止窗口，不额外引入 TLS 栈、任务或应用 new/malloc。现有 TLS/HTTP 库内部连接和头部缓冲为这一次临时请求持有，栈上客户端析构释放。Date GMT + Age 缓存秒数转换 UTC，Age 限 3600 秒，支持月份/闰年/星期/非法输入校验；缓存时间含义依据 RFC9111 4.2.3 https://www.rfc-editor.org/rfc/rfc9111.html#section-4.2.3 。该方式用于分钟级设备时钟，不宣称具有 NTP 毫秒精度。若 HTTP 期间 SNTP 已完成，优先使用 NTP；成功后统一写入/读回 RTC、持久化时间下限、恢复无线省电。首次运行完全没有有效 RTC/NVS 时间时不跳过证书有效期校验，HTTPS 备用不可自举。

主机测试用同一 OTA 根证书验证连接成功；Date 解析测试覆盖九年每日往返、Age 导致跨日、闰日、无效日期和当前 TZ 不被改写；现有 radio 生命周期回归通过。clock-https 编译/触控/真实对象校时归属检查通过；5,214,880 字节，SHA256 8eab67946ffc37448226a88ddaf2baef6a72c82f95ab0253d5fcd9f700ff2a19，备用槽余 28,000 字节。上机 HTTPS 校时结果待验证。

## 厂商白边建议（2026-10-13）

根据用户提供的厂商截图，将板级 `DISPLAY_WHITE_BORDER_PIXELS` 从 8 改为 0，`epd_lcd_reinit()` 的 `.white_border_pixels` 改为引用该宏。当前 `epd_filter_white_border()` 在 border<=0 时直接返回，不再对四边输出强制 hold/whitening。保留四个 `DISPLAY_CONTENT_*_INSET=8`，未改 UI 布局留白、触控坐标、波形、VCOM 或行时序。此变更用于按厂商建议恢复边缘正常扫描，不据此声称改善中央灰阶或闪烁。

原有区域显示 ASan/UBSan 回归通过；构建及上机状态见固件 manifest。边缘显示体验待用户确认。

白边 0 版已写入 ota_1，镜像 SHA256 bafd694a…，原厂槽／写入校验通过；ELF 08b63b37…，完整和区域显示刷新 result=0x0。串口采集已结束。四周边缘与切页体验待用户验收，日志 `logs/plus-border-zero-*.log`。
