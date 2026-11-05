#!/usr/bin/env python3
"""Generate docs/design-system/rickyos-1.2-mockups.html.

Usage: python3 docs/design-system/mockup-src/gen.py docs/design-system/rickyos-1.2-mockups.html
Screens are plain HTML/CSS at 684x1216 using the tokens of docs/design-system.md;
icons come from the lucide set in freeink-sdk.
"""
import base64, re, sys
from pathlib import Path

HERE = Path(__file__).parent
LUCIDE = HERE.parents[2] / 'freeink-sdk/libs/assets/Icons/lucide/icons'
OUT = Path(sys.argv[1])

def b64(p):
    return 'data:image/png;base64,' + base64.b64encode((HERE / p).read_bytes()).decode()

DOG, AVATAR = b64('dog.png'), b64('avatar.png')

def icon(name, size=40):
    src = (LUCIDE / f'{name}.svg').read_text()
    body = re.search(r'>\s*(.*)</svg>', src, re.S).group(1)
    stroke = {24: 2, 32: 3, 40: 3, 56: 4}.get(size, 3)
    sw = stroke / (size / 24)
    return (f'<svg class="ic" width="{size}" height="{size}" viewBox="0 0 24 24" fill="none" '
            f'stroke="currentColor" stroke-width="{sw:.3f}" stroke-linecap="round" '
            f'stroke-linejoin="round" aria-hidden="true">{body.strip()}</svg>')

def status(center=''):
    c = f'<span class="ink2">{center}</span>' if center else '<span></span>'
    return (f'<div class="sb"><span>09:41</span>{c}'
            f'<span class="bat">93%<i class="batt"><i style="width:84%"></i></i></span></div>')

TABS = [('house', '首页'), ('library', '书库'), ('layout-grid', '应用'), ('folder', '存储'), ('sliders-horizontal', '设置')]

def tabbar(active):
    items = ''.join(
        f'<div class="tab{" on" if i == active else ""}"><span class="pill">{icon(n, 40)}</span><span class="tl">{t}</span></div>'
        for i, (n, t) in enumerate(TABS))
    return f'<nav class="tabbar">{items}</nav>'

def cover(title, fmt, style, w, h, vertical=False):
    t = ''.join(f'<span>{ch}</span>' for ch in title) if vertical else title
    return (f'<div class="cv cv-{style}{" vert" if vertical else ""}" style="width:{w}px;height:{h}px">'
            f'<div class="cvt">{t}</div><div class="cvf">{fmt}</div></div>')

def chev():
    return f'<span class="chev">{icon("chevron-right", 24)}</span>'

def row(label, value='', ic=None, sel=False, sub=None, toggle=None):
    lead = f'<span class="rlead">{icon(ic, 32)}</span>' if ic else ''
    text = (f'<span class="rtext"><span class="rmain">{label}</span><span class="rsub">{sub}</span></span>'
            if sub else f'<span class="rtext"><span class="rmain">{label}</span></span>')
    if toggle is not None:
        trail = f'<span class="tog{" on" if toggle else ""}"><i></i></span>'
    else:
        trail = (f'<span class="rval">{value}</span>' if value else '') + chev()
    return f'<div class="row{" sel" if sel else ""}{" two" if sub else ""}">{lead}{text}{trail}</div>'

def screen(key, title, body):
    return f'<div class="dev" data-key="{key}"><div class="scr">{body}</div></div>'

