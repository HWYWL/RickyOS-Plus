# Plus 整合 design d091adbe

2026-10-14。来源 `chinoryunqin/RickyOS-dev:codex/rickyos-1.2-design`，固定提交 `d091adbef9f03a54cdc9e0ca0708cdd090aee6f2`；SDK 仍固定 `96de1be6ce08eb732909e6e8149af8f892b9a2c5`。初次整合来源清单包含 2824 个文件、76 项端口差异；应用页修正版为 77 项差异。整合前源码备份：`.local/plus-before-d091adbe.tar.gz`。用户已要求提交到设备专用分支 `codex/metalio-e-ink4-plus`。

## 新增内容与 Plus 适配

从此前的 `73901ae8` 补入五个提交：阅读菜单集中调整字号、字重和阅读线（727a2804）；产品 USB 传书接入（ddc5b45b）；更新后的开机书库与封面准备（7a7c8d81）；USB I/O 重试、SCSI 兼容与返回退出（1bb36c43）；小狗开机准备文案（d091adbe）。Read Pico 的 GPIO、屏电源和 USB 后端没有移植到 Plus。

重新检查优化的设备条件后，Plus 接入原先仅对 Read Pico 开启的相邻页预渲染与小章节预取。依据 `upstream/src/activities/reader/EpubReaderActivity.cpp:2316` 的缓存入口和 `:2855` 的绘制分支，Plus 使用自己的原生 16 级灰阶能力判断，原生灰阶渲染跳过此前重复的整页黑白正文绘制，仍更新状态栏、阅读线和既有刷新基准。没有套用 Read Pico 专用的 combined-base 或退出清屏策略。

前后各一页缓存，在原生 1216×684 分辨率下共 1247616 字节（约 1.19 MiB）：每槽保存 4 位灰阶画布和两份黑白代理，不分配未使用的 LSB/MSB 平面。使用受预算控制、失败可回退的 PSRAM；不能放进 8 KiB 任务栈，全局静态分配也无法在退出阅读时回收。菜单、切书、字体变更、取消、灰阶/反色条件或内存不足时按原路径绘制；小章节预取仍保留 24 KiB 上限和剩余内存门槛。主机验证缓存机制不能代替实机翻页耗时测量。

开机准备接入 Plus 显式 CMake 源码列表。`upstream/src/util/BootPreparation.cpp:34` 与 `upstream/lib/hal/HalSystem.cpp:97` 使用 HAL 提供的 ELF 哈希前 8 字节和版本组成有界标记；即使连续开发镜像的版本同为 `1.2.0-plus-dev`，更新仍会重新执行所需准备。同一镜像不会反复准备。其他目标的标记行为保留。

USB 继续使用 Plus 的 S31 高速后端。存储块设备采用至多四次 I/O 尝试并记录统计；保留同一 SDMMC 实例的失败重挂载。MODE SENSE(10) 返回有界可写介质头，SYNC CACHE(10)/(16) 调用真正的介质同步。通用包装器在 Plus 上转发真实后端，不以空成功代替落盘。传书连接期间返回退出沿用新的活动流程。

保留已适配的前光/音量控制、待机关灯、按键和震动设置、录音机准备提示、灰阶刷新与末行 DMA 修正、厂商白边 0、电量和 RTC。保留字体下载来源显示、已连接 Wi-Fi 复用、字节进度、目录等待预算及静态待机抑制重绘。此前网络不稳定的问题没有以本次合并宣称修复。

## 构建与验证

固件：`firmware/rickyos-plus-1.2/dist/RickyOS-1.2.0-plus-design-d091adbe-optimizations.bin`。

- 镜像大小：5235360 字节；既有 5 MiB ota_1 剩余 7520 字节，按擦除边界剩余 4096 字节。
- SHA256：`e1cb4790c260990c345441cdcf2c4bab649a31588362ddbcac22092ab90f97ee`。
- ELF SHA256：`20fd1a9c6fe48693f9aff06a55629253cecabc1448aa15143d58aa77cc6baca6`。
- 完整构建与镜像 checksum/hash 校验通过；12683 个打包字形的解码/像素校验通过。
- 完整 Plus 回归通过，包含实际缓存方法的前后页、原生像素/代理恢复、取消重试、六处分配失败清理、预算和条件回退；实际块设备重试与 SCSI 同步测试通过。
- 上游阅读缓存基础 4 项、阅读 UI/设置/统计/资源路线 38 项通过。源码从固定 Git 对象和补丁在新目录还原，2824 文件/76 差异校验通过。

日志：`logs/plus-design-d091-build-resume.log`、`logs/plus-design-d091-image-info.log`、`logs/plus-design-d091-tests.log`、`logs/plus-design-d091-reader-cache-base-tests.log`、`logs/plus-design-d091-ui-tests.log`、`logs/plus-design-d091-restore.log`。

首次构建因本机磁盘空间耗尽中止，清理仅本任务可再生成的临时源码还原目录后，用两个并行编译任务完成。没有删产品功能、裁剪字库或扩大原厂分区。

## 实机状态

固件已受控写入 ota_1，应用内容校验通过，再选择启动槽；原厂 2.0.62 槽、启动程序和分区预检一致，应用写入后原厂槽 MD5 仍一致。日志确认 ELF `20fd1a9c6…`、`metalio_plus`、16 MiB PSRAM 及内存检测通过；开机准备正确识别新镜像，进入 InxRecent，首次完整/区域刷新结果均为 0。日志见 `logs/plus-design-d091-preflight.log`、`logs/plus-design-d091-install.log` 和 `logs/plus-design-d091-boot.log`。

