# Plus 普通联网兼容版已安装（2026-10-16）

普通候选RickyOS-1.2.0-plus-tx-power-compat.bin，4,886,912B，SHA2569287f68b978ee5c7ed84eee7cceb2324467722404024e16c99fd91744d7aad9b，ELFb1006c9377586f49bc648ad9bebbde557c5202033fe6425b9de2690cb4fa4f86，IDF9a97f6c5。

已仅写ota_1（0x580000），应用读回MD5一致；启动程序/分区表/OTA选择/槽外区域MD5均与安装前一致。随后watchdog启动，40秒日志中进入InxRecent主页，无诊断入口或panic。正常构建未启用RADIOAUTO/ACCEPTANCE/NETTEST/TCPTRACE/速率初始化包装，仅启用TX_POWER_COMPAT；compile-flags.json及二进制标记检查已确认。当前设备保留此普通版；自动测试镜像已移除。

实际修正位于共享HalWifiAwakeLease：Plus网络使用者期间上限min(原值,20 quarter-dBm即5dBm)，最后使用者释放恢复；不提高更低的原上限，尊重外部修改，失败尽力回滚功率与省电模式。共享HTTP下载、网页传书、NTP租约，其他硬件板不受此项目私有宏影响。无新增堆对象/任务。

上机功能验收使用同一修正与实际生产下载/校时接口，详见../plus-tx-power-acceptance-20261016/report.md：正常DNS、SD挂载、各次冷重连，无前置预热探测；COS屏保列表827/565ms、字体列表676ms、完整字体1,763,452B/6702ms并CRC读回一致、原生SNTP1096ms成功。普通版此次验证启动，不冒称用户已手动验收UI、整字体家族安装或网页上传。远距离/其他AP和功率相关故障的供电/天线/PHY唯一根因仍未确定。

聚焦测试test_plus_clock_radio.py（实际HalClock/租约，两宏模式、重叠持有/失败回滚/较低上限/外部修改）及test_plus_acceptance_integrity.py（实际CRC/临时文件独占创建）通过；完整构建、2826文件/79适配来源校验、触控/时钟链接检查通过。编译仅有既有警告。

原始16MiB不可变恢复备份留在私有.local/device-installations/plus-tx-power-compat-20261016/pretest-flash.bin；对应guarded回退脚本.local/restore_after_tx_power_compat.py未执行。当前应用有意不同于备份，不能标记整片Flash一致。外部FT6336U触控V03维持，正常NTP可能更新外部RTC，不属于主Flash回滚范围。未提交/推送。
