# Plus 1.2.3 后续清理 · 2026-10-20

同步基线：私有 `RickyOS-dev:codex/plus-1.2.3`，`98b6405e`。

- 字体下载的「当前来源」已由该提交移除：下载主机只保留在诊断日志，不绘制来源文案、不占布局高度、不因来源变化增加刷新。保留下载容量和取消操作。`tests/test_plus_font_source.py` 编译实际方法并用 ASan/UBSan 验证。
- RickyOS 产品不再打包或自动安装 CrossMux 用户指南。共享入口和 Plus 固定快照均已关闭；保留非产品构建的上游行为，不删除用户现有书籍、已有指南、最近阅读或标记文件。
- PlatformIO 产品构建跳过指南生成，Plus 资源脚本也移除指南打包。旧的生成文件即便存在也不被产品源码引用。
- 新增 `scripts/tests/test_product_userguide.py`，编译共享及 Plus 实际源码，验证首次启动、语言切换、反复进入首页和既有文件保留；依赖检查确认不引用生成指南或存储实现。加入 Plus 回归入口。
- 全量还原后的编译发现线上分支的性能计时引用了 `RickyPerf.h`，但固定快照没有登记该文件。补齐原始 `9ec0ae65:lib/Logging/RickyPerf.h` 并登记为 Plus 新来源文件，不引入其他分支功能；发布默认关闭计时。

验证入口（仓库根目录）：

```sh
python3 scripts/tests/test_product_userguide.py -v
cd ports/metalio-e-ink4-plus
python3 tests/test_plus_font_source.py
python3 scripts/check_rickyos_plus_sources.py
```

本地开发修复不等于正式发布。线上旧包不会因同步源码自动变化；下一次构建发布须同时带上 `98b6405e` 和本轮指南清理。真机仍需核对字体下载页，以及使用空测试卡首次启动后不出现指南。不要清空用户卡来验证，也不自动删除旧指南。

本轮结果：产品指南 4 项检查、保留的上游指南 13 项主机测试、EPUB 生成检查、完整 Plus 回归入口均通过；完整 Plus 固件编译与触控／时钟链接检查通过。ELF 不含 `bundled_user_guide` 资源符号。开发包版本 `1.2.3-dev`，4,905,664 bytes，SHA256 `c2da8a88ef094367ffe4392516173f7aae044c37293dba8228c38a3a44336a51`。未刷机、未发布、未推送；其他机型未做完整构建验证。
