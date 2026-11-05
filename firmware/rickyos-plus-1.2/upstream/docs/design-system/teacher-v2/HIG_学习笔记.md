# Apple HIG → RickyOS 学习笔记

**读者：** 达芬奇（设计代理）· Rick  
**性质：** 原则提炼，**非改稿、非改固件**。对照已定方案：`RickyOS_视觉方案.md`、`方案.md`、`三项设计决定.md`、`RickyDesignTokens.h`。  
**屏：** 684×1216 · 16 灰 · 无动画过渡 · 中文 UI · 纸感小狗品牌  
**源：** [HIG 总览](https://developer.apple.com/design/human-interface-guidelines/) · [Foundations](https://developer.apple.com/design/human-interface-guidelines/foundations) · [Layout](https://developer.apple.com/design/human-interface-guidelines/layout) · [Typography](https://developer.apple.com/design/human-interface-guidelines/typography) · [Color](https://developer.apple.com/design/human-interface-guidelines/color) · [Icons](https://developer.apple.com/design/human-interface-guidelines/icons) · [Materials](https://developer.apple.com/design/human-interface-guidelines/materials) · [Inclusion](https://developer.apple.com/design/human-interface-guidelines/inclusion) · [CN 入口](https://developer.apple.com/cn/design/human-interface-guidelines) · [Dos & Don’ts](https://developer.apple.com/design/tips/) · 经典三主题（Clarity / Deference / Depth，iOS HIG 归档）  
**日期：** 2026-10-09（Asia/Shanghai）

> 今日 Apple 文档页多为 JS 渲染；正文以官方英文全文镜像 / Wayback 归档与 Dos & Don’ts 为准，CN 站作入口确认。原则取「精神」，实现取「墨水可落地」。

---

## 1. 三大设计主题（白话）

经典 iOS HIG 把平台气质收成三句话。RickyOS 要保留**意图**，换掉**手段**。

| 主题 | 白话 | Apple 原手段（参考） | RickyOS 等价手段 |
|---|---|---|---|
| **Clarity（清晰）** | 一眼能看懂、字读得清、图标不糊、装饰不抢戏；功能看得见。 | 字号阶梯、精确图标、留白、用色点出重点、足够对比 | 5 级字号 + 真字重；Lucide 单线宽；边距 40；`ink`/`ink-2` 才承载可读信息；选中=反白 |
| **Deference（克制 / 让路）** | 界面是仆人，内容是主人；chrome 不抢书页与封面。 | 内容铺满、半透明/模糊暗示下层、少用边框阴影 | **阅读页极简顶栏页脚**；首页「继续阅读」最大；控件靠近所改内容；说明文字只命名不讲解（三项决定 §2） |
| **Depth（层次）** | 让人感到「谁在上、谁在下、现在在哪」，而不是平面糊成一团。 | 图层、毛玻璃、视差、转场动画 | **对比 + 间距 + 字号/字重 + 反白块** 制造层级；Tab/卡片/列表用形状与位置分区，**不用模糊/透明/阴影** |

附带六条设计原则（Aesthetic Integrity / Consistency / Direct Manipulation / Feedback / Metaphors / User Control）对 RickyOS 的速译：

- **审美诚实：** 阅读器该安静；首页可以有小狗探头的品牌趣味——但阅读正文区禁止角色插画。
- **一致性：** 一种选中、一套圆角、一种图标族、中文同一套措辞。
- **直接操作：** 点封面就进书、点进度就知道页；手势结果必须用**可见的下一帧**确认（DU ≈230 ms）。
- **反馈：** 按下=反白；长任务用静态文案或确定进度条，**不转圈**。
- **隐喻：** 纸、墨、书卡、探头小狗——隐喻要尊重用户（小狗不拿书、不当「正在读书的狗」）。
- **用户控制：** 破坏性操作可取消；传书入口只列真实可用方式。

---

## 2. 该学进 RickyOS 的具体点（已翻译成墨水语）

### 2.1 Clarity → 可读与可点

- **字要够大、够重。** HIG：避免 Ultralight/Thin/Light；默认可读尺寸（iOS 默认约 17pt，最小约 11pt）。我们：body 25 / caption 21，禁止合成粗体，用真 Medium/Bold。
- **对比是底线。** Dos & Don’ts：字与底要有充分对比。我们：必读文字 ≤ `ink-2`（L4）；`mute` 只给禁用/占位；灰字不叠灰底。
- **触控目标够大。** HIG：≥ 44×44 pt。我们因视差+刷新延迟取 **88×88 px**；视觉可更小，命中区外扩。
- **对齐表达关系。** 控件靠近所修改的内容；列表主文一行、标题最多两行；分区靠留白与字号，不靠框套框。

### 2.2 Deference → 内容优先

- **内容占屏，chrome 退后。** 阅读页：安静顶栏 + 页脚进度；无小狗。首页：继续阅读卡主导，座右铭安静一行。
- **标签命名，不讲解。** 与 HIG「清晰标签」同向：分区标题、状态词、单位 / 短 caption；去掉「点这里继续」类旁白。空首页传书：**方法名**（或名 + 一句 caption），不要教程段落。
- **一屏一个主行动。** 空状态给一个清晰下一步（开始传书）；首页主卡是继续阅读；设置里每卡一件事。

### 2.3 Depth / Materials → 用「墨水层级」代替玻璃

Apple Materials / Liquid Glass 的**目的**是：功能层与内容层可分、前景可读、背景有上下文。墨水屏没有 blur / translucency / vibrancy。翻译表：

| Apple 手段 | 意图 | RickyOS 落地 |
|---|---|---|
| 毛玻璃 / Liquid Glass 浮在内容上 | 导航与内容分层 | Tab 顶线 + 墨水胶囊；内容区在 y144–1104；**不透明、不透底** |
| 厚/薄 material | 对比 vs 保留上下文 | 大块只用 `paper`/`tint`；细线 `line`；选中整块 `ink` |
| Vibrancy 多级 label | 主/次/三/四级文字 | `ink` → `ink-2` → `mute`（三级足够；禁用才 mute） |
| 转场表现「进一层」 | 位置感 | **整页替换 + 局部 DU**；禁止过渡动画；返回用页眉，不靠视差 |
| 内容从栏下「透出」 | 暗示可滚 | 列表/栅格自然裁切；进度用实心条，不用半透明轨 |

**Depth 在 RickyOS = 对比 × 间距 × 字号字重 × 反白形状。** 不是 z 轴特效。

### 2.4 布局与模式（Lists / Tabs / Empty / Progress）

- **Tab：** 平级大类持久切换（首页 / 书库 / …）。当前项用胶囊反白；标签文字保持可读；五等分整列可点。
- **列表：** 可扫读；主文 + 可选副文；焦点整行反白；「已选值」用行尾对勾，**不等于**焦点反白。
- **空状态：** 说明「为什么没有」+ 提供相关下一步（ContentUnavailable 精神）。我们：headline + ≤2 行 body + 允许的传书入口；小狗按探头规则引导，不替代主行动按钮的清晰性。
- **进度：** 能量化用确定条；不能量化用静态「正在…」文案；位置固定；能量化时尽量准确；**不要假装前进的假条**；墨水上**禁止旋转 activity indicator**。
- **加载 / 空 / 错互斥：** 加载完且为空 → 换空状态，不要转圈叠空页。

### 2.5 颜色（语义，不是色相）

- 颜色语义化：同一颜色不要多重含义。我们只有 6 名灰——**选中绝不用灰**，只用反白。
- **不只靠颜色区分。** 形状、位置、图标、文字并行。反白本身是形状+对比，已满足「Differentiate Without Color Alone」精神。
- 文化：红在中文语境常偏正面；我们几乎不用「色相」，靠墨/纸即可，少引入文化歧义。

### 2.6 图标

- 图标精确、与相邻文字字重匹配；意义图标要够粗以免 1-bit 断笔。
- 我们：Lucide 几何 + 整数线宽；选中不换「实心版图标」，由反白承载；禁圆形外框第二套语言。

### 2.7 包容（Inclusion）→ 品牌与中文

- 避免刻板印象；让用户定义自己。已定：**小狗不拿书、不做「正在读书的狗」**——用户不是宠物。
- 语言直接、本地化自然：问候带昵称；座右铭用户可改；空状态中性、可行动。
- 插画服务欢迎感，不服务嘲讽或角色错位。

---

## 3. 不可照搬的清单（Copy 会翻车）

| 勿抄 | 原因 |
|---|---|
| **Liquid Glass / blur / 半透明 / vibrancy** | 墨水无实时合成；DU 只推黑白；玻璃=脏边+残影 |
| **SF Symbols 多色 / hierarchical / palette 模式** | 无彩色；用单色描边即可；选中靠反白不靠色阶 |
| **Dynamic Type 全档极端放大** | Flash 与版面锁死；提供固定 5 级阶梯 + 阅读器字号设置即可，不做系统级无限缩放 |
| **连续圆角（continuous / squircles）当默认** | 1-bit 台阶难看；我们只有 0 / 12 / 24 / pill |
| **阴影、渐变、大面积中灰底** | 残影与「廉价棋盘」；大面积只 `paper`/`tint` |
| **旋转 Spinner / 过渡动画 / 视差** | 刷新贵；反馈=反白一帧；长任务用文案或确定条 |
| **系统蓝强调色 / tint fill 当主按钮** | 无色；主行动=墨底纸字或反白块 |
| **分组表的多层 tint 背景叠叠乐** | 用留白与字号分区，一张纸感到底 |
| **英文 SF 字偶与 tracking 表** | 中文用思源/Noto；拉丁跟同一体系；不搬 SF tracking |
| **把用户画成正在被「训练」的狗** | 违反 Inclusion 与已定品牌红线 |

---

## 4. 对照已定方案的短检查表

对象：home peek2 · library · reader · settings · empty（及 board 总览）。

| 项 | 现状 | HIG 对齐？ | 留意 |
|---|---|---|---|
| 纸底墨文、六名色 | 已定 | ✅ Clarity 对比 | 静止帧勿让 `mute` 承载必读信息 |
| 选中=反白一种语言 | 已定 | ✅ 清晰反馈 / 无色可辨 | 勿再加描边加粗第二种焦点 |
| 首页继续阅读最大 + 小狗探头 | peek2 已批 | ✅ Deference（内容主）+ 品牌 | 探头不挡主按钮命中；阅读页无狗 |
| Tab 墨水胶囊 | 已定 | ✅ Tab 平级导航 | 胶囊命中 ≥88；当前态一眼可辨（1-bit 亦成立） |
| 书库芯片反白 + 封面托盘 | 已定 | ✅ 列表/栅格可扫读 | 筛选芯片文案短；托盘勿吃掉书名对比 |
| 阅读安静顶栏页脚 | 已定 | ✅ Deference 典范 | 进度用确定条/页码，忌装饰性 chrome |
| 设置 2×3 卡 + 列表行 | 已定 | ✅ 一致控件 | 每卡一事；列表「已选」用对勾 |
| 空首页 headline+短文+传书入口 | 已定 | ✅ Empty + 下一步 | 入口**只**列固件已支持方式；勿教程墙 |
| 说明文字「命名不讲解」 | 三项决定 | ✅ Clarity 文案 | 新屏评审时先删旁白 |
| 应用瓷砖描边 + 选中反白 | 三项决定 | ✅ 图标一致 | 无圆框、无 r28 |
| 触控 88、边距 40、圆角 3 档 | token | ✅ Layout / hit target 精神 | 新控件先查命中再画好看 |
| 无玻璃/无动画 | 规范原则 | ✅ 已主动拒绝 Depth 特效 | 任何「透一下」「弹一下」提案直接否 |

**总判：** 已定方案与 HIG **精神高度同向**；主要风险是后续稿件**偷偷引入**玻璃感、灰选中、第二种焦点、或空状态说明书。

---

## 5. 可写入 RickyOS 工艺的行动规则（7 条）

每条一句，供达芬奇与评审直接勾选：

1. **内容压过 chrome：** 书页、封面、继续阅读优先占视觉重量；栏、线、图标只够导航。  
2. **一屏一个主行动：** 每屏只强调一个下一步（继续读 / 传书 / 保存），其余降为次级。  
3. **触控先于美观：** 可点区域 ≥ 88×88；视觉再小也要外扩命中，相邻间隔 ≥ 8。  
4. **标签命名，不解释：** 控件与分区用名词/短状态；禁止按钮旁再写操作说明书。  
5. **层级靠墨与空，不靠特效：** 用字号字重、留白、反白块表达上下级；禁止模糊、透明、阴影、渐变。  
6. **一种选中语言：** 焦点/当前 = 承载面转墨、内容转纸；已选值用对勾，不用第二种描边。  
7. **空与忙要诚实：** 空状态说清原因并给真实可用下一步；进度能量化用条、不能量化用静态文案——不转圈、不假进度。

（可选第 8 条，品牌专用）**小狗是欢迎者不是用户替身：** 探头可以俏皮，但不拿书、不占领阅读正文、不暗示用户是宠物。

---

## 6. 给达芬奇的使用方式

- 改稿 / 新屏前：过一遍 §5 规则 + §4 留意列。  
- 争议时：先问「这是 Clarity、Deference，还是在模仿 iOS 皮？」——皮不要，骨要。  
- 本笔记**不**改 `RickyDesignTokens.h`、不改 PNG、不改固件；若某条要升格进 `RickyOS_设计规范.md`，另开变更，由 Rick 点头。

---

## 7. 源与局限

| 源 | 用途 |
|---|---|
| iOS HIG Themes（Wayback） | Clarity / Deference / Depth 原文 |
| apple-docs 镜像：Color / Typography / Materials | 当代 Foundations 细则 |
| developer.apple.com/design/tips | 44pt、对比、对齐等 Dos & Don’ts |
| Progress / ContentUnavailable 文档与社区整理 | 进度与空状态模式 |
| CN HIG 入口 | 确认官方中文导航存在；细则仍以英文全文为准 |
| 本仓库已定视觉方案 | 对照清单事实源 |

局限：官方现站多为客户端渲染，单次抓取正文不完整；若 Apple 大幅改写 Foundations，以开发者站点更新为准，再修订本笔记 §2–§3 翻译表即可。
