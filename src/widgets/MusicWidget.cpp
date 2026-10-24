#include "widgets/MusicWidget.h"

#include "core/AppSettings.h"
#include "core/lyricfetcher.h"
#include "platform/MediaVolume.h"
#include "ui/CoverView.h"
#include "ui/MarqueeLabel.h"
#include "ui/UiStyle.h"
#include "widgets/LyricsView.h"

#ifdef QDW_HAS_GSMTC
#  include "core/globalmediasessionlistener.h"
#endif

#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>

// MusicWidget 实现。数据源只有 GSMTC 回调，界面完全是「被动渲染 + 本地补时」：
// onTick 用 QElapsedTimer 在两次上报之间推算进度，所以 seek/切歌后必须同步重置
// m_posBase 与 m_posClock，否则进度条会往前/往后瞬跳。
// 布局分三档：underCover（封面在上、歌词在右）、lyricOverInfo（封面独占左列）、兜底窄版式；
// 尺寸档位全部离散，由 metrics() 单点计算，applyMetrics() 是唯一的应用入口。
// 没有 GSMTC 的构建（非 MSVC）所有 #ifdef 分支都要保持可用，不能只留空壳。

namespace
{

/**
 * 毫秒转「分:秒」。
 * @param ms 毫秒；<=0（未知时长、直播流）统一显示 0:00
 * @return 形如 3:07 / 12:45 的文本，秒恒为两位，分钟不补零
 */
QString formatTime(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("0:00");
    const qint64 total = ms / 1000;
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

/**
 * 给弹出菜单套上全局主题样式。
 * @param m 目标菜单（含自建的右键子菜单与音量面板）
 * 之所以逐个 setStyleSheet：QMenu 是顶层窗口，不会继承父控件的样式表。
 */
void styleMenu(QMenu *m)
{
    const AppSettings &st = AppSettings::get();
    m->setFont(st.uiFont(13));
    m->setStyleSheet(QStringLiteral(
                         "QMenu{background:%1;border:1px solid %2;border-radius:9px;padding:6px;}"
                         "QMenu::item{padding:6px 22px 6px 10px;border-radius:6px;color:%3;}"
                         "QMenu::item:selected{background:%4;}"
                         "QMenu::item:disabled{color:%5;}"
                         "QMenu::separator{height:1px;background:%2;margin:5px 8px;}")
                         .arg(st.menuBg().name(QColor::HexArgb),
                              st.borderColor().name(QColor::HexArgb),
                              st.textColor().name(),
                              st.accent().name(),
                              st.subTextColor().name()));
}

/**
 * 把 cloudmusic.exe / AUMID 之类的标识翻成人看的名字。
 * @param appId 会话标识，大小写不敏感；可能是裸模块名（cloudmusic）、
 *              带扩展名（cloudmusic.exe）或 UWP 的 AUMID（含 '_' 与 '!'）
 * @return 中文/品牌名；不认识或 appId 为空时返回空串，由调用方退回播放器自报名
 */
QString friendlyApp(const QString &appId)
{
    // 键都是小写；命中后返回中文/品牌名。查不到就返回空，由调用方退回播放器自报名
    static const QHash<QString, QString> map = {
        { QStringLiteral("cloudmusic"), QStringLiteral("网易云音乐") },
        { QStringLiteral("qqmusic"), QStringLiteral("QQ音乐") },
        { QStringLiteral("kugou"), QStringLiteral("酷狗音乐") },
        { QStringLiteral("kugoumusic"), QStringLiteral("酷狗音乐") },
        { QStringLiteral("kuwo"), QStringLiteral("酷我音乐") },
        { QStringLiteral("kuwomusic"), QStringLiteral("酷我音乐") },
        { QStringLiteral("migu"), QStringLiteral("咪咕音乐") },
        { QStringLiteral("spotify"), QStringLiteral("Spotify") },
        { QStringLiteral("chrome"), QStringLiteral("Chrome") },
        { QStringLiteral("msedge"), QStringLiteral("Edge") },
        { QStringLiteral("firefox"), QStringLiteral("Firefox") },
        { QStringLiteral("vlc"), QStringLiteral("VLC") },
        { QStringLiteral("wmplayer"), QStringLiteral("Windows Media Player") },
        { QStringLiteral("music.ui"), QStringLiteral("媒体播放器") },
        { QStringLiteral("zunemusic"), QStringLiteral("Groove 音乐") },
    };

    QString key = appId.toLower().trimmed();
    if (key.isEmpty())
        return QString();

    auto lookup = [](const QString &k) { return map.value(k); };
    if (const QString v = lookup(key); !v.isEmpty())
        return v;

    // 形如 cloudmusic.exe：去掉扩展名再查一次
    if (key.endsWith(QStringLiteral(".exe"))) {
        const QString stem = key.chopped(4);
        if (const QString v = lookup(stem); !v.isEmpty())
            return v;
        return QString();
    }

    // UWP 的 AUMID 形如 "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify"：
    // 取 '_' 前的主体查一次；再不行取最后一个 '.' 之后的段（各家的命名不统一，两条都试）
    if (key.contains(QLatin1Char('_'))) {
        const QString head = key.section(QLatin1Char('_'), 0, 0);
        if (const QString v = lookup(head); !v.isEmpty())
            return v;
        const QString app = head.section(QLatin1Char('.'), -1);
        if (const QString v = lookup(app); !v.isEmpty())
            return v;
        return QString();
    }

    return QString();
}

/**
 * 把标准图标按主题色重新着色（QStyle 的图标在暗色玻璃上偏黑，几乎看不见）。
 * @param src   原始 QIcon（通常来自 QStyle::standardIcon）
 * @param color 目标颜色，只取其 RGB，alpha 由原图像的 alpha 决定
 * @param px    期望边长（px）；内部夹到 >= 8，因为更小的位图细节会糊成一团
 * @return 着色后的图标；源图标取不出位图时原样返回 src
 */
QIcon tintIcon(const QIcon &src, const QColor &color, int px)
{
    // 先按目标尺寸取位图；小于 8px 时图标细节会糊成一团，兜到 8
    const QPixmap pm = src.pixmap(qMax(8, px), qMax(8, px));
    if (pm.isNull())
        return src;

    // SourceIn：用纯色填满整个矩形，只保留原图 alpha，等于一次性换色且保留抗锯齿边缘
    QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(img.rect(), color);
    p.end();
    return QIcon(QPixmap::fromImage(img));
}

/**
 * 循环图标：两段圆弧 + 两个箭头。
 * @param mode 循环模式，正常取值 0..2（与 RepeatMode 一致）
 *             0=顺序（灰）、1=列表循环（强调色）、2=单曲循环（强调色 + 中间的 1）；
 *             非 0/2 的取值会落到「强调色但不画 1」的分支
 * @return 64×64 的透明底图标；缩放交给 QIcon 按按钮图标尺寸取样
 *
 * 64×64 画布上按角度参数化生成，箭头用切线/法线算三角形顶点，改半径时箭头自动跟随。
 */
QIcon repeatIcon(int mode)
{
    const AppSettings &st = AppSettings::get();
    // 顺序播放不需要强调，用次要文字色；另两种模式说明用户主动开启了循环，用强调色
    const QColor c = mode == 0 ? st.subTextColor() : st.accent();

    constexpr int kSize = 64;
    QPixmap pm(kSize, kSize);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHints(QPainter::Antialiasing);
    const QPen pen(c, 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const QRectF circle(16, 16, 32, 32);
    const QPointF center = circle.center();
    const double radius = circle.width() / 2.0;

    // 圆周上某个角度（度，0°=正右，顺时针为正）对应的点
    auto ptOn = [&](double deg)
    {
        const double r = qDegreesToRadians(deg);
        return center + QPointF(std::cos(r) * radius, std::sin(r) * radius);
    };
    // 在角度 deg 处放一个箭头：dir=+1 表示沿圆的顺时针切线方向
    auto arrowAt = [&](double deg, double dir)
    {
        const double r = qDegreesToRadians(deg);
        const QPointF tangent(-std::sin(r) * dir, std::cos(r) * dir);
        const QPointF normal(-tangent.y(), tangent.x());
        const QPointF base = ptOn(deg);
        // 三角形顶点沿切线前伸 7、后收 1.5，法线方向各开 5.5，视觉上是个不遮挡弧线的箭头
        QPolygonF tri;
        tri << base + tangent * 7.0 << base - tangent * 1.5 + normal * 5.5
            << base - tangent * 1.5 - normal * 5.5;
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri);
        // 画完箭头恢复描边状态，否则下一段弧线会被无笔刷画成实心
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
    };

    // 上弧（左 → 右）+ 下弧（右 → 左）；QPainter 角度单位是 1/16 度，正值逆时针
    p.drawArc(circle, 200 * 16, 125 * 16);
    p.drawArc(circle, 20 * 16, 125 * 16);
    arrowAt(-55, 1);
    arrowAt(125, 1);

    if (mode == 2) {
        // 单曲循环：圆心压一个 "1"，字号跟着 64px 画布定死，缩放交给 QIcon 处理
        QFont f = st.uiFont(22, QFont::DemiBold);
        p.setFont(f);
        p.setPen(c);
        p.drawText(circle, Qt::AlignCenter, QStringLiteral("1"));
    }

    p.end();
    return QIcon(pm);
}

/**
 * 解析 LRC：支持一行多个时间标签、[offset:]。
 * @param text 歌词原文，按 \n 切行；\r 由 trimmed 顺带去掉
 * @return 按时间升序排列的 (毫秒, 该句文本)；一条带时间的歌词都没解析出来时返回空
 *
 * 失败行为：返回空表示「不是 LRC」，调用方会退回纯文本分支（并自行剥掉方括号标签）。
 * 注意标签正则末尾的 `(?:-\d+)?` 是为了兼容 `[00:00.00-1]` 这种变体（有的歌词站用它标注
 * 「这一行重复播放」），不吞掉的话会被当作正文的一部分画出来。
 */
QVector<QPair<qint64, QString>> parseLrc(const QString &text)
{
    QVector<QPair<qint64, QString>> out;
    static const QRegularExpression offsetRe(QStringLiteral("\\[offset:\\s*(-?\\d+)\\]"));
    static const QRegularExpression tagRe(
        QStringLiteral("\\[(\\d{1,3}):(\\d{1,2})(?:[.:](\\d{1,3}))?(?:-\\d+)?\\]"));

    int offset = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;

        // [offset:] 影响其后所有行（也影响已解析出来的行，但实用歌词文件只写在一处）；
        // 出现多次时后者覆盖前者
        const auto om = offsetRe.match(line);
        if (om.hasMatch()) {
            offset = om.captured(1).toInt();
            continue;
        }

        QList<qint64> times;
        int lastEnd = 0; // 最后一个时间标签在行内的结束位置，其后的部分才是正文
        auto it = tagRe.globalMatch(line);
        while (it.hasNext()) {
            const auto m = it.next();
            const qint64 min = m.captured(1).toLongLong();
            const qint64 sec = m.captured(2).toLongLong();
            QString frac = m.captured(3);
            qint64 ms = 0;
            if (!frac.isEmpty()) {
                // 小数位长度不定：1 位当十分之一秒、2 位当百分之一秒，统一补齐到 3 位毫秒
                if (frac.size() == 1)
                    frac += QStringLiteral("00");
                else if (frac.size() == 2)
                    frac += QLatin1Char('0');
                ms = frac.left(3).toLongLong();
            }
            times << min * 60000 + sec * 1000 + ms;
            lastEnd = m.capturedEnd();
        }
        if (times.isEmpty())
            continue; // [ti:] / [ar:] 这类元信息

        const QString content = line.mid(lastEnd).trimmed();
        if (content.isEmpty())
            continue;
        // 一行多个时间标签 = 同一句重复出现，每个时间点各生成一条
        // offset 为正表示歌词应该提前，所以是「减」，并夹到 0 防止负时间
        for (qint64 t : times)
            out.append({ qMax<qint64>(0, t - offset), content });
    }

    // 用 stable_sort：同一时间戳的多句保持文件里的先后顺序
    std::stable_sort(out.begin(), out.end(),
                     [](const QPair<qint64, QString> &a, const QPair<qint64, QString> &b)
                     { return a.first < b.first; });
    return out;
}

} // namespace

