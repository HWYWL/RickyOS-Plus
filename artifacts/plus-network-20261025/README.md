# Plus 网络优化验证 · 2026-10-25

基于正式 Plus 1.2.7（`e26f4277`），分支 `codex/plus-network-allocation-20261025`。
仅针对 Metalio E-Ink4 Plus / ESP32-S31；使用补丁 IDF `9a97f6c5`。
Pico、正式 OTA/网站和 SD 初始化逻辑均未修改。
最小产品修复提交为 `bd745e93`；同分支的后续提交保存默认关闭的诊断工具、回归测试和本目录记录。
其他分支只需要产品优化时可先审阅/选择合入最小修复，诊断工具不应作为正常固件开关启用。

## 产品变化

普通分配的内部内存优先阈值从 SDK 默认 16 KiB 改为 2 KiB：较大的普通对象优先使用 PSRAM，
为无线内部/DMA分配保留空间。显式 INTERNAL/DMA、TLS 分配方式、功率/省电租约、
连接协议和下载超时保持原策略。未将复制发送实验作为产品默认值。

`sdkconfig.defaults` 是受维护输入。已有旧 sdkconfig 不会自动采用新默认值；构建后的
`check_plus_web_memory.py` 会拒绝仍为 16 KiB 的缓存配置。应使用新的 SDKCONFIG 路径重新配置，
不要绕过检查或手改生成头。正常固件不设置任何 `NET_*`、`NETWORK_ACCEPTANCE` 诊断环境开关。

## 有效实测

| 场景 | 结果 | 证据 |
| --- | --- | --- |
| 原 1.2.7 正常操作 | 一次目录成功、一次 TLS 总预算超时；另四轮前三成功、第四驱动发送 NO_MEM | 原版比较 JSON、完整诊断文档 |
| r6b 运行时 2 KiB 对照 | 12/12 目录成功；实际传输中位 641 ms | `netmem-r6b-*` |
| r9 从启动采用 2 KiB | 12/12 目录成功；实际传输中位 634 ms | `netmem-r9-*` |
| r9 完整字体下载并 SD 读回 | 2/2，1,763,452 B，5355/5548 ms，CRC32 2837099957 | `netmem-r9-config-policy-weread.json` |
| 正常操作包微信读书 | 53/53 图下载；约 10 MB 书籍打包成功并进入阅读页 | `weread-clean-user-download.json` |
| 登录后 r10 书架同步及整理 | 3/3，2534/2533/2492 ms，每轮 176 条缓存可读 | `netmem-r10-authenticated-weread.json` |
| r10 详情读取 | 3/3，768/46/47 ms；后两轮命中缓存 | 同上 |
| r10 字体目录/完整字体 | 12/12 目录、2/2 完整文件读回 CRC 通过 | 同上 |

目录传输耗时不含完整 UI 进入/绘制/退出；后者单轮约 4.7 秒并含墨水屏绘制。
完整字体约 0.30–0.33 MB/s（约 290–322 KiB/s），包含连接和 SD 写入；不是 LAN 或芯片上限。
r6b/r9/r10 未记录到被包装堆失败、pbuf 分配失败或驱动 NO_MEM，不代表全部驱动错误为零。
原版故障、运行时阈值组与从启动配置组的初始状态不同，不能合为随机化故障率或长期稳定结论。

## 失败和受干扰样本

- `netauto-core-aba.json` 是核心路径，未复现真实父页问题，不能用来宣称复制发送修复。
- `netui-aba-partial.json` 受早期自动程序覆盖 pending-pop 的时序错误干扰；
  审查标记在 `netui-aba-partial-audit.json`，不能用它证明产品泄漏或复制发送优劣。
- `netmem-r4-ui-init.json` / `netmem-r5-ui-init.json` 在 Wi-Fi 初始化阶段停止，HTTP 请求数为零。
  r5 捕获 1704 B、INTERNAL|DMA|8BIT 的 `wDev_Rxbuf_Init` 分配失败。
- r9 的微信读书三次需要认证恢复，按约定在续期/登录前停止；这不是超时，也不是功能通过。
- 刷机后偶发 SD CMD8 CRC 错误也在原版出现；r10 首次启动失败，单次自动重启后恢复。
  失败启动没有算入成功样本。此问题单独继续排查，本提交不改 SD 初始化。

WPA3-only 尚不支持（当前本机 SAE 关闭），需独立启用并同时验证纯 WPA3 与混合网络。
长时阅读、多路由器、AP/STA 大文件吞吐、设备能耗上限仍未完成。

## 验证与复现

完整过程、镜像身份和实验边界见 [诊断记录](../../docs/plus-network-allocation-20261025.md)。
正常操作候选 `1.2.7-netfix` 的 SHA256：
`97e561a8c438472de64f400bfdd460cf4ef8da6d4d206fb10b26a5d8403bef7a`。
已确认无自动测试/分配包装符号，恢复后正常进入首页、SD 正常；未发布正式渠道。

主机检查（在 Plus 目录运行）：

```sh
python3 tests/test_plus_net_alloc_trace.py
python3 tests/test_plus_font_network_auto.py
python3 tests/test_plus_web_memory.py
python3 tests/test_plus_clock_radio.py
python3 tests/test_plus_index_budget.py
python3 tests/test_plus_download_cancel.py
```

默认诊断关闭；主机桩测试不能替代 S31 无线、DMA 和真实页面测试。
`scripts/summarize_plus_network_auto.py LOG --full-ui` 可在本地生成相同的数值摘要。
JSON 仅含阶段、计数、耗时和错误码；不含账号、凭据、书名、正文、IP/MAC、设备序列号。
原始串口日志、Flash 恢复点、固件二进制与设备文件保留本地，不随源码上传。
