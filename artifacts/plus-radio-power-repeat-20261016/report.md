# Same-binary independent reboot repeat



## 实测

| 阶段 | 第一次TCP | 第二次TCP | TX失败数 |
|---|---:|---:|---:|
| power-default | 15003ms / False | 1363ms / True | 11/2 |
| power-same-explicit | 15003ms / False | 1353ms / True | 14/3 |
| power-cap13 | 15003ms / False | 1181ms / True | 11/1 |
| power-cap5 | 116ms / True | 100ms / True | 0/0 |
| power-original-repeat | 14810ms / True | 94ms / True | 12/0 |

所有阶段BSSID与本轮初始AP相同（只保存相同比较结果，不公开MAC/SSID），RSSI约-55至-57。5dBm两个连接均成功且发送失败0；20/13dBm第一次均15秒超时，恢复20dBm后首次连接仍耗时14.810秒（在15秒上限前成功），并出现12次TX失败；不是严格超时。两次独立启动使用相同应用SHA。效果支持发送功率相关故障；供电/天线/PHY校准或驱动原因未分离，未验证远距离网络。没有将5dBm原始TCP结果直接称为字体安装或校时验收。