# ---------------------------------------------------------------- screens
home = status() + f'''
<header class="hhead">
  <div><div class="disp">10月8日</div><div class="cap ink2">星期四 · 连续阅读 12 天</div>
  <div class="body greet">Ricky，今天也读几页。</div></div>
  <img class="avatar" src="{AVATAR}" alt="">
</header>
<div class="peek"><img class="dog" src="{DOG}" alt="">
<section class="card cont">
  {cover('瓦尔登湖', 'EPUB', 'ink', 160, 240, True)}
  <div class="cinfo">
    <div class="cap ink2">正在读</div>
    <div class="head">瓦尔登湖</div>
    <div class="cap ink2">亨利·梭罗</div>
    <div class="prog"><i style="width:62%"></i></div>
    <div class="split cap"><span>第 9 章 · 62%</span><span class="ink2">约 2 小时</span></div>
    <div class="btn primary">继续阅读</div>
  </div>
</section></div>
<div class="g2 stats">
  <section class="card stat"><div class="cap ink2">今日阅读</div>
    <div class="num"><b>32</b><span class="cap">分钟</span></div>
    <div class="prog sm"><i style="width:71%"></i></div><div class="cap ink2">目标 45 分钟</div></section>
  <section class="card stat"><div class="cap ink2">连续阅读</div>
    <div class="num"><b>12</b><span class="cap">天</span></div>
    <div class="spark">{"".join(f'<i style="height:{h}px"></i>' for h in (10,18,8,22,16,26,32))}</div>
    <div class="cap ink2">近 7 天</div></section>
</div>
<div class="sechd"><span class="head">最近翻过</span><span class="cap">全部 ›</span></div>
<div class="recent">
  <div class="rb">{cover('小王子', 'EPUB', 'paper', 136, 204)}<div class="cap one">小王子</div><div class="cap ink2">EPUB · 88%</div></div>
  <div class="rb">{cover('浮生六记', 'EPUB', 'ink', 136, 204)}<div class="cap one">浮生六记</div><div class="cap ink2">EPUB · 48%</div></div>
  <div class="rb">{cover('人间词话', 'TXT', 'band', 136, 204)}<div class="cap one">人间词话</div><div class="cap ink2">TXT · 24%</div></div>
  <div class="rb">{cover('诗经', 'EPUB', 'frame', 136, 204)}<div class="cap one">诗经</div><div class="cap ink2">EPUB · 未读</div></div>
</div>
<div class="motto cap ink2">慢一点，读几页。</div>
''' + tabbar(0)

lib_items = [('瓦尔登湖', 'EPUB · 62%', 'ink', True), ('小王子', 'EPUB · 88%', 'paper', True),
             ('浮生六记', 'EPUB · 48%', 'ink', True), ('人间词话', 'TXT · 24%', 'band', False),
             ('诗经', 'EPUB · 未读', 'frame', False), ('阅读与生活', 'EPUB · 12%', 'tint', False)]
library = status() + '''<header class="phead"><span class="title">书库</span><span class="cap ink2">6 本</span></header>
<div class="chips"><span class="chip on">全部</span><span class="chip">在读</span><span class="chip">未读</span>
<span class="ibtn">''' + icon('sliders-horizontal', 32) + '''</span></div><div class="g3 grid">''' + ''.join(
    f'<div class="gb{" plate" if i == 1 else ""}">{cover(t, m.split(" ")[0], s, 188, 282, v)}'
    f'<div class="cap one strong">{t}</div><div class="cap ink2">{m}</div></div>'
    for i, (t, m, s, v) in enumerate(lib_items)) + '</div>' + tabbar(1)

reader = status('瓦尔登湖 · 第 9 章') + '''<article class="page">
<p>我步入丛林，因为我希望生活得有意义，我希望活得深刻，汲取生命中所有的精华，把非生命的一切都击溃，以免当我生命终结时，发现自己从来没有活过。</p>
<p>大部分人，即使是在这个比较自由的国度里，也仅仅由于无知和错误，满载着虚构的忧虑，忙不完的粗活，却不能采集生命的美果。</p>
<p>其实，人不必如此劳碌。他只需付出与收获相当的劳动。何必为了生活，把自己弄成一架机器？</p>
<p>简化，再简化。一天只做两三件事，而不是上百件。算账简化了，人生也简化了。</p>
<p>（以下为示意文字）清晨的湖面最安静，风还没有起来，岸边的树影一动不动。走到水边坐一会儿，什么也不想，只是看着光一点点亮起来。</p>
</article>
<footer class="rfoot"><div class="prog"><i style="width:62%"></i></div>
<div class="split cap ink2"><span>128 / 206</span><span>62%</span></div></footer>'''

