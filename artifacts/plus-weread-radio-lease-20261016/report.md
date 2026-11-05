# Plus 微信读书联网：补上发射功率租约（2026-10-16）

28344e84 只在持有 `HalWifiAwakeLease` 时把发射功率限到 5 dBm。HttpDownloader（含在线更新）、网页传书、KOReader 和校时都持有它，但微信读书走的是自己的 `WeReadHttpClient`，没有持有，所以仍以 20 dBm 发射，也就是原来出问题的条件。

无人值守验收（同一台 Plus、同一个路由器，RSSI −55/−56，每轮都冷重连）：用正式的 `WeReadClient::Operation` 做书架同步。这台 Plus 没登录微信读书，所以每轮到拿到登录二维码（`/api/auth/getLoginUid`）为止，然后取消，不记录二维码。

| 轮次 | 对照 A：不持有租约（20 dBm） | 修复 B：持有租约（5 dBm） |
|---|---:|---:|
| 1 | 15,934 ms | 960 ms |
| 2 | 16,671 ms | 518 ms |
| 3 | 21,329 ms | 972 ms |

A 的 TCP 跟踪显示：发往 weread.qq.com（124.223.x）的 SYN 反复没有回应，首次连接 5 s 超时，第二个套接字又重传约 9 s 才收到 SYN-ACK。B 的 TCP 在 90–516 ms 内连上。两轮中 COS 下载 4/4 与校时都通过。

修复：`WeReadHttpClient::request` 在 Plus 上于请求期间持有该租约（与 HttpDownloader 相同），覆盖书架、详情、封面、整书下载和进度同步。

验收后已写回正式 1.2.0（4,816,672 B，应用 SHA256 3153045f…，ELF 306e98d4e…），启动至 InxRecent。

未验证：已登录账号下的书架同步、整书下载（本机未登录，二维码需要人扫码）。
