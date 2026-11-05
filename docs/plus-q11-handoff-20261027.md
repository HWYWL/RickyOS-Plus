# Plus q11 显示实验移交（2026-10-27）

当前分支：`codex/plus-memory-20261026`，正式版基线 `b9cb060e`。
仅用于 Metalio E-Ink4-Plus；不是 Pico 固件，也不是新正式发布。

## 当前接受的版本

用户在 q11 实机测试后反馈“现在还行”，并授权提交推送。
版本 `1.2.8-q11t`，普通包不带启动图案自测。

- BIN：5735072 B，SHA256 `24f122712e89d43d6200da9ec1a68214c0d1a0aa524ce6313505327fb6b42789`。
- ELF SHA256：`075c7ea2c615b28456f8f05644cbcbca42fc882ba86390bf497574b5ab2fc818`。
- IDF 必须是打补丁的 `9a97f6c54ec638111ce55cd36581b3c192f15207`；构建脚本会校验。
- 实际刷入、读回和启动均通过，观察到匹配 ELF 并进入 InxRecent，无启动异常。
- 用户接受是主观体验反馈，尚无光学仪器测量或长期多机型验收。

## 这批改动

1. 保留完整阅读 GL16 波形，恢复笔画墨色；不修改 TPS/VCOM、充电或行时钟。
2. 保留提前清除旧字的时序。q9/q10 的擦白交叠重现了白字，q11 明确设为 **0**。
3. 三种阅读动效都保留：掠页/柔弧 spatial spread4，涟漪 spread15；每像素48相。
   固定20ms实验已否定，q11 period0，沿用q6的自适应供数节奏和最低间隔。
4. 热 LUT 使用直接字节动作及增量环缓存，避免打包解码导致的供数跟不上。
   静态热表上限6144B；不是额外预留一整帧，也不把未使用尾部算作可用堆。
5. 修复原生灰阶提交漏用 FULL_REFRESH 覆盖的问题，待机唤醒清屏请求能够生效。
6. 系统白底维持实验、首页/书库真 Medium 字重、退出阅读后的清屏请求及可重建缓存释放。
7. 现场诊断受限、可选；电源/USB问题的完整调查记录随代码保存。

## 复现当前体验包

先按端口原有流程准备依赖和源快照。设置自己的补丁 IDF 路径及字体生成 Python，
不要使用此机器的绝对路径。以下从仓库根目录执行；所有参数都要显式保留：

```sh
export RICKYOS_IDF_PATH=/your/patched-esp-idf-v6.1
export RICKYOS_RESOURCE_PYTHON=/your/python-with-font-tools
unset RICKYOS_PLUS_TURN_SELFTEST
RICKYOS_PLUS_VERSION=1.2.8-q11t \
RICKYOS_PLUS_FIELD_DIAG=1 \
RICKYOS_PLUS_UI_WHITE_MAINTENANCE=1 \
RICKYOS_PLUS_READER_BLACK_RESTORE_PHASES=4 \
RICKYOS_PLUS_READER_RETAINED_BLACK_PULSE=17 \
RICKYOS_PLUS_READER_QUALITY=1 \
RICKYOS_PLUS_READER_EARLY_ERASE=1 \
RICKYOS_PLUS_READER_FINE_RIPPLE=1 \
RICKYOS_PLUS_READER_MOTION_PERIOD_US=0 \
RICKYOS_PLUS_READER_RIPPLE_SPREAD=15 \
RICKYOS_PLUS_READER_SWEEP_SPREAD=4 \
RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES=0 \
bash ports/metalio-e-ink4-plus/scripts/build_rickyos_plus.sh
```

源树含 opt-in 的历史对照代码，直接运行默认构建不等同于 q11 参数组合。
不要重新启用 `ERASE_OVERLAP_PHASES=8`，也不要照搬固定20ms或失败的打包LUT方案。
重建会改变时间戳/ELF及整包摘要；以上摘要标识本次实际验收产物。
源快照改动通过 `upstream-plus.patch` 和 `port-changes.json` 记录，不能强制加入忽略目录。
真 Medium 字库的生成器、压缩器和资源校验已一并提供；大补丁主要来自新增字库数据。

## 测试证据与边界

- q10 全量 Plus 端口测试退出0，包含并发队列、失败恢复、波形、缓存和资源检查。
- q10 启动自测32/32通过：OFF/掠页/柔弧/涟漪各8幅匹配图案，含二值/灰边和4方向。
  合成驱动均值分别961.875/1014/1027.625/1185ms；**不是实际图书总翻页延迟**。
- q11 生产代码与 q10 逐文件摘要一致，仅关闭交叠编译参数；完整构建退出0。
- q11 额外原生 ASan/UBSan 回归复现 q10 的96处旧黑擦除前缀动作不一致；
  q11 配置为0处，同时覆盖三种样式、4方向、完整像素动作、UI、纸背景及OOM。
- q11 没有重复整套 runner 或32次启动自测；不要把 q10 数据当作 q11 实机验收。
  新白字回归用例已加入后续完整 runner。
- q11 IRAM100026B / BSS77528B / data43096B；此次修正无新增帧、队列、堆池。
- q11 实机普通包读回、app外Flash、OTA选择、正确ELF及正常启动均核验；用户反馈可用。

完整历史见 [显示与内存调查](plus-display-memory-investigation-20261026.md)。
现场原始串口日志、用户图书和固件二进制保留本机，不提交到代码仓库。

## 未解决项

- 提前清旧字可能使白色过渡比交叠版稍宽；继续压白区时不能再次破坏已验证擦写顺序。
- 动效残影、墨色和实际图书速度仍需长期验证；没有证明“无残影”或“达到屏幕极限”。
- USB 等待连接问题曾重启后恢复，不能据此宣布永久修复；也没有证明存在持续内存泄漏。
- 用户UTF-16 TXT已复现编码兼容缺口，已提供UTF-8副本；**q11不含UTF-16支持**。
  另见 [TXT问题记录](txt-utf16-open-failure-20261027.md)。
