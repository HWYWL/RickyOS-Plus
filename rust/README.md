# Rust 迁移预留

这里将来放 `cargo` workspace。当前阶段目标是先把 C/C++ 固件在本仓库内稳定构建、
烧录和验收，**本目录暂不参与构建**。

计划的 crate 划分：

| crate | 类型 | 职责 |
| --- | --- | --- |
| `rickyos-core` | `no_std` 可主机测试 | EPUB/TXT 解析、分页、排版、设置与统计的序列化 |
| `rickyos-ui` | 依赖 `core` | Activity 栈与页面状态机（渲染先走 C++ `GfxRenderer`） |
| `rickyos-ffi` | `staticlib` | 导出 C ABI，作为 IDF 组件接入现有工程 |
| `rickyos-sim` | 主机二进制 | 复用 684×1216 帧缓冲做黄金帧回归 |

接入约束（务必遵守）：

1. 只通过 `extern "C"` 边界交互，参数只用 POD 与 `(ptr, len)`；不得让 Arduino `String`、
   `std::vector`、C++ 异常跨越边界。
2. `panic = "abort"`，`opt-level = "z"`，禁用 `core::fmt` 的重路径，构建后必须校验镜像体积。
3. 分配器走 `esp-alloc` 的 PSRAM 能力分流；面板 DMA 所需的内部连续内存不得被挤占。
4. `metalio_board`（LCD 时序、波形、LUT）、`rickyos_display`、音频、USB、SD、TLS 保持 C/C++。

迁移顺序：解析与排版 → 设置/统计 → i18n → Activity 框架 → 渲染路径。
每一步都要有主机侧差分（与 C++ 实现逐页对比）和实机翻页回归。