// 构造顺序有依赖：LyricsView 必须最先建（buildUi 里只摆放不创建，applyMetrics 会直接用），
// 歌词抓取器要早于 buildUi 的信号连接，m_applyTimer 必须在任何 resize 触发前就绪。
MusicWidget::MusicWidget(QWidget *parent)
    : WidgetPanel(parent)
{
    // 歌词视图由本类持有并在 relayout 里动态摆放，所以先隐藏，等版式确定后再显示
    m_lyrics = new LyricsView(this);
    m_lyrics->hide();

    // 点歌词某一句 → 从那一句开始播（大档才开放，见 applyMetrics 的 setSeekable）
    connect(m_lyrics, &LyricsView::seekRequested, this, [this](qint64 ms)
    {
#ifdef QDW_HAS_GSMTC
        if (m_listener && m_seekable)
            m_listener->seek(ms);
#endif
        // 与拖动进度条同样的处理：不管播放器支不支持 seek，本地时钟都从目标位置重建，
        // 否则下一次 300ms 心跳会把进度条拽回旧位置，看起来像「点了没反应」
        m_posBase = ms;
        m_posClock.restart();
        m_pos = ms;
        updateTimeLabels();
        if (m_lyrics)
            m_lyrics->setPosition(ms);
    });

    m_lyricFetcher = new LyricFetcher(this);
    connect(m_lyricFetcher, &LyricFetcher::lyricReady, this, &MusicWidget::onLyricReady);

    buildUi();

    // 初始占位文案：此时还没有任何 GSMTC 回调，先给用户一句能看懂的引导语
    m_titleText = QStringLiteral("未在播放");
    m_artistText = QStringLiteral("打开任意音乐软件即可自动显示");
    m_lyricHint = m_artistText;
    m_lyrics->setPlaceholder(m_lyricHint);
    updateTexts();

    // 0ms 单次定时器 = 把重排推迟到当前事件循环末尾，连续 resize 只会触发一次 applyMetrics
    m_applyTimer = new QTimer(this);
    m_applyTimer->setSingleShot(true);
    m_applyTimer->setInterval(0);
    connect(m_applyTimer, &QTimer::timeout, this, [this]
    {
        // 这里再判一次：攒到执行时窗口可能已经停在别的档位，甚至回到原档位
        if (metricsChanged())
            applyMetrics();
    });

    // 100ms 心跳：本地补时的节拍，也决定歌词换句的响应速度。
    // 之前是 300ms，最坏情况歌词要晚 300ms 才切过去，和播放器对上就能看出「组件慢一拍」；
    // 现在压到 100ms，滞后缩到一帧级别。代价只是每秒多 7 次 setValue/setPosition，
    // 两者都是「值没变就直接返回」的轻操作，实测无感。
    m_tick = new QTimer(this);
    m_tick->setInterval(100);
    connect(m_tick, &QTimer::timeout, this, &MusicWidget::onTick);
    m_tick->start();

    // 默认沿用上次选择的播放器（全局记忆），空则跟随系统当前会话
    m_appId = AppSettings::get().musicAppId;
    initSystem();
}

MusicWidget::~MusicWidget()
{
#ifdef QDW_HAS_GSMTC
    // 先断开监听器 → 本组件的连接，避免析构过程中还有排队信号打进来
    if (m_listener)
        disconnect(m_listener, nullptr, this, nullptr);
#endif
}

// ---------------------------------------------------------------- 版式

/**
 * 按当前控件尺寸算出这一档的全部尺寸参数。
 * @return 完整的一档 Metrics；不读也不写任何界面状态（可安全地用于「是否换档」比较）
 *
 * 判定顺序：先按高度分 6 个基础档位，再按宽度做局部删减（时间/循环音量/underCover），
 * 最后在「放不下封面下歌名」的矮版式里按剩余高度压封面。所有阈值都是硬编码常量，
 * 改动时注意 metricsChanged() 只比较了部分字段，新增「会影响布局」的字段必须同步加进去。
 */
