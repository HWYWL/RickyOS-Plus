// =============================================================================
//  RickyDesignTokens.h — RickyOS 设计规范 (docs/design-system.md) 的 C++20 实现
//
//  Wired into refresh, InxTheme metrics and the main tab bar. Screens still
//  draw their own geometry; device acceptance is still open.
//  目标位置：src/components/RickyDesignTokens.h
//  目标：取代散落在 UiHighDpiProfile.h / InxTheme.h / BaseTheme.h /
//        UIThemeTokens.h / RickyPageUi.h 与各页面 constexpr 中的样式常量。
//  约定：
//    * 所有长度单位为 Read Pico 物理像素（684×1216 竖屏，≈297 ppi）。
//    * 灰阶等级 0 = 黑 … 15 = 白（与 epdiy 4bpp 一致，见 epdiy.h:93）。
//    * 只读 constexpr 数据；无全局对象、无堆分配、无 RTTI。
//    * 名称即规范：改数值必须同步改 docs/design-system.md。
// =============================================================================
#pragma once

#include <cstdint>

namespace ricky::ds {

inline constexpr int kSchemaVersion = 1;  // DRAFT v1 (2026-10-08)

// -----------------------------------------------------------------------------
// 0. 面板
// -----------------------------------------------------------------------------
namespace panel {
inline constexpr int16_t kWidth = 684;
inline constexpr int16_t kHeight = 1216;
inline constexpr int kPpi = 297;  // 标称值；1 mm ≈ 11.7 px
}  // namespace panel

// -----------------------------------------------------------------------------
// 1. 灰阶色板（§3）。只有 6 个具名等级；其余 10 级不得直接出现在 UI 代码里。
// -----------------------------------------------------------------------------
// RICKYOS_GRAY_PRESET（Pico 灰阶深浅，lib/GfxRenderer 里的浅灰/深灰填充同步）。默认 3（用户 2026-10-17 定）：
//   0 = 1.2.x 原值，1 = 加深一档，2 = 和 Plus 一样深（细线也加到 3 px），
//   3 = 呼应 Pico 机身按键：圆键和两道短线的印刷灰（实拍约等于第 8 级）、约 3 px 粗。
#ifndef RICKYOS_GRAY_PRESET
#define RICKYOS_GRAY_PRESET 3
#endif
namespace gray {
using Level = uint8_t;  // 0..15, 0 = black
inline constexpr Level kInk = 0;  // #000000 文字/图标/反白底/主按钮
#if RICKYOS_GRAY_PRESET == 3
inline constexpr Level kInk2 = 3;   // 次级文字（仅静止帧）
inline constexpr Level kMute = 6;   // 禁用/占位（仅静止帧）
inline constexpr Level kLine = 8;   // 分隔线/进度轨/边框：机身按键的印刷灰
inline constexpr Level kTint = 12;  // 小面积底（仅静止帧）
#elif RICKYOS_GRAY_PRESET == 2
#if FREEINK_DEVICE_METALIO_PLUS
// Plus menu chrome needs more separation from paper after fast gray updates.
// Keep the black/white endpoints, antialias coverage and image tones unchanged.
inline constexpr Level kInk2 = 2;
inline constexpr Level kMute = 3;
inline constexpr Level kLine = 5;
#else
inline constexpr Level kInk2 = 2;   // 次级文字（仅静止帧）
inline constexpr Level kMute = 5;   // 禁用/占位（仅静止帧）
inline constexpr Level kLine = 7;   // 分隔线/进度轨（仅静止帧）
#endif
inline constexpr Level kTint = 12;  // 小面积底（仅静止帧）
#elif RICKYOS_GRAY_PRESET == 1
inline constexpr Level kInk2 = 3;   // 次级文字（仅静止帧）
inline constexpr Level kMute = 6;   // 禁用/占位（仅静止帧）
inline constexpr Level kLine = 9;   // 分隔线/进度轨（仅静止帧）
inline constexpr Level kTint = 12;  // 小面积底（仅静止帧）
#else
inline constexpr Level kInk2 = 4;   // #444444 次级文字（仅静止帧）
inline constexpr Level kMute = 8;   // #888888 禁用/占位（仅静止帧）
inline constexpr Level kLine = 11;  // #BBBBBB 分隔线/进度轨（仅静止帧）
inline constexpr Level kTint = 13;  // #DDDDDD 小面积底（仅静止帧）
#endif
inline constexpr Level kPaper = 15;  // #FFFFFF 背景/反白文字

// 8-bit sRGB 近似值，供模拟器 / 截图对比使用。
constexpr uint8_t toRgb8(Level l) { return static_cast<uint8_t>(l * 17); }

// 交互帧（1-bit, DU）回退：每个灰都必须有确定的黑白去向。
enum class Fallback : uint8_t { ToInk, ToPaper };
constexpr Fallback oneBitFallback(Level l) {
  // ink-2 / mute / line 用在文字与线上 → 墨；tint 只做底色 → 去底（纸）。
  return l <= kLine ? Fallback::ToInk : Fallback::ToPaper;
}
constexpr bool oneBitInk(Level l) { return oneBitFallback(l) == Fallback::ToInk; }

// 大面积（任一边 > kMaxGrayAreaSide）灰底只允许 tint 或更浅：DU 回黑白时残影最轻。
inline constexpr int kMaxGrayAreaSide = 48;
constexpr bool allowedAreaFill(Level l) { return l == kInk || l >= kTint; }

static_assert(oneBitInk(kInk2) && oneBitInk(kMute) && oneBitInk(kLine) && !oneBitInk(kTint));
}  // namespace gray

// -----------------------------------------------------------------------------
// 2. 字体（§4）。px = round(fontconvert pt × 150 / 72)。
// -----------------------------------------------------------------------------
namespace type {
enum class Weight : uint8_t { Regular, Medium, Bold };
// 字符覆盖：Full = cn_rickyos_chars（6474 字，含书名/作者等动态文本）；
// Ui = cn_i18n_chars + ASCII + 标点（只覆盖系统字符串）；Numeric = 数字/日期专用小集。
enum class Coverage : uint8_t { Full, Ui, Numeric };

struct Style {
  uint8_t px;          // 字面 em 高度
  uint8_t lineHeight;  // 行高盒，4 的倍数
  Weight weight;
  int8_t trackingPx;  // 整数像素字距
  uint8_t convertPt;  // fontconvert.py 的 size 参数（150 DPI 基准）
  Coverage coverage;
  bool tabularDigits;  // 生成时不加 --pnum / 冻结 tnum
};

inline constexpr Style kDisplay{48, 56, Weight::Bold, -1, 23, Coverage::Numeric, true};
inline constexpr Style kTitle{33, 44, Weight::Bold, 0, 16, Coverage::Ui, false};
inline constexpr Style kHeadline{29, 40, Weight::Medium, 0, 14, Coverage::Ui, false};
inline constexpr Style kBody{25, 36, Weight::Regular, 0, 12, Coverage::Full, false};
inline constexpr Style kCaption{21, 28, Weight::Regular, 0, 10, Coverage::Ui, true};

// 动态文本（书名、作者、文件名、用户名）若在目标字重子集中缺字，整串退回 kBody；
// 绝不逐字混排字重，绝不使用 GfxRenderer::SyntheticBoldScope。
inline constexpr const Style& kDynamicFallback = kBody;
inline constexpr bool kAllowSyntheticBold = false;

constexpr bool onGrid(const Style& s) { return s.lineHeight % 4 == 0 && s.lineHeight > s.px; }
static_assert(onGrid(kDisplay) && onGrid(kTitle) && onGrid(kHeadline) && onGrid(kBody) && onGrid(kCaption));
}  // namespace type

// -----------------------------------------------------------------------------
// 3. 间距与栅格（§5）。8px 基础，4px 半步。
// -----------------------------------------------------------------------------
namespace space {
inline constexpr int16_t k1 = 4;
inline constexpr int16_t k2 = 8;
inline constexpr int16_t k3 = 12;
inline constexpr int16_t k4 = 16;
inline constexpr int16_t k5 = 20;
inline constexpr int16_t k6 = 24;
inline constexpr int16_t k8 = 32;
inline constexpr int16_t k10 = 40;
inline constexpr int16_t k12 = 48;
inline constexpr int16_t k16 = 64;

inline constexpr int16_t kPageMargin = k10;                                // 40：所有 UI 页面左右边距（阅读正文除外）
inline constexpr int16_t kContentWidth = panel::kWidth - 2 * kPageMargin;  // 604
inline constexpr int16_t kGutter = k5;                                     // 20：所有栅格栏距
inline constexpr int16_t kSectionGap = k10;                                // 40：分组之间
inline constexpr int16_t kBlockGap = k8;                                   // 32：同组内大块之间（页眉→首卡）
inline constexpr int16_t kCardPadding = k6;                                // 24：卡片内边距
inline constexpr int16_t kDialogPadding = k8;                              // 32
inline constexpr int16_t kInlineGap = k4;                                  // 16：图标→文字
inline constexpr int16_t kTextStackGap = k1;                               // 4：标题→副标题
inline constexpr int16_t kSelectionBleed = k4;                             // 16：列表反白条向页边距外扩
inline constexpr int16_t kTouchSpacing = k2;                               // 8：相邻触控区最小间隔

constexpr int16_t columnWidth(int columns) {
  return static_cast<int16_t>((kContentWidth - (columns - 1) * kGutter) / columns);
}
static_assert(columnWidth(2) * 2 + kGutter == kContentWidth, "2-col grid must be exact");      // 292
static_assert(columnWidth(3) * 3 + 2 * kGutter == kContentWidth, "3-col grid must be exact");  // 188
static_assert(columnWidth(4) * 4 + 3 * kGutter == kContentWidth, "4-col grid must be exact");  // 136
}  // namespace space

namespace layout {
inline constexpr int16_t kTouchMin = 88;  // ≈7.5 mm
inline constexpr int16_t kStatusBarHeight = 48;
inline constexpr int16_t kPageHeaderHeight = 96;   // 大标题页眉（tab 根页面）
inline constexpr int16_t kSubHeaderHeight = 88;    // 返回 + 标题（子页面）
inline constexpr int16_t kTabBarHeight = 112;      // 含顶部 2px hairline
inline constexpr int16_t kRowHeight = 96;          // 单行列表
inline constexpr int16_t kRowHeightTwoLine = 120;  // 标题 + 副标题
inline constexpr int16_t kSectionHeaderHeight = 48;
inline constexpr int16_t kButtonHeight = 88;
inline constexpr int16_t kButtonCompactHeight = 56;  // 命中区仍补足 kTouchMin
inline constexpr int16_t kButtonMinWidth = 160;
inline constexpr int16_t kButtonPaddingX = 32;
inline constexpr int16_t kTabPillWidth = 88;
inline constexpr int16_t kTabPillHeight = 48;
inline constexpr int16_t kSettingsCardHeight = 144;
inline constexpr int16_t kProfileCardHeight = 136;
inline constexpr int16_t kAppTileSize = 112;
inline constexpr int16_t kAvatarSize = 88;
inline constexpr int16_t kProgressHeight = 8;
inline constexpr int16_t kReaderHeaderHeight = 48;
inline constexpr int16_t kReaderHeaderGap = 16;
inline constexpr int16_t kReaderFooterHeight = 96;
inline constexpr int16_t kReaderChromeInset = 32;
inline constexpr int16_t kToggleWidth = 64;
inline constexpr int16_t kToggleHeight = 36;
inline constexpr int16_t kToggleKnobOn = 26;   // 直径
inline constexpr int16_t kToggleKnobOff = 22;  // 直径
inline constexpr int16_t kCoverAspectW = 2;
inline constexpr int16_t kCoverAspectH = 3;

// 内容区（tab 根页面）
inline constexpr int16_t kContentTop = kStatusBarHeight + kPageHeaderHeight;  // 144
inline constexpr int16_t kContentBottom = panel::kHeight - kTabBarHeight;     // 1104

static_assert(kRowHeight >= kTouchMin && kButtonHeight >= kTouchMin && kTabBarHeight >= kTouchMin);
static_assert(kStatusBarHeight % 8 == 0 && kPageHeaderHeight % 8 == 0 && kSubHeaderHeight % 8 == 0 &&
              kTabBarHeight % 8 == 0 && kRowHeight % 8 == 0 && kRowHeightTwoLine % 8 == 0);
}  // namespace layout

// -----------------------------------------------------------------------------
// 4. 圆角（§6）：3 个值 + pill。
// -----------------------------------------------------------------------------
namespace radius {
inline constexpr uint8_t kNone = 0;        // 封面、全宽条
inline constexpr uint8_t kControl = 12;    // 按钮、行选中、输入框、封面托盘
inline constexpr uint8_t kContainer = 24;  // 卡片、应用图块、弹窗、面板
inline constexpr uint8_t kPill = 0xFF;     // 哨兵：绘制时取 min(w,h)/2

constexpr int resolve(uint8_t r, int w, int h) {
  const int half = (w < h ? w : h) / 2;
  return r == kPill ? half : (r < half ? r : half);
}
// 同心规则：内层圆角 = 外层 − 内缩，不足取 0。
constexpr int concentric(int outer, int inset) { return outer - inset > 0 ? outer - inset : 0; }
}  // namespace radius

// -----------------------------------------------------------------------------
// 5. 线宽（§7）：2 个值。图标线宽见 icon::strokeFor()。
// -----------------------------------------------------------------------------
namespace stroke {
inline constexpr uint8_t kHairline = RICKYOS_GRAY_PRESET >= 2 ? 3 : 2;  // 卡片描边、分隔线、次按钮、进度轨(1-bit)
inline constexpr uint8_t kEmphasis = 4;        // 弹窗边框、按下态主按钮、强调线
// 真实封面图的外沿：浅灰（gray::kLine）细边，只为让浅色封面不融进纸面；黑框会压过灰度封面。
inline constexpr uint8_t kCoverEdge = 2;
enum class Divider : uint8_t { None, Solid };  // 无 Dotted
inline constexpr Divider kDefaultListDivider = Divider::None;
}  // namespace stroke

// -----------------------------------------------------------------------------
// 6. 选中与交互状态（§8）
// -----------------------------------------------------------------------------
namespace state {
enum class Selection : uint8_t { Invert };  // 唯一选中语言：承载面反白
enum class Pressed : uint8_t { Invert };    // 按下 = 同一反白，DU 立即提交
// 主按钮本身是墨底，其按下态改为纸底 + kEmphasis 描边（“反向反白”）。
inline constexpr uint8_t kPrimaryPressedStroke = stroke::kEmphasis;
inline constexpr uint32_t kPressedMinVisibleMs = 0;        // 不额外拖延；导航帧自然覆盖
inline constexpr gray::Level kDisabledText = gray::kMute;  // 1-bit 回退：墨 + 去箭头/值
}  // namespace state

// -----------------------------------------------------------------------------
// 7. 图标（§9）
// -----------------------------------------------------------------------------
namespace icon {
inline constexpr int kGridUnits = 24;
inline constexpr int kPaddingUnits = 2;  // 活动区 20u
inline constexpr int16_t kInline = 24;   // 状态栏、行尾箭头、行内
inline constexpr int16_t kList = 32;     // 列表行前导、子页返回
inline constexpr int16_t kNav = 40;      // 标签栏、设置卡片
inline constexpr int16_t kTile = 56;     // 应用图块、空状态、文件网格
constexpr uint8_t strokeFor(int16_t size) {
  return size <= 24 ? 2 : size <= 40 ? 3 : 4;  // 24→2, 32→3, 40→3, 56→4
}
// 奇数线宽的中心线落在 .5 像素上，偶数落在整数像素上（导出脚本负责）。
constexpr bool centerOnHalfPixel(int16_t size) { return strokeFor(size) % 2 == 1; }
static_assert(strokeFor(kInline) == 2 && strokeFor(kList) == 3 && strokeFor(kNav) == 3 && strokeFor(kTile) == 4);
}  // namespace icon

// -----------------------------------------------------------------------------
// 8. 渲染与刷新（§2）
// -----------------------------------------------------------------------------
namespace refresh {
enum class Tier : uint8_t { Interaction1Bit, Resting16Gray };
// Local visual experiment: -DRICKYOS_UI_DIRECT_GRAY=0 restores B/W + idle
// settle. This is not a user setting and does not alter reader page rendering.
#ifndef RICKYOS_UI_DIRECT_GRAY
#define RICKYOS_UI_DIRECT_GRAY 1
#endif
inline constexpr bool kDirectGrayUi = RICKYOS_UI_DIRECT_GRAY != 0;
inline constexpr uint8_t kGc16EveryDirectFrames = 8;    // Content-area changes, not every UI redraw.
inline constexpr uint8_t kGc16EveryControlFrames = 32;  // Small controls/navigation have their own regional ledger.
inline constexpr uint32_t kGraySettleDelayMs = 1500;    // 同 ActivityManager.cpp kGraySettleDelayMs
inline constexpr uint8_t kGc16EverySettles = 8;         // 同 settleToGray() kCleanEvery
inline constexpr uint8_t kGc16EveryDuFrames = 32;       // 建议值，需真机调参
inline constexpr bool kDitherInChrome = false;          // UI 外框禁止棋盘抖动
}  // namespace refresh

// -----------------------------------------------------------------------------
// 9. 组件配方（§10）— 只放几何；绘制在 RickyPageUi / InxTheme 中实现。
// -----------------------------------------------------------------------------
namespace component {
struct Box {
  uint8_t radius;
  uint8_t strokeWidth;  // 0 = 无描边
  gray::Level fill;
  gray::Level strokeLevel;
};
inline constexpr Box kCard{radius::kContainer, stroke::kHairline, gray::kPaper, gray::kInk};
inline constexpr Box kCardSelected{radius::kContainer, 0, gray::kInk, gray::kInk};
inline constexpr Box kDialog{radius::kContainer, stroke::kEmphasis, gray::kPaper, gray::kInk};
inline constexpr Box kButtonPrimary{radius::kControl, 0, gray::kInk, gray::kInk};
inline constexpr Box kButtonPrimaryPressed{radius::kControl, stroke::kEmphasis, gray::kPaper, gray::kInk};
inline constexpr Box kButtonSecondary{radius::kControl, stroke::kHairline, gray::kPaper, gray::kInk};
inline constexpr Box kButtonSecondaryPressed{radius::kControl, 0, gray::kInk, gray::kInk};
inline constexpr Box kChip{radius::kPill, stroke::kHairline, gray::kPaper, gray::kInk};
inline constexpr Box kRowSelected{radius::kControl, 0, gray::kInk, gray::kInk};
// 三项设计决定 §3：应用瓷砖用控件圆角 12，不是容器圆角。
inline constexpr Box kAppTile{radius::kControl, stroke::kHairline, gray::kPaper, gray::kInk};
inline constexpr Box kCoverPlateSelected{radius::kControl, 0, gray::kInk, gray::kInk};
inline constexpr int16_t kCoverPlatePadding = space::k2;  // 8

struct ProgressBar {
  int16_t height = layout::kProgressHeight;
  gray::Level trackResting = gray::kTint;   // 静止帧：8px tint 胶囊
  uint8_t trackOneBit = stroke::kHairline;  // 交互帧：居中 2px 墨线
  gray::Level fill = gray::kInk;
  uint8_t cornerRadius = radius::kPill;
};
inline constexpr ProgressBar kProgress{};

inline constexpr int kCoverTemplates = 6;
// FNV-1a 低位分布差：三本演示书 hash % 6 全为 0（全是“树”）。取高 16 位。
constexpr uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (; s && *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
  return h;
}
constexpr int coverTemplateFor(const char* title) { return static_cast<int>((fnv1a(title) >> 16) % kCoverTemplates); }
}  // namespace component

}  // namespace ricky::ds