menu = status() + f'''<header class="subhead"><span class="back">{icon('chevron-left', 32)}</span>
<span class="head">瓦尔登湖</span><span class="ibtn right">{icon('house', 32)}</span></header>
<div class="cap ink2 subcap">第 9 章 · 128 / 206 · 62%</div>
<div class="g4 quick">
  <div class="qt sel">{icon('list', 40)}<span class="cap">目录</span></div>
  <div class="qt">{icon('bookmark-plus', 40)}<span class="cap">加书签</span></div>
  <div class="qt">{icon('a-large-small', 40)}<span class="cap">文字</span></div>
  <div class="qt">{icon('book-a', 40)}<span class="cap">查词</span></div>
</div>
<div class="list">
{row('夜间模式', ic='moon', toggle=False)}
{row('阅读方向', '竖屏', 'rotate-cw')}
{row('自动翻页', '关', 'timer')}
{row('跳转到进度', '62%', 'percent')}
{row('书签', '3 个', 'bookmark')}
{row('同步进度', '', 'refresh-cw')}
{row('更多', '截屏、二维码、缓存', 'list-tree')}
</div>'''

settings = status() + f'''<header class="phead"><span class="title">设置</span></header>
<section class="card prof"><img class="avatar sm" src="{AVATAR}" alt="">
<div class="ptext"><div class="head">Ricky</div><div class="cap ink2">头像、昵称和给自己的话</div></div>{chev()}</section>
<div class="g2 cards">
  <div class="scard sel">{icon('tablet', 40)}<span class="head">显示与待机</span></div>
  <div class="scard">{icon('book-open', 40)}<span class="head">阅读</span></div>
  <div class="scard">{icon('wifi', 40)}<span class="head">网络与同步</span></div>
  <div class="scard">{icon('type', 40)}<span class="head">字体管理</span></div>
  <div class="scard">{icon('folder', 40)}<span class="head">书库与文件</span></div>
  <div class="scard">{icon('settings', 40)}<span class="head">系统</span></div>
</div>
<div class="list tight">{row('关于本机', 'RickyOS 1.2.0')}{row('帮助与反馈')}</div>
''' + tabbar(4)

apps_list = [('arrow-up-down', '文件传输'), ('book-open', '微信读书'), ('chart-column', '阅读统计'),
             ('grid-3x3', '五子棋'), ('calculator', '计算器'), ('moon', '屏保')]
apps = status() + '<header class="phead"><span class="title">应用</span></header><div class="g3 apps">' + ''.join(
    f'<div class="app{" sel" if i == 0 else ""}"><span class="tile">{icon(n, 40)}</span><span class="cap">{t}</span></div>'
    for i, (n, t) in enumerate(apps_list)) + '</div>' + tabbar(2)

storage = status() + f'''<header class="phead"><span class="title">存储</span><span class="ibtn txt">{icon('refresh-cw', 24)}<span class="cap">刷新</span></span></header>
<section class="card cap-card"><div class="ring"><svg viewBox="0 0 88 88" width="88" height="88"><circle cx="44" cy="44" r="38" fill="none" stroke="var(--ink)" stroke-width="2"/><circle cx="44" cy="44" r="38" fill="none" stroke="var(--ink)" stroke-width="8" stroke-dasharray="16.7 238.8" transform="rotate(-90 44 44)" stroke-linecap="round"/></svg><span class="cap">7%</span></div>
<div class="ptext"><div class="head">SD 卡</div><div class="cap ink2">可用 117 GB，共 126 GB</div></div>{chev()}</section>
<div class="g2 cards">
  <div class="scard sel">{icon('book-open', 40)}<span class="split"><span class="head">书籍</span><span class="cap">6</span></span></div>
  <div class="scard">{icon('type', 40)}<span class="split"><span class="head">字体</span><span class="cap">2</span></span></div>
  <div class="scard">{icon('image', 40)}<span class="split"><span class="head">图片</span><span class="cap">0</span></span></div>
  <div class="scard">{icon('download', 40)}<span class="split"><span class="head">下载</span><span class="cap">1</span></span></div>
</div>
<div class="list tight">{row('浏览全部文件', '', 'folder')}</div>
''' + tabbar(3)