MusicWidget::Metrics MusicWidget::metrics() const
{
    Metrics m;
    const int w = width();
    const int h = height();

    // 边距也离散：极窄时收边距比缩字号更划算，字还能保持可读
    m.marginX = w < 200 ? 9 : (w < 300 ? 12 : 16);
    // 标题栏隐藏后没有分隔线需要留白，反而可以给更多内边距
    m.marginY = h < 100 ? 6 : (m_headerHidden ? 12 : 10);

    // ---------------------------------------------------------------- 版式：只有两档
    // 小档（≈ 384×166 那种扁条）：封面在左，右列是「歌名（够高才有）/ 歌词 / 进度 / 控制」；
    // 大档（≈ 380×290 往上，网易云那种）：左边大封面竖跨，右侧上歌名下歌词，底部进度 + 控制。
    // 两档之间用「宽 + 高」双阈值判定 —— 只按高度判会让又窄又高的窗口挤出一个畸形的大档。
    m.underCover = (w >= 320 && h >= 230);
    m.lyricOverInfo = !m.underCover;

    if (m.underCover) {
        // 字号跟着**尺寸**走而不是分档：以内容区高度为尺度线性插值（230 → 800 映射到 0 → 1）。
        // 字号必须是整数 px，所以只有真的跨过 1px 才算「档位变化」触发重排，
        // 重排次数仍然有限，拖动时不会逐像素抖动。
        // 封面是例外：换直径要重新裁圆（最贵的一步），保持离散档位。
        const qreal t = qBound(0.0, (h - 230) / 570.0, 1.0);
        m.titlePx = qRound(18 + 14 * t);   // 18 → 32
        m.artistPx = qRound(11 + 5 * t);   // 11 → 16
        m.lyricPx = qRound(14 + 8 * t);    // 14 → 22
        m.timePx = qRound(10 + 3 * t);     // 10 → 13
        m.btn = qRound(28 + 14 * t);       // 28 → 42
        m.playBtn = m.btn + qRound(6 + 2 * t);
        m.iconPx = qRound(14 + 6 * t);     // 14 → 20
        m.gapX = qRound(14 + 6 * t);
        m.gapY = qRound(6 + 6 * t);
        m.marginX = qRound(14 + 8 * t);
        m.marginY = qRound(12 + 8 * t);

        // 封面：离散档位（重建圆形的成本高，不能连续）；随窗口变大分四档
        m.cover = (h >= 480) ? 180 : (h >= 340 ? 150 : (h >= 280 ? 130 : 110));

        m.showArtist = true;
        m.showProgress = true;
        m.showTimes = true;
        m.showPrevNext = true;
        m.showRepeatVolume = true;
        return m;   // 大档不需要下面那套竖向预算
    }

    // ---------------------------------------------------------------- 小档
    // 版式只有一种；字号同样按尺寸连续插值（小档高度 84 → 230 映射到 0 → 1），
    // 只有「次要控件显不显示」还按阈值判 —— 那是宽度够不够的问题，不是字号问题。
    const bool tiny = (h < 84 || w < 176);
    m.showArtist = !tiny && w >= 230;
    m.showProgress = true;                       // 再小也保留进度条与控制
    m.showTimes = !tiny && w >= 300;
    m.showPrevNext = tiny ? (w >= 150) : true;
    m.showRepeatVolume = !tiny && w >= 268;

    {
        const qreal t = qBound(0.0, (h - 84) / 146.0, 1.0);
        m.titlePx = qRound(12 + 2.6 * t);          // 12 → 15
        m.artistPx = qRound(9 + 2.6 * t);          // 9  → 12
        m.lyricPx = qRound(13 + 3.0 * t);          // 13 → 16
        m.timePx = qRound(9 + 1.5 * t);            // 9  → 11
        m.btn = qRound(20 + 8 * t);                // 20 → 28
        m.playBtn = m.btn + qRound(2 + 2 * t);
        m.iconPx = qRound(11 + 4 * t);             // 11 → 15
        m.gapX = qRound(6 + 6 * t);
        m.gapY = qRound(2 + 3 * t);
        m.marginX = qRound(10 + 6 * t);            // 10 → 16
        m.marginY = qRound(8 + 4 * t);             // 8  → 12
        m.cover = 58;                              // 基准值，下面预算会按高度再压
    }

    // 极窄时再收掉次要控件（时间只是宽度问题，不影响高度）
    if (w < 300)
        m.showTimes = false;
    if (w < 250)
        m.showRepeatVolume = false;

    // 矮版式：进度条与控制**始终保留**。空间不够时靠上面那套连续公式自动缩小，
    // 只有封面仍需离散（重建圆形太贵）。
    if (m.lyricOverInfo) {
        // ---- 竖向预算 ----
        const int progressH = 18;                                // 滑条 + 两端时间同一行
        const int controlsH = m.playBtn + 4;                     // 控制行 = 播放键高 + 留白
        const int coreH = m.marginY * 2 + progressH + controlsH;  // 边距 + 进度 + 控制
        const int rightH = h - coreH;                             // 右列留给歌词的高度

        // 封面：这一档的基准是 58；高度不够才一档档往下压。
        // 仍然全是离散档位 —— 连续公式会让每 1px 缩放都触发圆形封面重建，拖边框时就是抖。
        const int coverRoom = h - m.marginY * 2;
        // 封面最多占一半宽度，否则窄窗口里右列会被挤没
        const int coverMaxByW = qMax(16, (w - m.marginX * 2 - m.gapX) / 2);
        int cov = 16;
        if (coverRoom >= 58 && coverMaxByW >= 58)
            cov = 58;
        else if (coverRoom >= 46 && coverMaxByW >= 46)
            cov = 46;
        else if (coverRoom >= 34 && coverMaxByW >= 34)
            cov = 34;
        else if (coverRoom >= 24 && coverMaxByW >= 24)
            cov = 24;
        m.cover = cov;

        // 小档不显示歌名块（歌名/歌手让位给歌词，没有歌词时由 effectivePlaceholder 兜底显示）。
        // 这里只算歌词能占几整行：留一行给上下两条等权重留白做垂直居中。
        const AppSettings &st = AppSettings::get();
        const int rowH = int(QFontMetricsF(st.uiFont(m.lyricPx, QFont::DemiBold)).height()) + 3;
        const int rowsNoInfo = (rightH - m.gapY * 2) / qMax(1, rowH);
        m.lyricRows = qBound(1, rowsNoInfo - 1, 4);
    }

    return m;
}

