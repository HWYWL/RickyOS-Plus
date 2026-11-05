# RickyOS Plus 1.2.4 · 2026-10-20

适用机型仅 Metalio E-Ink4-Plus（ESP32-S31 / board `metalio-plus-s31`）。
用户已确认本次网络传书和独立锁屏解锁开关正常，并明确授权正式发布。

## 发布内容

1. 改善热点／加入网络传书的网页打开与响应；空预连接上限 750 ms，已有请求与上传时限不缩短。
2. 热点探测入口跳转到设备实际 HTTP 地址；DNS 监听失败时保留 IP 直连。
3. Plus 网络通用分配优先利用 PSRAM，静态 DMA TX 池有界；保留已有 5 dBm 兼容租约、接收窗口和接收缓冲策略。
4. 新增「设置 → 系统 → 手势 → 锁屏右滑解锁」，默认关闭，独立保存。按键唤醒保留，触摸 Back 不再绕过开关。
5. 字体下载隐藏“当前来源”等排查信息，保留进度与取消；含已同步的 `98b6405e`。
6. 产品不再自动打包／安装上游用户指南，不删除用户已有书籍、指南或阅读状态。
7. 统一 CMake 应用描述和界面版本为 1.2.4，补齐固定快照遗漏的 `RickyPerf.h` 来源；正式性能诊断默认关闭。

MOBI/AZW3 和技能五子棋为此前 1.2.3 已发布功能，不重复列为新增。
不合入持续系统 Wi-Fi／显示服务独立实验，不改变 Pico 或 A4 的发布目录。

## 固件身份与验证

- 应用：4,906,992 bytes，版本 `1.2.4`，芯片 ID 32。
- BIN SHA256：`d633d920853646ccade0f171f2dbcdcc4f34406e8c91ed057c935604f951f903`。
- ELF SHA256：`a60ba2eaba332e657db0b2053af1b4d8df02b41f09e1e864d72ecc71ad3ad847`。
- 固定应用来源 `a42d939944c4495a8731b194171e365c85e5a000`；SDK `96de1be6ce08eb732909e6e8149af8f892b9a2c5`；2,844 来源文件／136 适配。
- 实际生成配置检查、完整 S31 构建、触控链接和时钟所有权检查通过；普通编译未启用 NETTEST／TCP_TRACE／RADIO_AUTOSTART。
- 最终 `tests/run_plus_port_tests.sh` 完整入口通过；网站验证工具 71/71 通过，所有当前发行镜像及安装组件校验通过。
- 同一镜像已写入用户测试机并摘要校验，app0 外全部 Flash 不变，显式复位且 ELF 匹配进入 InxRecent；用户确认滑动解锁正常。
- 网页验证过程和局限见 [网络记录](plus-web-response-20261020.md)，新开关验收见 [解锁记录](plus-standby-swipe-unlock-20261020.md)。保留早期间歇性失败，不宣传所有网络环境根治或固定提速倍数。

正式发布不重新编译应用。完整安装复用线上 1.2.3 已登记并校验的 S31 bootloader 与通用分区表，不用未上机的新启动组件替换它们。旧版应用及安装组件保留，不删除历史资源。

## 发布状态

已更新官网 `RickyOS-web:gh-pages`、设备 OTA 镜像 `RickyOS-site:gh-pages` 和 COS `ota/plus/`。
每处先上传不可变应用并核对摘要，之后才切换目录／OTA／说明。Pico 1.2.2 与 EEGO A4 1.2.5 的目录条目及 OTA 文件须逐字保持不变。
源码仅推送私有 `RickyOS-dev` 的 Plus 分支；公网站点只接收发行文件，不接收实验源码、设备备份或原始串口日志。

发布已完成：

- 私有源码提交：`6b888abf287c0fc258ef4c1a06badb98c8b8216b`；位于 `codex/plus-1.2.3`（沿用既有维护分支名），已推送。
- 私有发行标签：`plus-v1.2.4`，指向上述源码提交，不含设备备份或日志。
- 官网资源提交 `5cf4122`，启用提交 `36e5cd9b46338ff08eb589868441a5feb268ac92`。
- GitHub OTA 资源提交 `3a2b2d8`，启用提交 `b0c2d6aeb2a426965cdc353e9d9070be03ccb2e2`。
- COS 先上传不可变应用并进行公网摘要核对，再上传说明、最后上传 Plus OTA 清单；未触及其他机型路径。
- 官网入口：[安装](https://rickyaistudio.com/install.html)，选择 Metalio E-Ink4-Plus。
- 在线更新清单：[官网](https://rickyaistudio.com/plus/ota.json)、[GitHub 镜像](https://chinoryunqin.github.io/RickyOS-site/plus/ota.json)、[COS](https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/ota/plus/ota.json)。三处均为 1.2.4，文件长度／SHA256／审批字段一致。
- 公网完整下载三处应用并核对同一 SHA256；20 个目录／说明／版本页面检查均匹配本地发布内容。最终一批 23 个目标初次有 22 个成功，COS 应用下载一次被对端重置；仅对此失败项单独重取后完整长度与 SHA256 通过。初次失败和重核报告均保留，未覆盖或隐去该网络异常。
- Pico 1.2.2、A4 1.2.5 的目录条目及三处 OTA／说明保持原值；未删除历史二进制、`.nojekyll`、README 或其他站点文件。关于页只改 Plus 版本文字，不顺带改其他机型的既有展示。

操作与公网复核结果保存在本地 `.local/release-plus124/`，公开摘要见下述发布记录。本轮未再次刷写测试机；正式应用与用户已经验收的候选逐字一致。未以主机测试替代新的真实设备 OTA 写入验证。
