# RickyOS

由 **Ricky AI Studio** 为 MindReset Read Pico（小纸 Pico）打造的阅读与生活工具固件。
基于 [CrossMux](https://github.com/0x1abin/crossmux) 二次开发，保留上游历史、MIT
许可证和各第三方组件的许可。本仓库是未公开发布的协作开发仓库，不是上游官方固件。

当前正式版本：RickyOS `1.1.2`；开发版本：`1.1.3-dev`；设备：ESP32-S3 / 16 MB Flash，
684 × 1216 竖屏。RickyOS 品牌界面仅由产品环境启用；其他上游环境仍保留。

- 首页：昵称问候、继续阅读、最近书籍封面、阅读统计、自定义短句和头像（点头像进入阅读统计）。
- 个人资料：昵称、首页短句、头像；内置 8 个与 logo 同风格的线描头像（不同年龄与性别），也可从 SD 卡导入。
- 书库：全部 / 在读 / 未读；只收录书籍，跳过 XTCache 等系统文件夹和其他阅读器留下的记录文件。
- 存储：书籍、字体、图片、下载四个固定文件夹（/books、/fonts、/images、/downloads），
  首次开机把 /book、/Pushed Books 里的书连同阅读进度一次性移入 /books。
- 内置中文字体覆盖常用简体字、GB2312 一级字和常用繁体字（Big5 一级）。
- 在线下载字体：思源黑体、思源宋体、霞鹜文楷、得意黑（RickySans / RickySerif / LXGWWenKai / RickyGrin），
  常规体常用字子集，每个文件都能放进 Flash 加速缓存；来自 [RickyOS-fonts](https://github.com/chinoryunqin/RickyOS-fonts)。
- 固定主题与主页面布局，40 px 线条导航图标、选中项黑色胶囊反白，统一点入/返回的设置交互。
- 专属人物与狗狗 logo、静态开机标识和待机品牌画面。
- 待机画面在「应用 → 待机显示」中统一设定；可选择用户图片（BMP/PNG/JPG）、下载默认壁纸、
  设置待机信息或保留阅读页。「设置 → 电源与待机」也可进入同一页面。
- 设置中的「RickyOS 更新」仅检查本品牌正式固件，与官网共用发行目录；没有 Nightly 或 CrossMux 回退。
- 保留五子棋及阅读/生活工具；移除九款已确认不要的小游戏和趣味应用，以及 AirPage。
- macOS 浏览器预览直接显示原生模拟器帧缓冲，不另造一套网页 UI。
- 界面语言为简体中文和英文；断字词典仅英语，以腾出应用分区空间。

设备内置中文拼音输入尚未实现。字体管理内存、墨水屏残影/刷新、功耗和长期稳定性
仍需验收。部分功能已有实机记录，但合并后的整包、在线更新与重启仍需确认；
最终镜像必须保留至少 512 KiB 的应用槽空间。当前开发包不可作为正式发行版发布。

## 协作快速开始

```sh
git clone --recurse-submodules https://github.com/chinoryunqin/RickyOS.git
cd RickyOS
python3 -m venv .venv
source .venv/bin/activate
python -m pip install pioarduino==6.2.0
python scripts/patch_pioarduino_cache.py --prepare-platform
pio run -e rickyos_readpico
```

`--prepare-platform` 在新的 PlatformIO 工具目录首次构建前运行一次（删除或升级
`~/.platformio` 后需重跑）；否则平台安装器会替换正在运行的 SCons，构建报
`No module named 'SCons.Tool.FortranCommon'`。命令依赖上面激活的虚拟环境；
未激活时使用 `.venv/bin/python`。详见 [构建系统](docs/engineering/build-system.md#tool-initialization)。

已有克隆使用 `git submodule update --init --recursive` 获取固定版本的 SDK。
默认环境是上游 C3，不要直接运行不带 `-e` 的构建/上传命令来操作 Read Pico。

macOS 原生预览需要 Xcode Command Line Tools、SDL2、curl/pkg-config 开发依赖
（可通过 `brew install sdl2 curl pkg-config` 安装）：

```sh
pio run -e simulator_rickyos
python tools/rickyos-browser/server.py --open
```

打开 http://127.0.0.1:8765/。详见 [浏览器预览说明](tools/rickyos-browser/README.md)。
测试：`python -m unittest discover -s scripts/tests -v`。
字体和品牌资源生成另需开发依赖；普通构建使用已提交资源，不必重新生成。

提交请使用功能分支并通过 Pull Request 合入 `main`。先阅读 [AGENTS.md](AGENTS.md)，
再看 [RickyOS 协作与验证](docs/rickyos-collaboration.md)。不要上传设备备份、账号资料、
私人书籍、`.env`、本机日志或编译产物。实机刷写必须按设备身份、完整备份、分区与
镜像检查流程单独授权；本仓库没有包含绑定原作者设备的私有安装脚本。

## 在线刷机网站

公开预览：<https://chinoryunqin.github.io/RickyOS-site/>。
正式固件待发布，当前仅开放系统展示和流程演示，不连接、备份或刷写真实设备。
公开站点仓库只保存网站成品，本固件源码仓库继续保持私有。

独立静态站位于 `tools/rickyos-flasher`，需要 Node.js 22.12+（或 Vite 支持的新版 Node）。

```sh
cd tools/rickyos-flasher
npm ci
npm test
npm run build
npm run preview
```

打开 http://127.0.0.1:8766/。演示模式不连接设备、不写 Flash。
正式固件目录目前为空，真实安装尚未开放；网站使用统一入口，自动判断兼容更新
或原厂首次安装。首次安装须匹配经过验收的原厂版本指纹及完整安装包，不能只凭
芯片型号写入；会替换启动组件、调整分区并清理原厂内部文件，不操作 SD 卡。
备份保存在用户电脑，不上传服务器。两种路径都仍需真实 USB 验收。
GitHub Pages 部署与硬件验收要求见 [刷机站说明](tools/rickyos-flasher/README.md)。
设备更新规则见 [RickyOS 在线更新](docs/engineering/rickyos-firmware-update.md)；
`ota.json` 由同一网站目录生成，当前明确返回无更新。

## 上游 CrossMux 说明

下面保留上游参考内容；其发布链接、设备范围和应用列表并不等于 RickyOS 发行范围。

### CrossMux

**English** | [简体中文](./README.zh-CN.md)

**CrossMux** is a community fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) for ESP32 e-ink devices. Reading comes first, with lightweight apps, reading analytics, standby faces, and on-demand services alongside the reader.

[Releases](https://github.com/0x1abin/crossmux/releases) · [User guide](./USER_GUIDE.md) · [Contributing](./docs/contributing/README.md)

![CrossMux running on an Xteink device](./docs/images/cover.jpg)

## Features

- **Reading and library**: EPUB, TXT, XTC/XTCH and images; chapter navigation, bookmarks, dictionaries, custom fonts, reading backgrounds, and KOReader progress sync.
- **Wireless workflows**: browser file transfer and settings, Calibre wireless, OPDS downloads, WebDAV, and device OTA updates.
- **Apps**: lightweight games and tools including Sudoku, Gomoku, Chinese Chess, Minesweeper, 2048, Electronic Woodfish, and Ugly Avatar. [Apps guide](./src/activities/apps/README.md).
- **AirPage**: scan to upload content, then display BMP/JPEG images with manual refresh or foreground live delivery; images can become a sleep screen. [Usage and network behavior](./src/activities/apps/README.md#airpage).
- **WeRead**: QR login, bookshelf browsing, EPUB downloads for offline reading, and progress sync. Available in the China content profile. [WeRead guide (Chinese)](./src/activities/apps/weread/README.md).
- **Reading analytics and standby**: reading statistics, heatmaps, profiles and achievements; clock and Chinese almanac faces. [Analytics guide](./src/activities/apps/reading-stats/README.md).
- **Languages and development**: 33 UI languages in one firmware per hardware target, plus desktop simulators for UI development.

> **WeRead security:** this unofficial Web protocol may change. Device traffic is encrypted, but its client does not verify the server certificate or hostname; use it only on a trusted network. The native simulator verifies certificates through the host trust store. See the [transport details](./docs/engineering/chinese-build.md#weread-transport).

## Devices and release channels

| Device | Chip | Published channels |
|---|---|---|
| Xteink X3 / X4 (shared image) | ESP32-C3 | Stable, Nightly |
| [Seeed Sticky](https://www.seeedstudio.com/sticky/?utm_source=partner&utm_medium=crossmux&utm_campaign=readme) | ESP32-S3 | Nightly |
| Xteink X4 Pro | ESP32-S3 | Nightly |
| M5Stack Paper Mono | ESP32-S3 | Nightly |
| eego A4 | ESP32-S3 | Nightly |
| Murphy M4 | ESP32-S3 | Nightly |
| Waveshare ePaper 3.97 | ESP32-S3 | Nightly |
| [Metalio E-Ink 4](./docs/engineering/metalio-eink4.md) | ESP32-S3 | Nightly |
| [Read Pico (小纸 Pico)](./docs/engineering/read-pico.md) | ESP32-S3 | Nightly |

This table describes configured release targets, not a claim that every feature has passed hardware acceptance. Each S3 target needs its own image. X4 Classic has a build-only target and is absent from public release/OTA indexes. See [device variants](./docs/engineering/device-variants.md) for target-specific limitations.

Use [Stable](https://github.com/0x1abin/crossmux/releases/tag/stable) for the stable X3/X4 channel, or [Nightly](https://github.com/0x1abin/crossmux/releases/tag/nightly) for development builds. The [target table](./scripts/nightly_targets.py) defines channels and artifact names; [release architecture](./docs/engineering/firmware-release.md) explains packaging and OTA. Current source version and build environments live in [platformio.ini](./platformio.ini).

## Install firmware

1. Open [CrossMux Releases](https://github.com/0x1abin/crossmux/releases), choose the channel and exact device, and follow that release's asset links and installation notes. X3/X4 share an image; S3 images are board-specific.
2. Back up your SD card data before changing firmware. Use the matching installation package; an application-only `firmware.bin` is not a complete first-install image.
3. For an existing X3/X4 installation, the [upstream CrossPoint web flasher](https://crosspointreader.com/#flash-tools) offers a custom binary upload: select X3/X4 and upload the **CrossMux** application binary. Choosing an upstream release installs CrossPoint instead.
4. For S3 installation and recovery, follow the matching [device documentation](./docs/engineering/device-variants.md) and release instructions. Do not reuse X3/X4 flash commands or offsets for another board.

To build and flash X3/X4 from source, use the [development commands](#development-quick-start) below. For an existing CrossMux installation, device OTA selects the model, content profile, and channel; S3 targets have no Stable channel.

Metalio E-Ink 4 uses the `metalio-eink4` Nightly package and model/board tag `metalio_eink4`. Both language entries point to the same multilingual firmware. Follow the [Metalio guide](./docs/engineering/metalio-eink4.md) for first installation, wiring, and hardware validation status. The [global Web tool](https://crossmux.com) and [China Web tool](https://crossmux.cn) show its install option once Web support is deployed and a matching Nightly package is present in the release catalog.

### USB-locked Xteink devices

Some devices may restrict USB flashing. The [upstream Xteink Unlocker](https://crosspointreader.com/#unlock-tool) is a separate tool; consult its current compatibility and recovery instructions before use. CrossMux compatibility with a locked device must not be inferred from CrossPoint compatibility. Flashing unsupported firmware may leave the device without a recovery path. If the serial device is missing, also check the data cable, port, and browser permissions.

## Chinese fonts and content profiles

Every hardware target builds one language-unified firmware. Simplified Chinese selects the China content profile (`crossmux.cn`); other UI languages select Global (`crossmux.com`). Changing the UI language updates the profile and regional apps, including WeRead and Chinese Chess.

The UI includes compact 8/10/12pt Simplified-Chinese fallback fonts. Built-in reader font choices share a 12pt offline fallback; complete families, other sizes, style variants, and broader Unicode coverage use SD-card `.cpfont` files. Embedded fonts are a subset, so rare or Traditional Chinese characters may require an appropriate SD font.

Download fonts from **Settings > Reader > Manage Fonts**, or copy converted fonts to the SD card. See [SD-card fonts](./docs/sd-card-fonts.md) for installation and conversion, and [Chinese support](./docs/engineering/chinese-build.md) for the embedded-font toolchain. Normal builds need no font regeneration.

## Development quick start

Install PlatformIO Core (`pio`) and Python 3; the repository pins its pioarduino platform. Full code checks also need clang-format 21+, CMake, and Ninja. See [Getting Started](./docs/contributing/getting-started.md) for setup.

```bash
git clone --recursive https://github.com/0x1abin/crossmux.git
cd crossmux

# If submodules were not initialized:
git submodule update --init --recursive

# X3/X4 development build
pio run -e default

# X3/X4 unified-language stable build
pio run -e gh_release

# Build and flash that image to a connected X3/X4
pio run -e gh_release -t upload
```

The application binary is `.pio/build/gh_release/firmware.bin`. For other boards, use the matching environment in [build-system.md](./docs/engineering/build-system.md).

For Metalio E-Ink 4:

```bash
pio run -e metalio_eink4
CROSSPOINT_RC_HASH=$(git rev-parse --short=7 HEAD) pio run -e metalio_eink4_nightly
```

The development application is `.pio/build/metalio_eink4/firmware.bin`; first installation also requires the matching bootloader and partition layout described in the [Metalio guide](./docs/engineering/metalio-eink4.md).

For Read Pico (小纸 Pico):

Until FreeInk SDK PR #35 is merged, first follow the temporary SDK checkout in the [Read Pico guide](./docs/engineering/read-pico.md#current-implementation--2026-09-30).

```bash
pio run -e readpico
```

On Windows, set `PYTHONIOENCODING=utf-8` before running `pio`, otherwise the build can stall when PlatformIO prints the Arabic i18n language row.

The application is `.pio/build/readpico/firmware.bin`. First installation is a **full-table flash** — `bootloader@0x0`, `partitions@0x8000`, `boot_app0@0xe000`, `app@0x10000` — after a verified full-chip (16 MiB) backup, because the repository partition table and the board's factory `partitions_16M.csv` disagree at `0xE000` and use different app-slot sizes. This target is **build-only in this round**: it is absent from the Nightly/OTA/Web release mappings, and no hardware acceptance has been recorded yet. See the [Read Pico guide](./docs/engineering/read-pico.md) for the pin map, the frozen build flags, and the pending acceptance checklist.

### Desktop simulator

Install SDL2 and curl (plus OpenSSL development headers on Linux), place EPUBs in `fs_/books/`, then run:

```bash
pio run -e simulator -t run_simulator           # X4
pio run -e simulator_x3 -t run_simulator        # X3
pio run -e simulator_eego_a4 -t run_simulator   # eego A4
pio run -e simulator_murphy_m4 -t run_simulator # Murphy M4
pio run -e simulator_readpico -t run_simulator  # Read Pico, 684x1216 portrait
```

The [CrossMux simulator fork](https://github.com/0x1abin/crosspoint-simulator) is pinned in `platformio.ini`. It previews UI and input flows; it does not validate display waveforms, power consumption, or physical hardware timing.

Read Pico fits its window to the desktop while retaining full-resolution BMP screenshots. Mouse input supports tap, hold and swipe; Up/Escape/Down represent the three capacitive keys, and only Power (`P`) wakes from sleep. Native sixteen-level images and SD UI fonts follow the Read Pico paths. See the [device guide](./docs/engineering/read-pico.md#desktop-simulator) for validation details.

### Checks and debugging

```bash
./bin/ci-check       # Full code-change checks; does not rewrite sources
pio device monitor  # Serial logs from a connected device
```

Use [Testing and Debugging](./docs/contributing/testing-debugging.md) for focused checks and the enhanced serial monitor. Documentation-only changes need link, command, and whitespace checks rather than firmware builds.

## Documentation and contributing

- [User guide](./USER_GUIDE.md) · [Web transfer](./docs/webserver.md) · [Web API](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md) · [Governance](./GOVERNANCE.md) · [Contributor guide](./docs/contributing/README.md)
- [Agent instructions](./AGENTS.md) · [Engineering reference](./docs/engineering/index.md) · [Touch and UI](./docs/contributing/touch-and-ui.md)
- [Cache management](./docs/engineering/cache-management.md) · [File formats](./docs/file-formats.md)

Report bugs and propose changes in [CrossMux Issues](https://github.com/0x1abin/crossmux/issues). Contributions target **`0x1abin/crossmux:main`**; keep each PR focused and describe its verification. Existing CrossPoint class names and the `/.crosspoint` SD data directory remain compatibility details, not instructions to target the upstream repository. That directory also holds settings and progress; do not delete it merely to clear a book cache.

## Credits

Thanks to [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), [Inx](https://github.com/obijuankenobiii/inx), [cpr-vcodex](https://github.com/franssjz/cpr-vcodex), and their contributors, and to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader) for the original inspiration.

CrossMux is not affiliated with Xteink or any device manufacturer. Upstream tools and communities are independent of this fork. See [LICENSE](./LICENSE) for the repository license.
