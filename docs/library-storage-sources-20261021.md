# Plus：可控收录与手动分类（未刷机体验版）

工作分支 `codex/plus-library-storage-sources`，基线 `21382423`。
共享功能来自独立小纸工作区 `codex/library-storage-sources`，没有合并整条设备分支或升级 SDK。

## 功能

- 用户指定单本或文件夹收录，可选子目录；进书库／存储不自动全盘扫描。
- 移出书库与物理删除分离，排除记录可抵抗刷新重加。
- 翻页不解析 EPUB 生成封面，不为未读书计算内容哈希。
- 手动分类与全部／在读／未读独立；可多分类、批量跨页归类、重命名、删除分类。
- 分类删除不删书，返回可取消未保存的归类。
- 16 个分类、4096 本索引上限；当前文件浏览器仍缓存当前目录名称，不能据此承诺万级单层目录不卡。

## Plus 持久适配

实际应用源码位于 `firmware/rickyos-plus-1.2/upstream/`。
新增来源／分类模块已在 `components/rickyos_application/CMakeLists.txt` 显式注册，
通过 `scripts/sync_plus_design.py --record` 记录到 `upstream-plus.patch` 与 `port-changes.json`。
原固定来源 `a42d9399`（2844 文件）不变，现记录 149 项适配。

预览：`scripts/prepare_plus_library_preview.py --shared /Users/rick/rickyos/wt-library-storage`，
运行真实 Plus 应用和原有桌面适配，逐项核对 17 个功能输入。
地址 http://127.0.0.1:8791/，并非 S31 外设、波形或 SD 时延仿真。

固件构建目录 `.local/build-library-storage`，App 4,923,952 B，
SHA256 `604dccf33419a34e757c309f06f5089a933bb57ecf7b8888dd3cccb17dd39faa`。
IDF 构建、固定源校验和链接后的触摸／时钟／网络内存检查通过。
已有端口回归通过，日志 `/tmp/rickyos-plus-library-tests.log`。
生成的统一差异补丁含有语法要求的空白上下文行；不要为消除补丁文件的 Git 行尾告警而剥离这些上下文前缀。

按用户要求交付至私有仓库 `chinoryunqin/RickyOS-dev` 的 `codex/plus-library-storage-sources` 实验分支。
没有合并正式版、刷机或发布；没有修改其他工作区的资源迁移或服务隔离实验。

## 大书库模拟数据

私有模拟卡已加入同批 3000 本合成读物（EPUB/TXT/MD 各 1000），
2000 本为单层目录，另 1000 本分成 20 组。通过固件本身刷新收录，
最终 3006 本／501 页（本端原有 6 本），1000 次 EPUB 元数据解析、0 本不可读。
桌面冷导入日志为 2484 ms，仅代表宿主文件系统，不代表 S31 真机 SD 卡速度。
未直接伪造索引，固件 App 与上方摘要不变。
