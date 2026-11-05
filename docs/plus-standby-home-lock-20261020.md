# Plus 待机期间 Home 前光快捷键穿透 · 2026-10-20

用户已确认上一轮大图排版可用，另报告：待机锁屏后双击 Home 仍可开启前光。
本轮只修输入所有权，保留前一轮大图宽度、48 相正常显示及其他已有功能。

## 原因与修改依据

- `upstream/src/main.cpp:1330`（修改前）：Home 的 `ToggleFrontlight` 是主循环全局操作，
  在 `activityManager.loop()` 之前调用 `toggleFrontlight()` 并保存 `frontlightOn`。
- `ActivityManager.cpp:524` 的 `atRest = standbyShowing()` 只阻止页面内 Home / 控制中心手势，
  不能拦住此前已执行的全局前光操作。
- `MappedInputManager.cpp:24–43` 同时拥有 Home 双击识别状态、当前动作和传输期间延迟动作；
  仅在前光调用点判断待机，会留下半次点击或唤醒后补执行的延迟动作。
- `MappedInputManager.h:111` 原有 `resetHomeButtonInput()` 会取消全部 Plus 物理键手势，
  不能在每一帧待机时重复调用，否则可能破坏 Back 等合法唤醒按键的完整按下/释放。

## 修复

1. 主循环每次更新输入快照后，如 Standby 正显示，立即调用专用的
   `suppressStandbyHomeActions()`：清空 Home 识别器、当前动作和延迟动作。
   原始 Back / Power 和 Plus 物理按键识别器不被这一操作反复取消。
2. 设置 `homeNeedsFreshPress`，在下一次真正的 Home 按下边沿前，不接收此前接触产生的
   释放或长按事件。无需新增 SDK 按下状态接口；HAL 已有 `wasHomeKeyPressed()`。
3. `toggleFrontlight()` 本身再判断 `standbyShowing()`，阻止其他全局入口绕过保护。
   被忽略的按键不写前光硬件，也不保存设置。锁屏内的 Refresh / Home / ReaderMenu 等
   Home 动作同样被丢弃，而不是只临时阻止灯亮。

唤醒后原先的“恢复待机前前光”设置仍由 StandbyActivity 的原流程管理，未改变此偏好。
未新增 UI、配置字段、定时器、任务、延迟或堆分配，仅一个输入状态布尔值。

## 验证

`test_plus_standby_home.py` 编译运行实际 `MappedInputManager::update()`、生产抑制 / 重置方法、
真实 HomeButtonInput 识别器与实际全局前光函数及派发代码（ASan/UBSan）。

- 锁屏中反复双击、单击超时、长按，均不执行前光写入或设置保存。
- 锁屏前半次双击、锁屏中的延迟动作、不带新按下边沿的唤醒后释放/长按均被丢弃。
- 解锁后重新双击正常，只触发一次；Plus 映射为 Light 的全局动作也受保护。
- 原始 / 映射 Back 状态仍供原唤醒路径使用，前光 On/Off 偏好均保持不变。

既有 `test_plus_standby_frontlight.py` 验证待机关灯、亮度/色温保留和恢复开关；
`test_plus_button_mapping.py` 继续覆盖真实 Plus 五键映射、取消、短长按互斥和侧电源键独立。
上述主机测试不替代 Home 电容键与 PMU 的真机按键验收。

真机验收步骤：待机前分别开灯/关灯 → 锁屏 → 多次双击及长按 Home →
灯不亮、页面不跳；再用既定唤醒按键退出 → 检查恢复前光偏好 → 全新双击 Home 能正常切灯。
另测 Home 按住跨越唤醒后才松开，不应补触发。

## 构建与安装

完整 Plus 回归通过（39 个 Python 入口、3 个顶层原生程序），最终新增的完整 Back 手势
保留和解锁单/长按用例另行通过。固件编译、触控链接与单一时钟校验通过；既有 SDK / 未使用
变量警告仍在。来源记录保持 2,844 文件 / 102 Plus adaptations，尚未提交、推送或发布。

本地归档 `.local/standby-home-lock-20261020/`：

- `1.2.2-dev`，4,861,072 B。
- BIN SHA256 `a28872deae13e4d61bad1ac3a74e42c5a28dce39a1ed8320615473745dce111f`。
- ELF SHA256 `b9e01c18d29005a63ac30284d0579f94c730dabe813b81cb14ce5fe6cd517236`。
- 日志 `.local/tests-standby-home-lock-20261020.log` /
  `.local/build-standby-home-lock-20261020.log`。

设备仍为 Plus `1c:29:04:d0:2a:50`。重新读取分区/OTA/app 头，校验分区 MD5、OTA CRC、
序号 1 / app0，以及已验收大图版 `65e7f85c…` 的 app 头，再只写 app0 `0x10000`。
设备端摘要校验成功，显式 USB Serial/JTAG 重启后，确认 ELF `b9e01c18d…` 并进入 `InxRecent`。
未通过刷机工具写 bootloader、分区表、OTA 选择区、NVS、SPIFFS 或 SD 卡。
记录在 `.local/device-installations/standby-home-lock-20261020/`。

启动成功不替代实际 Home 双击 / 长按、跨唤醒释放及前光恢复的用户验收。

用户随后回复“可以了”，确认本次锁屏 Home 误开前光的问题已解决，并要求提交推送。
不把这一反馈扩展为全部按键组合、长时间待机或所有历史实验的验收。