// 一次性装配：控件层级、初始尺寸、信号槽。之后的尺寸/可见性变化全部由 applyMetrics
// 负责，这里不再参与。按钮统一 NoFocus + AutoRaise：点完不会留焦点框，也不会把键盘焦点
// 从父面板抢走。
void MusicWidget::buildUi()
{
    setStyleSheet(UiStyle::panelCss());
    const AppSettings &st = AppSettings::get();

    // 初始边距随便给一档，applyMetrics 会在构造末尾用真实档位覆盖
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(14, 10, 14, 10);
    m_grid->setHorizontalSpacing(12);
    m_grid->setVerticalSpacing(4);

    // ---- 旋转封面 ----
    // 这里是固定尺寸控件，构造后保持默认直径；真正的档位尺寸由 applyMetrics 的 setDiameter 驱动
    m_cover = new CoverView(this);

    // ---- 歌名 / 歌手（放在封面下方，超长横向滚动）----
    // 用一个容器包住两行：under 版式里整块要作为一个网格单元放置
    m_info = new QWidget(this);
    auto *infoLay = new QVBoxLayout(m_info);
    infoLay->setContentsMargins(0, 0, 0, 0);
    infoLay->setSpacing(1);

    m_title = new MarqueeLabel(m_info);
    m_title->setTextColor(st.textColor());

    m_artist = new MarqueeLabel(m_info);
    m_artist->setTextColor(st.subTextColor());

    infoLay->addWidget(m_title);
    infoLay->addWidget(m_artist);

    // ---- 歌词（自动换行 + 上下文多行）----
    // 注意：m_lyrics 在构造函数里先建好，这里只负责排版

    // ---- 进度：时间在两端，滑条在中间 ----
    m_progressRow = new QWidget(this);
    auto *progressLay = new QHBoxLayout(m_progressRow);
    progressLay->setContentsMargins(0, 0, 0, 0);
    progressLay->setSpacing(8);

    // objectName 交给样式表选「次要文字」样式，不用单独 setStyleSheet
    m_elapsed = new QLabel(QStringLiteral("0:00"), m_progressRow);
    m_elapsed->setObjectName(QStringLiteral("sub"));
    m_elapsed->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // 范围先给 (0,0)，等拿到时长再设；高度固定 18 与 metrics() 里的 progressH 对应
    m_progress = new QSlider(Qt::Horizontal, m_progressRow);
    m_progress->setRange(0, 0);
    m_progress->setSingleStep(1000); // 1 秒，键盘微调时的手感
    m_progress->setFixedHeight(18);

    m_total = new QLabel(QStringLiteral("0:00"), m_progressRow);
    m_total->setObjectName(QStringLiteral("sub"));
    m_total->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // 滑条拉伸因子 1，两端时间 0：宽度变化全给滑条
    progressLay->addWidget(m_elapsed, 0);
    progressLay->addWidget(m_progress, 1);
    progressLay->addWidget(m_total, 0);

    // ---- 控制按钮：整行居中 ----
    m_controls = new QWidget(this);
    auto *ctl = new QHBoxLayout(m_controls);
    ctl->setContentsMargins(0, 0, 0, 0);
    ctl->setSpacing(10);

    // 统一按钮样式：无边框（靠 hover 绘制）、手型光标、不抢焦点、tooltip 说明功能
    auto makeBtn = [this](const QString &tip)
    {
        auto *b = new QToolButton(m_controls);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    // 创建顺序即界面从左到右的顺序，后面 addWidget 保持一致
    m_repeatBtn = makeBtn(QStringLiteral("循环播放：顺序 → 列表 → 单曲"));
    m_prevBtn = makeBtn(QStringLiteral("上一首"));
    m_playBtn = makeBtn(QStringLiteral("播放 / 暂停"));
    m_nextBtn = makeBtn(QStringLiteral("下一首"));
    m_volumeBtn = makeBtn(QStringLiteral("音量"));

    // InstantPopup：按下立刻弹面板，不需要「先点开再点一下」的两段式
    m_volumeBtn->setPopupMode(QToolButton::InstantPopup);
    // 挂了 menu 的 QToolButton 默认会在右下角画一个下拉小三角（menu-indicator），
    // 这个按钮按下就是弹面板、没有「点箭头才弹」的语义，所以把指示器去掉，
    // 否则图标会被挤偏、多出来的小箭头也很突兀。
    m_volumeBtn->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
    // 按钮本身不处理滚轮，靠 eventFilter 拦下来做音量微调（见 eventFilter）
    m_volumeBtn->installEventFilter(this);

    // 两侧 stretch 相等 → 整组按钮视觉居中，且窗口变宽时按钮不会被拉开
    ctl->addStretch(1);
    ctl->addWidget(m_repeatBtn);
    ctl->addWidget(m_prevBtn);
    ctl->addWidget(m_playBtn);
    ctl->addWidget(m_nextBtn);
    ctl->addWidget(m_volumeBtn);
    ctl->addStretch(1);

    // ---- 音量弹出面板 ----
    // 用 QWidgetAction 把「滑条 + 百分比」塞进菜单，比自建 QDialog 更容易跟随主题
    m_volumeMenu = new QMenu(this);
    styleMenu(m_volumeMenu);
    auto *volumeAction = new QWidgetAction(m_volumeMenu);
    auto *volumeBox = new QWidget(m_volumeMenu);
    auto *volumeLay = new QHBoxLayout(volumeBox);
    volumeLay->setContentsMargins(10, 8, 10, 8);
    volumeLay->setSpacing(8);

    m_volumeSlider = new QSlider(Qt::Horizontal, volumeBox);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(132); // 定宽，避免菜单随百分比文本变宽收窄
    m_volumeValue = new QLabel(QStringLiteral("—"), volumeBox); // 「—」= 还没读到音量
    m_volumeValue->setFont(st.uiFont(11));
    m_volumeValue->setMinimumWidth(34); // 容纳 "100%" 且数字变化时不左右跳
    m_volumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    volumeLay->addWidget(m_volumeSlider);
    volumeLay->addWidget(m_volumeValue);
    volumeAction->setDefaultWidget(volumeBox);
    m_volumeMenu->addAction(volumeAction);
    m_volumeBtn->setMenu(m_volumeMenu);

    // ---- 交互 ----
    connect(m_repeatBtn, &QToolButton::clicked, this, &MusicWidget::cycleRepeat);
    // 三个传输控制全部转发给监听器；没有 GSMTC 时按钮点了没反应（但仍会显示，保持版式稳定）
    connect(m_playBtn, &QToolButton::clicked, this, [this]
    {
#ifdef QDW_HAS_GSMTC
        if (m_listener)
            m_listener->toggle();
#endif
    });
    connect(m_prevBtn, &QToolButton::clicked, this, [this]
    {
#ifdef QDW_HAS_GSMTC
        if (m_listener)
            m_listener->previous();
#endif
    });
    connect(m_nextBtn, &QToolButton::clicked, this, [this]
    {
#ifdef QDW_HAS_GSMTC
        if (m_listener)
            m_listener->next();
#endif
    });

    // 按下期间置 m_seeking，屏蔽外部进度覆盖（否则 300ms 心跳会把滑块拽回去）
    connect(m_progress, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
    connect(m_progress, &QSlider::sliderReleased, this, [this]
    {
        m_seeking = false;
        const qint64 target = m_progress->value();
#ifdef QDW_HAS_GSMTC
        if (m_listener && m_seekable)
            m_listener->seek(target);
#endif
        // 不管播放器支不支持 seek，本地时钟都从目标位置重建：
        // 这样即使播放器忽略请求，进度条也会先跟上手，随后被下一次上报纠正
        m_posBase = target;
        m_posClock.restart();
        m_pos = target;
        updateTimeLabels();
    });
    // 拖动过程中只更新左侧时间，不碰 m_pos：松手时才真正提交
    connect(m_progress, &QSlider::sliderMoved, this, [this](int v)
    {
        m_elapsed->setText(formatTime(v));
    });

    // 用户拖音量才写系统（fromUser = true 会触发持久化），程序回填走 syncVolume
    connect(m_volumeSlider, &QSlider::valueChanged, this,
            [this](int v) { applyVolume(v, true); });
    // 每次弹出前重新读一次真实音量：别的程序/键盘改过音量时菜单不会显示旧值
    connect(m_volumeMenu, &QMenu::aboutToShow, this, &MusicWidget::syncVolume);

    // 图标依赖档位（m_m），必须在 applyMetrics 之后画；这里先给初始档位（默认 Metrics）
    updateRepeatButton();
    updateVolumeButton();
    updateTransportIcons();
    applyMetrics();
}

// 档位的唯一应用入口：先存 m_m（后续绘制/换档比较都用它），再设尺寸，最后摆位置。
// 顺序不能反：relayout 里的可见性依赖 m_m，applyMetrics 里也有基于新尺寸算行高的逻辑。
// 注意本函数会被 resize 防抖、主题切换、setHeaderHidden 多次调用，必须保持幂等。
void MusicWidget::applyMetrics()
{
    const AppSettings &st = AppSettings::get();
    m_m = metrics();

    m_grid->setContentsMargins(m_m.marginX, m_m.marginY, m_m.marginX, m_m.marginY);
    m_grid->setHorizontalSpacing(m_m.gapX);
    m_grid->setVerticalSpacing(m_m.gapY);

    // 换封面直径会触发 CoverView 重新裁圆；档位不变时它内部会自行短路
    m_cover->setDiameter(m_m.cover);

    const QFont titleF = st.uiFont(m_m.titlePx, QFont::DemiBold);
    const QFont artistF = st.uiFont(m_m.artistPx);
    const QFont timeF = st.uiFont(m_m.timePx);

    // +1：字体行高取整后可能比 QFontMetrics 报的少 1px，留一点防止下行被裁
    m_title->setFont(titleF);
    m_title->setFixedHeight(QFontMetrics(titleF).height() + 1);
    m_artist->setFont(artistF);
    m_artist->setFixedHeight(QFontMetrics(artistF).height() + 1);

    m_lyrics->setTextPixelSize(m_m.lyricPx);
    m_lyrics->setColors(st.textColor(), st.subTextColor());
    // 矮版式（歌词顶替歌名区）只画当前句，否则多行上下文会把封面挤没
    m_lyrics->setCompact(m_m.lyricOverInfo);
    // 大档的歌名/歌手是左对齐的，歌词也要左对齐才和它们落在同一条竖线上（「左右对齐」）；
    // 小档没有歌名做参照，居中更耐看。
    m_lyrics->setTextAlignment(m_m.lyricOverInfo ? Qt::AlignHCenter : Qt::AlignLeft);
    // 大档开放「时间列 + 点击 seek + 滚轮翻阅」（网易云歌词页那种交互）；小档纯展示。
    // 同时打上 qdwInteractive 标记：隐藏标题栏时的拖动过滤器据此把鼠标让给歌词，
    // 否则点歌词会被当成拖窗口。
    m_lyrics->setSeekable(!m_m.lyricOverInfo);
    m_lyrics->setProperty("qdwInteractive", !m_m.lyricOverInfo);

    // 紧凑模式下歌词占固定的整数行：高度一旦可以让步，布局就会把富余全塞给歌词，
    // 把进度条和控制按钮推到两端（三件套被拉开）；固定住之后富余只会落到
    // relayout() 里上下两条等权重的留白行上，于是整块内容在内容区里垂直居中。
    // 行高直接问 LyricsView 要（它按字号算好了），保证正好是整数行、不露半行。
    // 大版式相反：歌词区要能吃满右列，所以恢复 Expanding。
    if (m_m.lyricOverInfo) {
        const int rowH = qMax(1, m_lyrics->rowHeight());
        m_lyrics->setFixedHeight(rowH * qMax(1, m_m.lyricRows));
        m_lyrics->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    } else {
        m_lyrics->setMinimumHeight(0);
        m_lyrics->setMaximumHeight(QWIDGETSIZE_MAX);
        m_lyrics->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }

    // 时间标签用等宽占位，避免数字变化时进度条左右跳
    const int timeW = QFontMetrics(timeF).horizontalAdvance(QStringLiteral("88:88")) + 2;
    m_elapsed->setFont(timeF);
    m_total->setFont(timeF);
    m_elapsed->setFixedWidth(timeW);
    m_total->setFixedWidth(timeW);

    auto sizeBtn = [](QToolButton *b, int box, int icon)
    {
        b->setFixedSize(box, box);
        b->setIconSize(QSize(icon, icon));
    };
    // 播放键图标比别的按钮大 4px：它在视觉上要更突出
    sizeBtn(m_repeatBtn, m_m.btn, m_m.iconPx);
    sizeBtn(m_prevBtn, m_m.btn, m_m.iconPx);
    sizeBtn(m_nextBtn, m_m.btn, m_m.iconPx);
    sizeBtn(m_volumeBtn, m_m.btn, m_m.iconPx);
    sizeBtn(m_playBtn, m_m.playBtn, m_m.iconPx + 4);

    // 控制行比按钮高 6px 留呼吸空间；进度行至少 18px 且随字体行高增长，
    // 否则大字号下时间文本会被压扁
    m_controls->setFixedHeight(m_m.playBtn + 6);
    m_progressRow->setFixedHeight(qMax(18, QFontMetrics(timeF).height() + 4));

    // 图标尺寸依赖 m_m，必须在 m_m 更新之后重画
    updateTransportIcons();
    updateVolumeButton();
    updateRepeatButton();

    updateTexts();
    relayout();
}

// 版面摆放。只有两种版式：
//   小档（compact）—— 封面在左单列，右列「歌名（够高才有）/ 歌词 / 进度 / 控制」整体垂直居中
//   大档（full）   —— 网易云式：左边大封面竖跨，右侧上歌名下歌词，底部进度 + 控制横贯整行
// 每次调用会先清空网格再重摆；takeAt 返回的 item 只包着 widget，删 item 不会删 widget，
// 所以同一批控件可以反复复用，不会泄漏也不会被销毁。
void MusicWidget::relayout()
{
    while (QLayoutItem *item = m_grid->takeAt(0))
        delete item;

    // 先清掉旧的拉伸系数，否则上一档设置的 stretch 会残留，导致新档位留白异常
    // 8 是本版式的行列数量上限（实际最多用到 6 行 2 列），多清几格比算准更省事也更安全
    for (int i = 0; i < 8; ++i) {
        m_grid->setRowStretch(i, 0);
        m_grid->setColumnStretch(i, 0);
    }
    // 右列吃满剩余宽度（歌词/信息都放右列）
    m_grid->setColumnStretch(1, 1);

    const bool under = m_m.underCover;
    const bool lyricInfo = m_m.lyricOverInfo;

    // 左列宽度：够放大封面（大档还要留出呼吸空间），又不至于把歌词挤没
    // 38% 是经验值：太窄歌名要滚很久，太宽歌词折行后只剩一两个字。
    // 这里的连续百分比只影响列宽（不触发封面重建/歌词重折行），所以不算「连续档位」。
    const int leftW = qBound(m_m.cover + 16, int(width() * 0.38), m_m.cover + 110);
    // 大档左列要放封面 + 呼吸空间；小档左列只有封面，固定成封面宽度，富余全给右列
    m_grid->setColumnMinimumWidth(0, under ? leftW : m_m.cover);

    // 行号在各个版式分支里自行定义，避免上一个版式的行语义被下一个误用
    if (lyricInfo) {
        // 小档：左列封面竖跨整块内容区（上下居中）；右列自上而下是
        // 「歌词 / 进度条 / 控制按钮」。上下各放一条等权重的留白行，
        // 三件套作为一块整体在内容区里垂直居中。
        // 这个版式**不显示歌名块** —— 高度就这么多，留给歌词更值；
        // 没有歌词时由 effectivePlaceholder 顶上歌名/歌手，信息不会丢。
        constexpr int kPadTopRow = 0;
        constexpr int kLyricRow = 1;
        constexpr int kProgressRow = 2;
        constexpr int kControlsRow = 3;
        constexpr int kPadBottomRow = 4;

        m_grid->addWidget(m_cover, kPadTopRow, 0, kPadBottomRow + 1, 1,
                          Qt::AlignVCenter | Qt::AlignLeft);
        m_grid->addWidget(m_lyrics, kLyricRow, 1);
        if (m_m.showProgress)
            m_grid->addWidget(m_progressRow, kProgressRow, 1);
        m_grid->addWidget(m_controls, kControlsRow, 1);

        m_grid->setRowStretch(kPadTopRow, 1);
        m_grid->setRowStretch(kPadBottomRow, 1);
    } else {
        // 大档（网易云那种排法）：左列大封面竖跨「歌名 + 歌词」两行并垂直居中；
        // 右列上边是歌名/歌手、下边是歌词；进度条与控制横贯整行贴底。
        constexpr int kInfoRow = 0;
        constexpr int kLyricRow = 1;
        constexpr int kProgressRow = 2;
        constexpr int kControlsRow = 3;

        m_grid->addWidget(m_cover, kInfoRow, 0, 2, 1, Qt::AlignVCenter | Qt::AlignLeft);
        m_grid->addWidget(m_info, kInfoRow, 1, 1, 1);
        m_grid->addWidget(m_lyrics, kLyricRow, 1, 1, 1);
        // 富余高度全给歌词：歌名块按自身 sizeHint 定高，进度与控制贴底
        m_grid->setRowStretch(kLyricRow, 1);

        if (m_m.showProgress)
            m_grid->addWidget(m_progressRow, kProgressRow, 0, 1, 2);
        m_grid->addWidget(m_controls, kControlsRow, 0, 1, 2);
    }

    // 可见性统一在这里收口：lyricInfo 下歌名区被歌词取代，三者都要藏起来，
    // 否则 QGridLayout 里隐藏的 widget 仍占最小尺寸，会白留一条缝
    m_cover->show();
    // 歌名块只在大档显示；小档把它让给歌词（见 relayout 的 kLyricRow 注释）
    const bool infoVisible = !lyricInfo;
    m_info->setVisible(infoVisible);
    m_title->setVisible(infoVisible);
    m_artist->setVisible(infoVisible && m_m.showArtist);
    m_lyrics->setVisible(under || lyricInfo);
    m_progressRow->setVisible(m_m.showProgress);
    m_controls->show();
    m_repeatBtn->setVisible(m_m.showRepeatVolume);
    m_volumeBtn->setVisible(m_m.showRepeatVolume);
    m_prevBtn->setVisible(m_m.showPrevNext);
    m_nextBtn->setVisible(m_m.showPrevNext);
}

// 歌手为空时不要拼出多余的「 · 」，专辑为空时也只显示歌手。
void MusicWidget::updateTexts()
{
    m_title->setText(m_titleText);
    m_artist->setText(m_albumText.isEmpty()
                          ? m_artistText
                          : QStringLiteral("%1 · %2").arg(m_artistText, m_albumText));
    updateLyricHint();
}

// 歌词区该显示的兜底文案。返回的是「当前这一档版式下最该看到的一句话」，
// 不是固定提示语，所以每次 updateLyricHint 都必须重新调用。
QString MusicWidget::effectivePlaceholder() const
{
    // 矮版式下歌名区已被歌词顶替：没有歌词时至少把歌名/歌手显示出来，不要留空
    // 注意判断用的是 hasLyrics()（已折好行）而不是 m_lyricLines，纯文本歌词也算「有歌词」
    if (m_m.lyricOverInfo && !m_titleText.isEmpty() && !m_lyrics->hasLyrics()) {
        return m_artistText.isEmpty() ? m_titleText
                                      : QStringLiteral("%1 · %2").arg(m_titleText, m_artistText);
    }
    return m_lyricHint;
}

// placeholder 的具体内容取决于版式（是否矮版式、当前有没有歌词），所以每次都要重算，
// 不能只在歌词变化时设置一次。
void MusicWidget::updateLyricHint()
{
    m_lyrics->setPlaceholder(effectivePlaceholder());
}

// 标题栏折叠会改变纵向可用空间的计算前提，必须立刻按新档位重排（不等 resize）。
void MusicWidget::setHeaderHidden(bool hidden)
{
    if (m_headerHidden == hidden)
        return;
    m_headerHidden = hidden;
    applyMetrics();
}

// 主题切换：把「设置那一刻就固定下来」的颜色全部重设一遍。
// 走过的坑：深色主题下音乐组件的文字还是黑的 —— 因为 QSS 和几个 setTextColor 都只在
// buildUi() 里跑过一次，之后没有任何东西会重新读 AppSettings。
void MusicWidget::applyTheme()
{
    const AppSettings &st = AppSettings::get();

    // 面板 QSS：QLabel#sub（两端时间）、按钮、滑条的颜色都从这里来
    setStyleSheet(UiStyle::panelCss());

    m_title->setTextColor(st.textColor());
    m_artist->setTextColor(st.subTextColor());
    m_lyrics->setColors(st.textColor(), st.subTextColor());

    // 图标是 tintIcon 按当时的文字色现染的，换主题必须重染（形状不变、颜色要跟着换）
    updateTransportIcons();
    updateVolumeButton();
    updateRepeatButton();

    update();
}

// 只比较「会影响布局」的字段，成本低、可以每帧调用。
// 注意这是档位指纹的子集：漏掉 artistPx/gapX/gapY/playBtn/iconPx/timePx，
// 因为它们总是和已比较的字段同步变化；如果以后新增了「只改这些字段」的档位，这里会漏检。
bool MusicWidget::metricsChanged() const
{
    // 字号/按钮/间距现在是按尺寸连续算出来的，任何一个跨过 1px 都要重排，
    // 所以这里必须逐项比较，不能再假设「这些字段总是同步变化」（老版那种假设会漏检）。
    const Metrics n = metrics();
    return n.cover != m_m.cover || n.underCover != m_m.underCover
           || n.lyricOverInfo != m_m.lyricOverInfo
           || n.lyricRows != m_m.lyricRows
           || n.titlePx != m_m.titlePx || n.artistPx != m_m.artistPx
           || n.lyricPx != m_m.lyricPx || n.timePx != m_m.timePx
           || n.btn != m_m.btn || n.playBtn != m_m.playBtn || n.iconPx != m_m.iconPx
           || n.marginX != m_m.marginX || n.marginY != m_m.marginY
           || n.gapX != m_m.gapX || n.gapY != m_m.gapY
           || n.showArtist != m_m.showArtist
           || n.showProgress != m_m.showProgress || n.showPrevNext != m_m.showPrevNext
           || n.showRepeatVolume != m_m.showRepeatVolume || n.showTimes != m_m.showTimes;
}

void MusicWidget::resizeEvent(QResizeEvent *e)
{
    WidgetPanel::resizeEvent(e);

    // 拖边缘时 resize 事件非常密集，跨档位时每次都重排（重建封面、重新折行歌词）会明显卡顿。
    // 这里合并到事件循环空闲时执行一次，视觉上就是平顺的。
    // 只在档位真的变化时才排队：同档位内的纯尺寸变化不需要重排，交给布局自己处理。
    if (metricsChanged())
        m_applyTimer->start();
}

// 只关心音量按钮上的滚轮：上下各 5%，一次拖动就能微调。
// 返回 true 表示吞掉事件，避免 QToolButton/QMenu 再把它当成别的操作（如滚动父容器）。
bool MusicWidget::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_volumeBtn && e->type() == QEvent::Wheel) {
        auto *we = static_cast<QWheelEvent *>(e);
        // 音量未知（-1）时按 60 起步：直接加减会得到 55/65，符合「先给个中间值」的直觉
        const int base = m_volume < 0 ? 60 : m_volume;
        applyVolume(base + (we->angleDelta().y() > 0 ? 5 : -5), true);
        return true;
    }
    return WidgetPanel::eventFilter(obj, e);
}