files = status() + f'''<header class="subhead"><span class="back">{icon('chevron-left', 32)}</span>
<span class="head">SD 卡</span></header><div class="cap ink2 subcap">/</div>
<div class="list">
{row('books', ic='folder', sub='文件夹 · 6 项', sel=True)}
{row('downloads', ic='folder', sub='文件夹 · 1 项')}
{row('fonts', ic='folder', sub='文件夹 · 2 项')}
{row('images', ic='folder', sub='文件夹 · 空')}
{row('瓦尔登湖.epub', ic='book', sub='EPUB · 1.2 MB')}
{row('一本名字特别长的…（第三版）.epub', ic='book', sub='EPUB · 3.4 MB')}
{row('readme.txt', ic='file-text', sub='TXT · 2 KB')}
</div>'''

empty = status() + f'''
<header class="hhead">
  <div><div class="disp">10月8日</div><div class="cap ink2">星期四</div>
  <div class="body greet">Ricky，先放一本书进来。</div></div>
  <img class="avatar" src="{AVATAR}" alt="">
</header>
<div class="peek"><img class="dog" src="{DOG}" alt="">
<section class="card emptyc">
  <div class="drop">{icon('plus', 56)}</div>
  <div class="einfo"><div class="head">放进第一本书</div><div class="body ink2">书库还是空的。</div>
  <div class="btn primary">开始传书</div></div>
</section></div>
<div class="sechd"><span class="head">可以这样传书</span></div>
<div class="list tight">
{row('浏览器传书', '', 'globe')}
{row('WebDAV / Calibre', '', 'folder-sync')}
{row('创建热点', '', 'radio-tower')}
{row('SD 卡 /books', '', 'folder')}
{row('微信读书', '', 'book-open')}
</div>''' + tabbar(0)

poweroff = f'''<div class="off"><img class="avatar xl" src="{AVATAR}" alt="">
<div class="title">RickyOS</div><div class="body ink2">阅读，自有节奏。</div></div>
<div class="offcap cap">已关机 · 长按侧键开机</div>'''

SCREENS = [
    ('home', '首页', home, [
        '统计卡把"目标 45 分钟"做成小进度条加一行字，一眼能看出完成了多少；目标在阅读统计设置里改。',
        '连续阅读卡的柱状图标明"近 7 天"，不让图形自己猜。',
        '最近翻过一排 4 本靠左排，比老师稿多一本；元信息只写"格式 · 进度"，字号不小于 21 px。',
        '座右铭和次要文字一律用深灰（ink-2），不用中灰，快刷时也看得清。',
        '主按钮用 12 圆角，和规范的按钮配方一致；全屏只有它一个墨底按钮。']),
    ('library', '书库', library, [
        '三栏网格不变；焦点封面放在墨色托盘上，书名和进度跟着反白。',
        '无封面的书用 6 种生成模板之一（墨底竖排、纸底竖排、底部墨条、细框、浅灰底），按书名固定。',
        '筛选图标按钮的点按区补足 88 px。']),
    ('reader', '阅读', reader, [
        '顶栏和页脚安静，只留时间、书名章节、电量和进度。',
        '正文用用户选的阅读字体（这里用思源宋体示意），不出现小狗。']),
    ('menu', '阅读菜单', menu, [
        '固件现在的菜单有 15 项，"文字设置"排在最后。改成上面 4 个常用的大按钮：目录、加书签、文字、查词。',
        '夜间模式是开关，用开关控件，不用方框勾选。',
        '截屏、二维码、删除图书缓存这类少用的收进"更多"；删除缓存只在里面出现，并要确认。',
        '右上角的房子图标是回首页，给手势一个看得见的替代。']),
    ('settings', '设置', settings, [
        '资料卡说明改成"头像、昵称和给自己的话"，座右铭就在这里改。',
        '关于本机显示真实版本号 RickyOS 1.2.0（老师稿里的 27.10 是示例）。',
        '2×3 设置卡选中整卡反白，每张卡只做一件事。']),
    ('apps', '应用', apps, [
        '应用名用固件里的真实名称："待机显示"改为"屏保"（1.1.3 已定）。',
        '瓷砖 2 px 描边、12 圆角、纸底；选中时连同名字一起反白。']),
    ('storage', '存储', storage, [
        '容量卡标题用"SD 卡"，不叫"本机存储"：书和文件都在卡上。',
        '底部加"浏览全部文件"入口，进入文件页；分类卡只是快捷入口。']),
    ('files', '文件', files, [
        '双行列表：名字 + "类型 · 大小"。文件夹写"文件夹 · N 项"，不写"目录"。',
        '长文件名中间省略，保留开头和扩展名。']),
    ('empty', '空首页', empty, [
        '放书占位从中灰虚线框改成 2 px 墨线框加号：老师稿里这个框在快刷时整个消失了。',
        '传书方式只写方法名，一行一个，点按区 96 px；"TF 卡"改为"SD 卡"。',
        '空状态只有一个主按钮"开始传书"。']),
    ('poweroff', '关机画面', poweroff, [
        '老师稿叫"休眠"、写"按电源键唤醒"。固件里待机和关机是两件事：待机画面由屏保设置决定，这一屏是关机后留在屏上的画面。',
        '底部写"已关机 · 长按侧键开机"，和 1.1.3 的关机提示一致。',
        '不出现探头小狗。']),
]

