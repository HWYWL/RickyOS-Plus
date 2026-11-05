# Read Pico USB 传书实验构建

这是独立于正式 `rickyos_readpico` 的实验环境，仅通过 macOS 单机小文件真机验证，完整验收尚未完成，**不能直接发布或用于用户设备 OTA**。

## 构建与边界

```sh
pio run -e rickyos_readpico_usb
```

产物为 `.pio/build/rickyos_readpico_usb/firmware.bin`。该环境沿用 Read Pico 的自编 Arduino/ESP-IDF 核心、分区表、USB Serial/JTAG 控制台、显示缓存与 PSRAM 参数；只为 USB OTG 增加 TinyUSB MSC。它单独启用与其他 S3 USB 机型相同的应用 LTO，不改变正式 `rickyos_readpico` 构建。构建前脚本从 Espressif 的 `esp32-arduino-lib-builder` 和 TinyUSB 仓库取固定提交，并校验组件文件哈希。首次构建需要网络；依赖放在被忽略的 `.pio/rickyos_usb_components/`，只加载进 USB 实验构建，不修改 `freeink-sdk` 子模块。

现有的“文件传输 → USB 磁盘”入口会在启用 MSC 后出现。进入时，存储 HAL 先卸载设备侧 SD 文件系统，再把底层块设备交给电脑，避免两边同时写卡；电脑安全弹出后重启回首页并重新扫描文件。书籍应放在 SD 卡 `/books` 文件夹；其他文件夹仍可通过“存储”管理。

## 模拟器预览与其他设备移植

`simulator_rickyos` 单独定义 `RICKYOS_USB_PREVIEW`，在“应用 → 文件传输”显示“USB 传书 · 模拟预览”。这只用于查看入口和说明页；预览 Activity **不调用** `Storage.beginUsbDrive()`／`endUsbDrive()`，不会把电脑磁盘或演示 SD 卡交给 USB 主机，也不模拟传输成功。普通 `rickyos_readpico` 未启用 MSC，仍不显示真机 USB 入口。

其他设备分支可参考共用的 `NetworkModeSelectionActivity`、`UsbDriveActivity`、`HalStorage` 与 SDK 的 `UsbMassStorage`／`SDCardManager` 交接流程，但必须分别确认 USB OTG 接口、物理线材、SD 块设备类型、供电及主机弹出行为；不能只复制 `FREEINK_CAP_USB_MSC` 构建标志。Metalio E-Ink4-Plus 和 EEGO A4 均未在这次构建或验收。

## 真机验收清单

在保留当前正式固件与 SD 卡备份的前提下，先在单台测试机完成：

1. 插入有数据传输能力的 USB 线，从设备进入“文件传输 → USB 磁盘”，确认 macOS 与 Windows 均能识别 SD 卡，而不是仅识别串口或内部 Flash。
2. 向 `/books` 拷入 EPUB、TXT、MD 各一份，并拷入一份较大的文件；电脑端读取回传、比较哈希，检查写入是否完整。不要在拷贝中途拔线。
3. 在电脑上安全弹出，确认设备返回首页、书库能识别新书、阅读进度及原有 SD 文件未损坏。
4. 分别测试连接中拔线、空卡/无卡、主机睡眠、重复连接/弹出、重启后的 USB 串口与正常充电。
5. 回归阅读、Wi-Fi 传书、字体管理、待机与唤醒；记录最低内部堆、最大可分配块、错误日志和固件体积。

应用槽上限为 6,553,600 字节。构建通过只说明能链接且未越界，不代表有足够发布余量，更不代表 USB 物理链路与 SD 交接已通过。正式版应在上述测试通过、确认体积预算后，再将能力并入正常 Read Pico 构建。

## 2026-10-13 单机体验版进度

最初的 1.2 USB 实验构建大小为 **6,180,800 字节**，SHA-256 为
`d64b7f51216988bb9b80a501dce5f260f14a7ea0ce1523e1ef0ec48afa97787b`。
4 KiB 擦除对齐后的应用槽余量为 **372,736 字节**：超过 128 KiB 的单机试刷门槛，
但仍低于 512 KiB 的正式发布预留。禁止据此制作公开 OTA。

Read Pico `14:c1:9f:d1:9f:58` 已通过设备身份、原应用槽回退副本与设备摘要、
分区/OTA 元数据、写入后双重摘要和启动日志检查；应用仅写入 `0x10000`，
启动加载器、分区表、OTA 元数据及 SD 卡未写入。启动日志确认进入阅读页且无 fatal。
当时 **USB-MSC 枚举、拖拽、弹出、断线和文件完整性均未验收**，不能因为启动成功标记功能完成。

首次 macOS 真机试验中，15.9 GB SD 卡曾以 FAT32 `NO NAME` 卷枚举并挂载，
但读卷信息卡住，随后磁盘从主机消失；设备画面仍停在“已连接”。没有向卡中写入测试文件。
这次试验判定为 **未通过**，不能发布。代码检查发现
`UsbDriveActivity` 曾把正常的 USB 主机 suspend 持续 2 秒当作断线并主动重启；
现已移除该规则，保留真实 eject/disconnect 路径。下面的复测检验了该修正，
但尚不能断定它是唯一原因。
修正后的实验镜像为 **6,180,608 字节**，SHA-256：
`7668aea51142e92c5a18068089ed53df57e0149bd972f3784ac84203858094b1`；
通过编译及源级回归测试。重新刷入同一台 Read Pico 后，macOS 将 15.9 GB FAT32
SD 卡识别并挂载，空闲约一分钟未自行消失；`/books` 目录可读，卡上既有 MD
与电脑原件 SHA-256 一致。另复制 19,370 字节临时 MD 到 `/books`，从盘中读回
逐字节一致，随后删除测试副本并安全弹出。设备自动返回首页，用户确认书库原有书籍
仍能显示。此为 **macOS 单机小文件路径通过**，不等于功能已完成：Windows、
大文件、断线、重复连接和长时间稳定性仍待验证，仍不得公开发布或用于用户 OTA。