// ---------------------------------------------------------------- 循环 / 音量

// 三态轮转。清 m_repeatHandled：换了模式就该按新模式重新判断本轮是否已处理，
// 否则从「单曲」切到「列表」时可能因为标志还是 true 而漏掉一次自动下一首。
void MusicWidget::cycleRepeat()
{
    m_repeat = (m_repeat + 1) % 3;
    m_repeatHandled = false;
    updateRepeatButton();
    emit panelChanged(); // 让外层把模式写进布局文件
}

// 图标和提示文案一一对应，改图标时别忘了同步 tooltip，否则用户看到的解释是错的。
void MusicWidget::updateRepeatButton()
{
    if (!m_repeatBtn)
        return;
    m_repeatBtn->setIcon(repeatIcon(m_repeat));
    switch (m_repeat) {
    case RepeatAll:
        m_repeatBtn->setToolTip(QStringLiteral("列表循环：播完自动下一首"));
        break;
    case RepeatOne:
        m_repeatBtn->setToolTip(QStringLiteral("单曲循环：快结束时自动回到开头"));
        break;
    default:
        m_repeatBtn->setToolTip(QStringLiteral("顺序播放（跟随播放器）"));
        break;
    }
}

// 音量读取：优先读播放器自己的音频会话，读不到（<0）才退化成系统音量。
// 拖动滑条期间直接返回：此时真实值正被用户改写，回读会把滑块拽回旧位置。
void MusicWidget::syncVolume()
{
    if (m_volumeSlider && m_volumeSlider->isSliderDown())
        return;

    // 目标会话：用户锁定了播放器就用它，否则用当前实际在放歌的那个
    const QString target = m_appId.isEmpty() ? m_currentAppId : m_appId;
    int v = MediaVolume::volumeFor(target);
    m_volumeIsSystem = false;
    if (v < 0) {
        v = MediaVolume::systemVolume();
        m_volumeIsSystem = true;
    }
    if (v < 0)
        return; // 两路都读不到：保持「—」不变，也不要误报成 0

    m_volume = v;
    // 先比一次再 setValue：避免触发 valueChanged 又把音量写回系统（回环）
    if (m_volumeSlider && m_volumeSlider->value() != v)
        m_volumeSlider->setValue(v);
    if (m_volumeValue)
        m_volumeValue->setText(QStringLiteral("%1%").arg(v));
    updateVolumeButton();
}

