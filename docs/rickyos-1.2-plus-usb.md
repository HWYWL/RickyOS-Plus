# Metalio E-Ink4-Plus USB 传书

本机实现采用 ESP32-S31 HS UTMI 与厂商 GPIO36 ANA_SW 路线，复用 RickyOS 的「传输 → USB 磁盘」入口；不使用 Read Pico 的 S3 Arduino USB 驱动。进入前由 HalStorage 卸载 SdFat；电脑接管整张 SD 卡的原始扇区，设备此时暂停普通书库／阅读、截图与休眠。

## 后端与所有权

components/rickyos_storage/port/PlusUsbMassStorage.cpp 实现 FreeInk UsbMassStorage 接口；HAL/活动不直接操作 USB 芯片。Espressif esp-usb c77046c5d3a6b44c556a95d789bdec914db1cb71 提供 S31 PHY、任务及描述符，TinyUSB registry 0.21.0~2（hash 16c8d774dab1f484d3e0c0f61105624d2424bbae864efd19818baf7b451b41e7）提供 USB/MSC 栈。

scripts/prepare_plus_usb.py 固定提交并校验 registry 哈希，检查三个原始挂载钩子及头文件再生成 raw wrapper；不链接 Espressif 的 FatFS MSC 挂载实现，避免和 SdFat 重复拥有 SD。Arduino USBMSC.cpp 也从本机构建移除。依赖只写入忽略的 .local/usb-dependencies/。

- 静态 USB 描述符提供 FS 64、HS 512 端点，序列号来自 esp_read_mac(..., ESP_MAC_WIFI_STA) 的 6-byte MAC（不用 S31 会返回 8-byte EUI-64 的默认 efuse API）。
- 静态 512-byte scratch 支持部分扇区读改写；对 64-bit 范围作校验，拒绝越界／无效 LUN。整扇区连续请求复用 readSectors/writeSectors。
- USB MSC FIFO 为 4096 bytes，USB 任务默认 4096-byte 栈，库的 PHY／描述符／任务上下文按传书生命周期分配。现有 SdFat block device 不重复分配；没有整文件缓存。额外静态互斥量保护回调与退出，保证不在扇区操作中删除 USB 任务。
- 主机 SYNCHRONIZE CACHE 与弹出调用 syncDevice；弹出后拒绝再读写。退出先 tud_disconnect，再等待并阻止新扇区操作、卸载 USB 栈，恢复 GPIO36 高电平，最后由已有活动重启至主页重挂载 SD。
- 若回调三秒内无法排空或栈卸载失败，恢复调试路线后重启；不重新开放文件系统。串口在 USB 磁盘期间消失是 GPIO36 路由切换的预期行为。
- 对持续总线 suspend，只在 CX25601N 0x6b/REG_STATUS1 0x1e bits2:0 明确无 VBUS 时按拔线退出；有 VBUS／读取失败不把电脑睡眠当成拔线。不写充电芯片寄存器。

## 验证

python3 tests/test_plus_usb.py 编译实际后端，ASan/UBSan 覆盖 init/retry、FS/HS 描述符、扇区范围、partial I/O 周围字节不变、sync/eject、I/O 与卸载错误、退出顺序、调试路线、host suspend/VBUS 不确定及断线状态。完整镜像容量、安装及实机磁盘读回另行记录，主机测试不代表 USB 已实机可用。

验收从「传输 → USB 磁盘」进入，电脑会显示 SD；书放入 /books。完成后在电脑安全弹出，设备应重启回主页并识别新书；再次连接 USB 应恢复调试串口。不要在主机写入期间拔线。

当前构建保留现有原厂 5 MiB OTA 槽与恢复包，不通过修改分区／删除字库绕过容量检查。

## 完整固件和实机过程

当前候选 usb-rasterfix：5,242,720 bytes（5 MiB 槽原始余量 160 bytes，擦除对齐后余量 0），SHA256 2d0babd99f84d07d43d01d5caebc9876f54860ee9b8a06c9023a03300f3bd690。完整 IDF 构建、图像 checksum/hash、源完整性与链接触控／时钟检查通过。未扩大分区，后续任何改动必须重新检查容量。

USB debug 级别为 0，wrapper 保留错误日志；USB／存储参与 LTO，应用和 USB 使用 GCC 15 -Oz（末次链接也设置 -Oz）。外部依赖断言中的工作站绝对路径改为相对路径，断言及源码行诊断保留。IDF／无线／显示驱动优化参数不变。不声称编译策略提升速度。

首个 USB 镜像 c1218f45 已上机：初次启动停留 WaitingForHost，重新插接后电脑枚举 RickyOS Metalio Plus（VID303a/PID4002），挂载 15.7 GB FAT32 SD；未执行文件写入。diskutil 安全弹出成功、调试口恢复。枚举中发现误用默认 efuse MAC API：S31 默认返回 8-byte EUI-64，而代码缓冲只有 6 bytes。已改为 esp_read_mac/ESP_MAC_WIFI_STA 并重测，不交付旧实现。新版本进入 MSC 时先软断开、GPIO36 置低、等待 150 ms 再连接，修正版打开 USB 磁盘后已枚举，序列号正确为 1c2904d02a50。

同轮用户照片显示 Tab 切换残留旧页面。区域高层代码将非零裁剪 x 传给扫描层，厂商 get_buffer_params 将源指针右移，LCD 却继续解码整行。新代码保留 full-raster crop/stride，通过 dirty_lines 和 masked dirty_columns 限制提交范围；成功后仍只更新实际区域 back_fb。新增 tests/test_plus_regional_raster.py 执行厂商真实寻址函数，覆盖非零 x/y 与最后一行边界；原驱动测试只覆盖区域掩码／回写，没有执行该扫描寻址层，已补齐。

usb-rasterfix 实机验收通过：用户确认多次底部 Tab 切换恢复；电脑识别 15.7 GB FAT32 SD。scripts/test_plus_usb_transfer.py 在 /books 新建独立临时文件，以 Darwin F_NOCACHE 读写 1 MiB，fsync 后读回完全一致，SHA256 0c22b3bf0639cc46569b2c9bc5a2b5ad67359166f4cf912acebb6a3e6a487fb4；写入 0.3457 秒、读取 0.2089 秒。临时文件随后删除，没有覆盖已有书籍。diskutil eject /dev/disk4 成功，约 5 秒后调试串口 /dev/cu.usbmodem1101 与原调试 PID1001 恢复。报告 logs/plus-usb-transfer.json。短测不代表大文件持续吞吐；异常拔线和电脑休眠仍只有主机逻辑测试，没有实机破坏性写入验证。当前安装状态以 dist/manifest.json 为准。

usb-rasterfix 已写入 ota_1、读回校验一致、原厂槽一致；首启 ELF ff93d487f，进入 InxRecent，区域提交 result=0。安装日志 logs/plus-usb-rasterfix-install.log，启动 logs/plus-usb-rasterfix-boot.log。