GLOBAL = [
    ('字号', '界面文字不小于 21 px。老师稿里状态栏、封面元信息、列表副标题都在 16 到 18 px，按 Apple 的最小字号换算偏小，统一提到 21 px。'),
    ('次要文字', '只用深灰（ink-2，对比 9.7:1）。中灰对比只有 3.5:1，只留给禁用和占位。'),
    ('快刷', '每个元素都要在黑白快刷里看得见。可以点上方"快刷（黑白）"切换，看每屏在 1-bit 下的样子。'),
    ('真实内容', '应用名、菜单项、版本号、"SD 卡"都取自固件现状，不用示例词。'),
    ('一屏一主行动', '首页"继续阅读"、空首页"开始传书"是唯一的墨底按钮，其他都是卡片、列表或次按钮。'),
]

css = (HERE / 'board.css').read_text()
js = (HERE / 'board.js').read_text()
cards = ''.join(
    f'<figure class="shot" id="{k}"><div class="frame">{screen(k, t, b)}</div>'
    f'<figcaption><h3>{t}</h3><ul>{"".join(f"<li>{n}</li>" for n in notes)}</ul></figcaption></figure>'
    for k, t, b, notes in SCREENS)
glob = ''.join(f'<div class="gi"><dt>{a}</dt><dd>{b}</dd></div>' for a, b in GLOBAL)
html = f'''<title>RickyOS 1.2 设计稿</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Noto+Sans+SC:wght@400;500;700&family=Noto+Serif+SC:wght@400&display=swap">
<style>{css}</style>
<svg width="0" height="0" style="position:absolute" aria-hidden="true"><filter id="onebit" color-interpolation-filters="sRGB">
<feColorMatrix type="matrix" values="0.2126 0.7152 0.0722 0 0  0.2126 0.7152 0.0722 0 0  0.2126 0.7152 0.0722 0 0  0 0 0 1 0"/>
<feComponentTransfer><feFuncR type="discrete" tableValues="0 0 0 0 1"/><feFuncG type="discrete" tableValues="0 0 0 0 1"/><feFuncB type="discrete" tableValues="0 0 0 0 1"/></feComponentTransfer>
</filter></svg>
<main class="wrap">
<header class="top">
  <p class="eyebrow">RickyOS 1.2 · 684 × 1216 · 16 级灰度墨水屏</p>
  <h1>我理解后的十屏</h1>
  <p class="lede">以老师 10 月 9 日的第二版为底，按设计规范和 Apple 人机界面指南的准则重画。下面每屏旁边列出和老师稿不同的地方。书名、进度、统计都是示例数据。</p>
  <div class="mode" role="group" aria-label="刷新模式">
    <button type="button" id="m-rest" class="on" aria-pressed="true">静止帧（16 级灰度）</button>
    <button type="button" id="m-bw" aria-pressed="false">快刷（黑白）</button>
  </div>
</header>
<section class="global"><h2>全局调整</h2><dl>{glob}</dl></section>
<section class="shots">{cards}</section>
<footer class="foot">规范原文：仓库 docs/design-system.md（第 15 节为交互与文案准则）；数值：src/components/RickyDesignTokens.h。</footer>
</main>
<dialog id="zoom"><button type="button" class="close" id="zclose">关闭</button><div class="zbox" id="zbox"></div></dialog>
<script>{js}</script>
'''
OUT.write_text(html)
print('wrote', OUT, len(html))