// 写音量：目标会话写失败才退化成系统音量，并把 m_volumeIsSystem 记下来供 tooltip 提示。
// 这个标志是「上次写入是否降级」而不是「目标播放器是否支持音量」，所以每次写都要重置。
void MusicWidget::applyVolume(int value, bool fromUser)
{
    const int v = qBound(0, value, 100);
    m_volume = v;

    const QString target = m_appId.isEmpty() ? m_currentAppId : m_appId;
    const bool ok = MediaVolume::setVolume(target, v);
    m_volumeIsSystem = !ok;
    if (!ok)
        MediaVolume::setSystemVolume(v);

    if (m_volumeSlider && m_volumeSlider->value() != v)
        m_volumeSlider->setValue(v); // 越界值回夹后要同步回滑条
    if (m_volumeValue)
        m_volumeValue->setText(QStringLiteral("%1%").arg(v));
    updateVolumeButton();

    // 只有用户动作才触发持久化；程序内部回填不写盘
    if (fromUser)
        emit panelChanged();
}

// 三个传输图标都用 QStyle 标准图标再着色：自己画虽然更统一，但不同平台的形状差异太大。
void MusicWidget::updateTransportIcons()
{
    const AppSettings &st = AppSettings::get();
    const int px = m_m.iconPx + 4; // 传输键图标比其它按钮大一档，观感更接近播放器

    m_playBtn->setIcon(tintIcon(style()->standardIcon(
                                   m_playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay),
                               st.textColor(), px));
    m_prevBtn->setIcon(tintIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward),
                                st.textColor(), px));
    m_nextBtn->setIcon(tintIcon(style()->standardIcon(QStyle::SP_MediaSkipForward),
                                st.textColor(), px));
}

// 未知音量（-1）按 60 处理：只是为了选图标，别显示成静音让用户以为没声音。
void MusicWidget::updateVolumeButton()
{
    if (!m_volumeBtn)
        return;
    const int v = m_volume < 0 ? 60 : m_volume;
    m_volumeBtn->setIcon(tintIcon(style()->standardIcon(
                                      v == 0 ? QStyle::SP_MediaVolumeMuted
                                             : QStyle::SP_MediaVolume),
                                  AppSettings::get().textColor(), m_m.iconPx + 4));
    // 降级到系统音量时明确告知，否则用户会以为「拖了没反应」
    m_volumeBtn->setToolTip(m_volumeIsSystem
                                ? QStringLiteral("音量（未找到该播放器的音频会话，调的是系统音量）")
                                : QStringLiteral("音量"));
}

// ---------------------------------------------------------------- 系统媒体会话

// 初始化 GSMTC。三类信号分别对应：会话内容变化、broker 不可用、会话列表刷新。
// 构造函数里调用，此时 m_appId 已从全局设置读出，所以 selectSession 要放在 start 之后。
void MusicWidget::initSystem()
{
#ifdef QDW_HAS_GSMTC
    // 把监听器挂成本面板的子对象：随面板一起销毁（Qt 所有权），
    // 面板析构时会先断开所有连接、再删子对象，所以监听器析构期间发的信号不会打到半析构的 this。
    m_listener = new GlobalMediaSessionListener(this);

    connect(m_listener, &GlobalMediaSessionListener::currentMediaChanged, this,
            [this](const QString &appId, const QString &title, const QString &artist,
                   const QString &album, bool playing, qint64 position, qint64 duration,
                   bool seekable, const QByteArray &cover)
    {
        onMediaChanged(appId, title, artist, album, playing, position, duration, seekable, cover);
    });

    // 系统 broker 卡死时 RequestAsync 会一直不返回，必须给出能看懂的提示
    connect(m_listener, &GlobalMediaSessionListener::unavailable, this,
            [this](const QString &reason)
    {
        m_titleText = QStringLiteral("未检测到媒体会话");
        m_artistText = reason;
        updateTexts();
        emit panelChanged();
        update();
    });

    connect(m_listener, &GlobalMediaSessionListener::availableSessions, this,
            [this](const QList<GlobalMediaSessionListener::SessionEntry> &sessions)
    {
        // 两个列表按下标一一对应，右键菜单里用同一下标取 id 和名字
        m_sessionIds.clear();
        m_sessionNames.clear();
        for (const auto &s : sessions) {
            m_sessionIds << s.appId;
            const QString nice = friendlyApp(s.appId);
            // 内置映射优先（中文名更友好），否则用播放器自报的名字
            m_sessionNames << (nice.isEmpty() ? s.appName : nice);
        }
        // 锁定的播放器重新出现时刷新角标名字
        if (!m_appId.isEmpty()) {
            const int idx = m_sessionIds.indexOf(m_appId);
            if (idx >= 0)
                m_appName = m_sessionNames.at(idx);
        }
        syncVolume();
        emit panelChanged(); // 菜单里的播放器列表变了，外层需要重建右键菜单
    });

    m_systemReady = m_listener->start();
    if (!m_systemReady) {
        // 启动失败也要给出原因，否则界面看起来只是「没有歌在放」
        m_artistText = m_listener->lastError().isEmpty()
                           ? QStringLiteral("无法连接系统媒体会话")
                           : m_listener->lastError();
        m_lyricHint = m_artistText;
        updateTexts();
        updateLyricHint();
    }

    // 即使 start 失败也照常调用：不额外分支，交给监听器自行处理未初始化的情况
    m_listener->selectSession(m_appId);
    m_listener->refreshAvailableSessions();
#else
    m_systemReady = false;
    m_titleText = QStringLiteral("系统媒体会话不可用");
    m_artistText = QStringLiteral("当前构建未启用 GSMTC（需要 MSVC）");
    m_lyricHint = m_artistText;
    updateTexts();
    updateLyricHint();
    // 没有监听器也要摆一次版式，保证这个组件在 MinGW 构建下不是空白方块
    relayout();
#endif
}