随后日志已记录进入 EpubReader、多次成功正文刷新和进入 EpubReaderMenu。它们证明活动与刷新已执行，不等于用户确认视觉效果或缓存命中；连续前后翻页、字号/字重调整和 USB 连接退出仍需用户验收，缓存命中耗时未单独实测。当前记录不把以前镜像的用户验收当作本轮验证。

安装准备时原先端口出现另一台 ESP32-S3；芯片和 MAC 均不符合已固定的 Plus S31 身份，未执行分区或应用写入。已记录在 `logs/plus-design-d091-connect.log` 与 `logs/plus-design-d091-port-identity.log`。端口名称不能作为设备身份，后续须重新核对 S31 与 Plus MAC，再进行原厂槽预检。

用户重新连接后，通过 USB 序列号和下载器 MAC 核对已确认是目标 Plus。发现另一个只读 Pico 采集程序 `cap.py` 占用同一串口，已停止该采集并在本地保存重启参数，再由本轮单一采集进程记录启动与阅读。`plus-design-d091-connect.log` 此时记录正确的 S31 连接，先前 S3 身份单独保留在 `plus-design-d091-port-identity.log`。

用户对阅读检查回复「还可以吧」，记为基本阅读可用；没有将其扩展为每一项字体设置或缓存速度的确认。180 秒启动/交互日志采集已结束，串口释放。

## Plus 应用页完整高度修正

用户反馈应用页只占半屏，屏保需要翻页才能找到。原因是 `AppsMenuActivity.cpp:48` 固定每页六个入口，`:212` 固定内容区最大 448 像素高，而 Plus 增加录音机后有七个入口。

Plus 应用网格/列表现在使用 `pageContentRect()` 扣除标题和页边距后的完整可用高度。网格仍为设计规定的三列，按图标、名称和选中面所需最小高度决定可容纳的行数；当前竖屏七个应用占三行，屏保在同一页。横屏/受限高度只有放不下时才分页。绘制、点选、分页起点和滑动均使用同一页容量与 `RickyPageLayout::cell()`，栏距保留 20 像素；没有新增堆分配。一页能放下时不画滚动条，上下滑动不移动焦点或触发无效刷新。其他设备的六格/448 像素行为保留。

构建通过。既有应用图标/目录测试中，目录编译夹具缺少 `<algorithm>`，且仍将稳定应用 ID 数写为录音机加入前的 17；已补齐夹具并同时编译普通产品六入口、Plus 七入口两种配置，所有 18 位隐藏掩码继续验证稳定映射。6 项现有测试完成：5 项通过，1 项图标重生成因缺少 Node sharp 跳过，图标资源/路由检查通过。源清单仍为 2824 个固定文件，端口差异增加至 77 项。

新候选 `RickyOS-1.2.0-plus-design-d091adbe-apps-fullscreen.bin`：5235456 字节，SHA256 `e86755f8a03a6e23ff1b9101c536477b3623cd7b5f080f679b8570e3c00498ec`，ELF `9cc1b12015ac27013f2040801c0207e653878ee849de4835afda62316b319c0c`。原 ota_1 剩余 7424 字节，按擦除边界仍剩余 4096 字节。镜像 checksum/hash 有效。日志：`logs/plus-apps-fullscreen-build.log`、`logs/plus-apps-fullscreen-tests.log`、`logs/plus-apps-fullscreen-image-info.log`。应用页视觉与屏保点选等待新镜像启动后确认。

布局修正版已受控写入并校验 ota_1，原厂 2.0.62 槽 MD5 保持一致。启动日志确认 ELF `9cc1b1201…`、新镜像准备标记、InxRecent 和成功完整/区域刷新。记录于 `logs/plus-apps-fullscreen-preflight.log`、`logs/plus-apps-fullscreen-install.log`、`logs/plus-apps-fullscreen-boot.log`。应用页视觉与屏保点选仍等待用户反馈。

2026-10-14 用户回复「可以的」，确认七入口同页、下方空间利用和屏保入口点选符合预期。此反馈用于应用页实机验收，不扩大为本轮 USB 传输或缓存速度验收。

提交前补齐既有录音机 UI 测试对新版应用网格的夹具：直接执行生产页容量、分页起点和点击方法，覆盖 1–20 个入口、完整/受限高度、横竖宽度、空格与栏距拒绝点击；明确验证竖屏七入口同页，受限高度仍允许分页。录音机的准备提示、播放、取消、待机、继承按键和分页检查保留，ASan/UBSan 通过。该调整只修改测试，不改变已刷入固件。

提交包验证：从固定 Git 对象新还原 2824 文件并应用 77 项端口差异，散列与实机源码一致；重新生成资源后完整 Plus 回归通过，应用图标/目录检查 5 项通过、1 项重生成依赖跳过。时钟编译标志检查使用已验证的实际 S31 构建数据库，按键持久化测试使用现有固定 ArduinoJson 依赖；未宣称在新环境重新完成整套固件编译。日志为 `logs/plus-publish-d091-restore.log`、`logs/plus-publish-d091-tests.log`、`logs/plus-publish-d091-apps-tests.log`。仅提交专用端口源码、补丁、测试和文档，生成资源、固件二进制、备份及原始日志保持本地。
