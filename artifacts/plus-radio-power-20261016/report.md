# 无人值守TX功率上限对照（2026-10-16）

状态：对照完成，普通固件已完整恢复并验证主页。SHA256ef531b75f8ac9df666dede48ecea09c70552cb93434f7402533dbdb0e41c9181，ELF7d4db4343…，4,894,112B，IDF9a97f6c5。用户授权离开电脑后的自主测试；自动选择可见保存网络，固定COS36.248.13.174:443，不依赖DNS或TLS。

本轮所有阶段保留正常TX AMPDU输入值/自动速率，不启用固定速率。五组均完整重启同一Wi-Fi配置/原协议，保持默认STA路由、WIFI_PS_NONE、CPU性能锁、SD挂载。依次默认不设功率、显式设同一个原功率、上限52(13dBm)、上限20(5dBm)、恢复原功率；低功率值取min(原值,目标)，从不提高原上限。每阶段记录setter/getter返回及实际quarter-dBm，再做两个最多15秒的TCP；只有setter/readback成功才有样本。加入同值显式设置组以区分setter本身与降低数值的影响。

SDK esp_wifi_get/set_max_tx_power公开接口；实验只改变运行时上限，不改country/校准/NVS格式或生产配置。结束显式恢复原功率，控制器关闭串口并在核对USB序号到具体串口映射后才发复位，完整恢复普通Flash并验证主页。USB身份核对使用pyserial IOKit映射，避免只按可变usbmodem编号误复位另一设备；写Flash另有芯片/MAC/应用/启动程序/分区/槽外MD5保护。

上一固定速率实验没有显式disable就deinit，导致反向正常组未关联，不能作完整因果对照。本版已补显式清除固定速率状态并检查返回，但本轮整个新MCU启动均不enable固定速率。保留stack上的同一station配置快照，每阶段复制给mutable SDK setter，不持久化/输出凭据。

实际生产源ASan/UBSan回归覆盖power getter失败、cap设置失败跳过、原功率恢复、正常/速率配置复制、固定速率显式清理和资源释放；真实初始化wrapper/帧分类/自动选择回归通过；完整构建和来源/触控/时钟链接检查通过。失败帧分类只保存经AP/STA地址验证的type/subtype/Protected，仍不是空口抓包或逐SYN身份。


## 实测

| 阶段 | 第一次TCP | 第二次TCP | TX失败数 |
|---|---:|---:|---:|
| power-default | 15003ms / False | 1361ms / True | 12/1 |
| power-same-explicit | 15003ms / False | 1156ms / True | 12/1 |
| power-cap13 | 15003ms / False | 1151ms / True | 11/1 |
| power-cap5 | 114ms / True | 101ms / True | 0/0 |
| power-original-repeat | 15003ms / False | 1198ms / True | 11/1 |

所有阶段BSSID与本轮初始AP相同（只保存相同比较结果，不公开MAC/SSID），RSSI约-55至-57。5dBm两个连接均成功且发送失败0；20/13dBm第一次均15秒超时，恢复20dBm再次复现。两次独立启动使用相同应用SHA。效果支持发送功率相关故障；供电/天线/PHY校准或驱动原因未分离，未验证远距离网络。没有将5dBm原始TCP结果直接称为字体安装或校时验收。