// 会话状态回调。处理顺序有讲究：先判断是否换播放器（决定音量来源），
// 再用「歌名+歌手」判断是否换歌（决定拉歌词/清封面），最后同步进度与循环策略。
void MusicWidget::onMediaChanged(const QString &appId, const QString &title, const QString &artist,
                                 const QString &album, bool playing, qint64 position,
                                 qint64 duration, bool seekable, const QByteArray &cover)
{
    if (!appId.isEmpty() && appId != m_currentAppId) {
        const bool firstTime = m_currentAppId.isEmpty();
        m_currentAppId = appId;
        // 名字取值顺序：本次扫描到的展示名 → 内置映射 → 沿用上次的名字（都拿不到时至少别清空）
        const int idx = m_sessionIds.indexOf(appId);
        QString name = idx >= 0 ? m_sessionNames.at(idx) : QString();
        if (name.isEmpty())
            name = friendlyApp(appId);
        if (name.isEmpty())
            name = m_appName;
        if (name != m_appName) {
            m_appName = name;
            emit panelChanged(); // 角标文字变了
        }
        // 换了播放器，音量要重新读它自己的。只在「首次拿到会话」或「跟随系统」时读，
        // 否则用户锁定某个播放器时会被别的会话抢走音量显示。
        if (firstTime || m_appId.isEmpty())
            syncVolume();
    }

    // 歌名与歌手同时为空 = 播放器停了/没有元数据，此时保留组件但显示引导语
    if (title.isEmpty() && artist.isEmpty()) {
        setNowPlaying(QStringLiteral("未在播放"), QStringLiteral("打开任意音乐软件即可自动显示"),
                      QString());
        setProgress(0, 0);
        return;
    }

    // 用 title|artist 做换歌判断（专辑单独变化不算换歌）
    const bool trackChanged = (title + QLatin1Char('|') + artist) != m_trackKey;
    setNowPlaying(title, artist, album);

    if (!cover.isEmpty()) {
        // 逐字节比较：轮询每 500ms 都会把同一张封面再送过来，
        // 不做去重就是每 500ms 解码一次图片 + 重裁一次圆形封面（rebuild 里的平滑缩放最贵），
        // 既白烧 CPU，又会在 GUI 线程上周期性卡一下，连心跳都被拖慢。
        if (cover != m_coverData) {
            QPixmap pm;
            // 解码失败就当没封面，不要用半张图去污染 CoverView 的缓存
            if (pm.loadFromData(cover)) {
                m_coverData = cover;
                setCover(pm);
            }
        }
    } else if (trackChanged) {
        // 新歌没带封面 → 清掉旧图显示占位；同一首歌的后续回调没封面时不动，避免闪一下
        m_coverData.clear();
        setCover(QPixmap());
    }

    m_seekable = seekable;
    // 以这次上报为基准重建本地时钟：进度推算全部依赖这三个字段的同步更新
    m_posBase = position;
    m_posClock.restart();
    m_pos = position;
    m_dur = duration;

    setPlaying(playing);
    setProgress(position, duration);
    m_lyrics->setPosition(m_pos);
    applyRepeatPolicy();
}

// 切换优先显示的播放器。这里会写全局设置（跨组件共享），所以任何入口（右键菜单）都走它。
void MusicWidget::chooseSource(const QString &appId)
{
    m_appId = appId;

    // 先落盘再改界面：即使后面刷新失败，下次启动也记得用户的选择
    AppSettings::get().musicAppId = appId;
    AppSettings::get().save();

    // 展示名的三级回退：会话列表 → 内置映射 → 原始 id
    const int idx = m_sessionIds.indexOf(appId);
    m_appName = idx >= 0 ? m_sessionNames.at(idx) : QString();
    if (m_appName.isEmpty())
        m_appName = appId.isEmpty() ? QStringLiteral("跟随系统") : friendlyApp(appId);
    if (m_appName.isEmpty())
        m_appName = appId;

#ifdef QDW_HAS_GSMTC
    if (m_listener)
        m_listener->selectSession(m_appId);
#endif

    syncVolume(); // 不同播放器音量独立，切完要重读
    emit panelChanged();
}

// ---------------------------------------------------------------- 歌词

// 每次换歌都先彻底清空，再按当前歌名/歌手发起请求。
// 请求是异步的，结果回来时用歌名比对（见 onLyricReady）来丢弃过期响应。
void MusicWidget::requestLyrics()
{
    m_lyricLines.clear();
    m_lyricPlain.clear();
    m_lyrics->clear();

    if (m_titleText.isEmpty() || m_titleText == QStringLiteral("未在播放")) {
        m_lyricHint = QStringLiteral("打开任意音乐软件即可自动显示");
        updateLyricHint();
        return;
    }

    m_lyricHint = QStringLiteral("歌词加载中…");
    updateLyricHint();
    m_lyricFetcher->fetch(m_titleText, m_artistText);
}

// 抓取器的异步回调。text 为空/全空白、或解析后一个字都没有，都归到「暂无歌词」。
void MusicWidget::onLyricReady(const QString &title, const QString &artist, const QString &text)
{
    Q_UNUSED(artist)
    // 只比歌名、不比歌手：歌手字段在请求与回调之间可能被接口补写（feat. 顺序、多歌手分隔符），
    // 拿它做判据容易把有效结果误丢；歌名足以识别「迟到的上一首结果」。
    // 代价是同名不同歌会串，但这种并发在实际使用中几乎不出现。
    if (title != m_titleText)
        return; // 迟到的上一首结果

    if (text.trimmed().isEmpty()) {
        m_lyricHint = QStringLiteral("暂无歌词");
        m_lyrics->clear();
        updateLyricHint();
        return;
    }

    m_lyricLines = parseLrc(text);
    if (m_lyricLines.isEmpty()) {
        // 一条时间标签都没解析出来时退回纯文本。此时必须把 [..] 方括号标签清掉，
        // 否则会把 [00:12.34] 这种时间戳原样印在歌词区里。
        static const QRegularExpression anyTag(QStringLiteral("\\[[^\\]]*\\]"));
        QStringList kept;
        const QStringList rawLines = text.split(QLatin1Char('\n'));
        for (const QString &raw : rawLines) {
            QString s = raw;
            s.remove(anyTag); // 去掉所有标签，包括 [ti:] [ar:] 与残留时间戳
            s = s.trimmed();
            if (!s.isEmpty())
                kept << s;
        }
        m_lyricPlain = kept.join(QLatin1Char('\n'));
        // 全是标签没有正文（例如只有 [ti:xxx]）时，等同于「没有歌词」
        if (m_lyricPlain.isEmpty()) {
            m_lyricHint = QStringLiteral("暂无歌词");
            m_lyrics->clear();
            updateLyricHint();
            return;
        }
        m_lyrics->setPlainText(m_lyricPlain);
    } else {
        m_lyrics->setLyrics(m_lyricLines);
        // 立刻按当前进度定位：歌词往往比播放状态晚到，不补这一次会停在第 0 句
        m_lyrics->setPosition(m_pos);
    }
}

// ---------------------------------------------------------------- 显示

// 只有「歌名|歌手」变化才算换歌，专辑单独变化（同一首歌不同合辑）不重新拉歌词。
void MusicWidget::setNowPlaying(const QString &title, const QString &artist, const QString &album)
{
    const QString key = title + QLatin1Char('|') + artist;
    const bool changed = (key != m_trackKey);

    m_titleText = title;
    m_artistText = artist;
    m_albumText = album;
    m_trackKey = key;

    updateTexts();

    if (changed) {
        // 换歌先清歌词再请求，避免新歌开头几百毫秒还显示上一首的句子
        m_lyricLines.clear();
        m_lyricPlain.clear();
        m_lyrics->clear();
        requestLyrics();
    }
}

// m_coverPix 目前只是留档（内部没有读取点），真正按直径裁圆的是 CoverView 自己缓存的源图。
void MusicWidget::setCover(const QPixmap &pixmap)
{
    m_coverPix = pixmap;
    m_cover->setCover(pixmap);
}

// 封面旋转状态必须无条件同步（放在去重之前），否则暂停后再播放时，
// 如果 m_playing 恰好没变，封面就会一直停着不转。
void MusicWidget::setPlaying(bool playing)
{
    m_cover->setSpinning(playing);

    if (m_playing == playing)
        return;
    m_playing = playing;
    // 重新开始播放且位置还不在末尾两秒内 → 视为新一轮播放，允许再次触发循环处理
    if (playing && m_pos < qMax<qint64>(0, m_dur - 2000))
        m_repeatHandled = false;
    updateTransportIcons();
    emit panelChanged();
}

// 播放器上报的进度。拖动中不覆盖滑条，但仍更新 m_pos / m_dur，
// 这样松手后本地补时的基准不会落后于真实时长。
void MusicWidget::setProgress(qint64 position, qint64 duration)
{
    m_pos = position;
    m_dur = duration;
    if (!m_seeking) {
        // duration 可能为 0（未知/直播）：范围给 (0,0)，滑条自然变成不可拖
        m_progress->setRange(0, int(qMax<qint64>(0, duration)));
        // 双重夹取：position 可能超过 duration（播放器上报延迟）或为负
        m_progress->setValue(int(qBound<qint64>(0, position, qMax<qint64>(0, duration))));
    }
    m_progress->setEnabled(duration > 0);
    updateTimeLabels();
}

// 两端时间都从 m_pos / m_dur 现算，保证左右不会一个用新值一个用旧值。
void MusicWidget::updateTimeLabels()
{
    // 心跳已经压到 10Hz，但显示的只是 mm:ss：秒数没变就不重设文本。
    // 否则每秒要做 10 次 setText，两次之间还夹着进度行的重新排版，纯粹是抖动。
    const qint64 sec = m_pos / 1000;
    if (sec == m_shownSec && m_dur == m_shownDur)
        return;
    m_shownSec = sec;
    m_shownDur = m_dur;
    m_elapsed->setText(formatTime(m_pos));
    m_total->setText(formatTime(m_dur));
}

// 心跳。播放中才推进本地时钟；暂停时 m_pos 保持不动（此时 m_posBase + elapsed 已经没意义）。
void MusicWidget::onTick()
{
    // 两次上报之间用本地时钟补时间，进度条不会一跳一跳
    if (m_playing && m_dur > 0 && !m_seeking) {
        // 夹到 [0, m_dur]：时钟漂移或上报延迟都不该让进度越界
        m_pos = qBound<qint64>(0, m_posBase + m_posClock.elapsed(), m_dur);
        // 每帧都重设 range：某些播放器（浏览器）的时长是渐进上报的，会从 0 变到真实值
        m_progress->setRange(0, int(m_dur));
        m_progress->setValue(int(m_pos));
        updateTimeLabels();
    }
    // 歌词定位不受暂停影响：暂停在句中时仍应高亮那句
    m_lyrics->setPosition(m_pos);
    applyRepeatPolicy();
}

// 循环只是「在曲末替播放器发一个动作」，不接管播放。用 m_repeatHandled 保证一轮只做一次，
// 否则 300ms 心跳会在末尾反复 seek/next。只有位置重新回到曲目前段才允许下次触发。
void MusicWidget::applyRepeatPolicy()
{
    if (m_dur <= 0)
        return; // 时长未知（直播/还没拿到元数据）时无从判断「快结束」

    if (m_repeat == RepeatOne) {
        // 快结束时把位置拉回开头：只依赖 seek，任何播放器都能生效
        // 阈值 600ms 是折中：太早会切掉尾音，太晚可能已经播完进入停止状态
        if (m_playing && !m_repeatHandled && m_pos >= m_dur - 600) {
            m_repeatHandled = true;
#ifdef QDW_HAS_GSMTC
            if (m_listener && m_seekable)
                m_listener->seek(0);
#endif
            // 本地状态立刻归零：不等播放器回报，避免这 300ms 内进度条卡在末尾
            m_pos = 0;
            m_posBase = 0;
            m_posClock.restart();
            m_progress->setValue(0);
            updateTimeLabels();
        } else if (m_pos < m_dur / 2) {
            // 回到前半段说明确实重播了，解锁下一轮
            m_repeatHandled = false;
        }
        return;
    }

    if (m_repeat == RepeatAll) {
        // 播完停在末尾（播放器自己没循环）时，替它拉下一首
        // 用 !m_playing 判断「已经停了」，1200ms 提前量容纳上次心跳的时间差
        if (!m_playing && !m_repeatHandled && m_pos >= m_dur - 1200) {
            m_repeatHandled = true;
#ifdef QDW_HAS_GSMTC
            if (m_listener) {
                m_listener->next();
                // 800ms 后再补一次 play：部分播放器 next 之后处于暂停态，
                // 不补这一下就会「切了歌但不播」
                QTimer::singleShot(800, this, [this]
                {
                    if (m_listener && !m_playing)
                        m_listener->play();
                });
            }
#endif
        } else if (m_playing && m_pos < m_dur - 2000) {
            // 新一轮播放已经开始（位置离开末尾区域），允许下次曲末再触发
            m_repeatHandled = false;
        }
    }
}

// ---------------------------------------------------------------- 框架接口

// 固定标题，不跟随播放器变化（面板与角标已经区分了来源）。
QString MusicWidget::panelTitle() const
{
    return QStringLiteral("音乐");
}

// 角标：优先显示当前播放器名；没有名字时用「跟随系统 / 不可用」表达连接状态
QString MusicWidget::badgeText() const
{
    if (!m_appName.isEmpty())
        return m_appName;
    return m_systemReady ? QStringLiteral("跟随系统") : QStringLiteral("不可用");
}

// 外层定时/手动刷新时调用：重新枚举会话并回到用户锁定的那个，再同步音量。
void MusicWidget::refresh()
{
#ifdef QDW_HAS_GSMTC
    if (m_listener) {
        m_listener->refreshAvailableSessions();
        m_listener->selectSession(m_appId);
    }
#endif
    syncVolume();
}

// 只持久化用户选择，不存「当前在放什么歌」：重启后应显示实时状态而不是旧歌名。
void MusicWidget::saveState(QJsonObject &o) const
{
    o[QStringLiteral("appId")] = m_appId;
    o[QStringLiteral("repeat")] = m_repeat;
    // 音量未知时不写，避免把 -1 存进配置后下次被当成合法值
    if (m_volume >= 0)
        o[QStringLiteral("volume")] = m_volume;
}

// 反序列化时对所有枚举/数值做夹取：手改过的配置文件不能把内部状态搞坏。
void MusicWidget::restoreState(const QJsonObject &o)
{
    // repeat 只允许 0..2，缺省保留当前值
    m_repeat = qBound(0, o.value(QStringLiteral("repeat")).toInt(m_repeat), 2);
    updateRepeatButton();

    if (o.contains(QStringLiteral("volume"))) {
        m_volume = qBound(0, o.value(QStringLiteral("volume")).toInt(60), 100);
        // 直接刷界面，不回写系统音量：恢复布局不应该突然改动正在播放的音量
        if (m_volumeSlider)
            m_volumeSlider->setValue(m_volume);
        if (m_volumeValue)
            m_volumeValue->setText(QStringLiteral("%1%").arg(m_volume));
        updateVolumeButton();
    }

    // 空 appId 表示「跟随系统」，此时不需要走 chooseSource（那会重新落盘）
    const QString appId = o.value(QStringLiteral("appId")).toString();
    if (!appId.isEmpty())
        chooseSource(appId);
}

// 右键菜单：播放器来源 / 循环模式 / 重扫。菜单项都是 checkable，
// 勾选状态直接反映当前内部状态；真正切换仍走同一批成员函数，保证副作用一致。
void MusicWidget::buildMenu(QMenu *menu)
{
    QMenu *sourceMenu = menu->addMenu(QStringLiteral("优先显示哪个播放器"));
    styleMenu(sourceMenu);

    // 「跟随系统」用空 appId 表示，与 m_appId 的哨兵约定一致
    QAction *follow = sourceMenu->addAction(QStringLiteral("跟随系统当前会话"));
    follow->setCheckable(true);
    follow->setChecked(m_appId.isEmpty());
    connect(follow, &QAction::triggered, this, [this] { chooseSource(QString()); });

    for (int i = 0; i < m_sessionIds.size(); ++i) {
        const QString id = m_sessionIds.at(i);
        // 名字缺失时退回 id，至少能看出是哪个程序
        QAction *a = sourceMenu->addAction(m_sessionNames.value(i, id));
        a->setCheckable(true);
        a->setChecked(m_appId == id);
        // 按值捕获 id：循环变量 i 出了作用域就失效
        connect(a, &QAction::triggered, this, [this, id] { chooseSource(id); });
    }
    // 没有播放器时给一条禁用项，比空菜单更容易理解
    if (m_sessionIds.isEmpty())
        sourceMenu->addAction(QStringLiteral("（还没有检测到播放器）"))->setEnabled(false);

    sourceMenu->addSeparator();
#ifdef QDW_HAS_GSMTC
    // 兜底入口：broker 假死/换播放器后界面不更新时，用户可手动重连
    sourceMenu->addAction(QStringLiteral("重新连接系统媒体会话"), this, [this]
    {
        m_listener->start();
        m_listener->refreshAvailableSessions();
        emit panelChanged();
    });
#else
    sourceMenu->addAction(QStringLiteral("系统媒体会话不可用（需 MSVC 构建）"))->setEnabled(false);
#endif

    menu->addSeparator();
    QMenu *repeatMenu = menu->addMenu(QStringLiteral("循环播放"));
    styleMenu(repeatMenu);
    // 顺序必须与 RepeatMode 的枚举值一致（下标即模式值）
    const QStringList names = { QStringLiteral("顺序播放"), QStringLiteral("列表循环"),
                                QStringLiteral("单曲循环") };
    for (int i = 0; i < names.size(); ++i) {
        QAction *a = repeatMenu->addAction(names.at(i));
        a->setCheckable(true);
        a->setChecked(m_repeat == i);
        connect(a, &QAction::triggered, this, [this, i]
        {
            m_repeat = i;
            // 手动切模式同样要清标志，否则当前这曲末尾可能不按新模式处理
            m_repeatHandled = false;
            updateRepeatButton();
            emit panelChanged();
        });
    }

    // 只重扫会话列表，不重连 broker：适合「新开了播放器但菜单里还没有」的情况
    menu->addAction(QStringLiteral("重新扫描播放器"), this, [this]
    {
#ifdef QDW_HAS_GSMTC
        if (m_listener)
            m_listener->refreshAvailableSessions();
#endif
    });
}
