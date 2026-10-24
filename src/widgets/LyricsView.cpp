#include "widgets/LyricsView.h"

#include "core/AppSettings.h"

#include <QFontMetrics>
#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

// LyricsView 实现。三个关键点：
// 1) 折行单位是「视觉行」，滚动量 m_scrollRow 也是行号（浮点），所以行高必须是常量 m_rowH，
//    不能按字号逐行算，否则滚动会错位。
// 2) 所有「重建行结构」的入口（改字号 / 改宽度 / 换歌词）后面都必须跟 updateTarget(false)，
//    否则要么滚动停在旧行号，要么整屏动画滑一次。
// 3) 当前句永远垂直居中（多行时按句首行对齐中线），上下留给上下文做预告。
// 4) 每行的水平对齐可配（setTextAlignment）：小档居中好看，大档左对齐，
//    这样歌词首行的左边缘能和上方的歌名/歌手落在同一条竖线上。

namespace
{

/**
 * 按宽度把一句歌词折成多行；CJK 允许任意位置断行。
 * @param text  单句原文（不含换行符），空串也会产出一个空行以保持行数语义
 * @param font  折行用的字体，必须与绘制时一致，否则行数会算错
 * @param width 可用宽度（px）；内部夹到 >= 24，极窄时宁可溢出也不逐字折
 * @return 按顺序排好的行文本；每行已 trimmed，所以行首尾的空白不会占宽度
 */
QStringList wrapText(const QString &text, const QFont &font, int width)
{
    QStringList out;
    if (text.isEmpty()) {
        out << QString();
        return out;
    }

    // 下限 24px：控件被压到极窄时 QTextLayout 会退化成每字一行，留点宽度减少抖动
    const int w = qMax(24, width);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);

    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(w);
        // 只取纯文本片段：这里不做富文本渲染，保留原文即可
        out << text.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();

    // 极端情况（宽度为 0 等）可能一行都没产出，兜底成整句，宁可溢出也不丢内容
    if (out.isEmpty())
        out << text;
    return out;
}

} // namespace

// 构造只把控件本身准备好；没有歌词时 m_rows 为空，paintEvent 会去画 placeholder。
LyricsView::LyricsView(QWidget *parent)
    : QWidget(parent)
{
    // 默认不吃鼠标：小档/纯展示时点击要穿透给窗口，用来拖动盒子。
    // 只有 setSeekable(true) 才会把这条属性去掉（见 setSeekable）。
    setAttribute(Qt::WA_TransparentForMouseEvents);
    // 宽度跟随父布局（Ignored 表示 sizeHint 不参与宽度竞争），高度尽量占满
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    setMinimumWidth(40);

    // 动画必须先建好：setTextPixelSize 里会调 updateTarget 用到它
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(220);
    // OutCubic：起步快、收尾慢，比匀速更像「自然滑到位」
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v)
    {
        m_scrollRow = v.toReal();
        update();
    });

    // 滚轮翻阅后停手 2.5s 自动回到「跟随当前句」，否则视图会永远停在用户翻到的地方
    m_resumeTimer = new QTimer(this);
    m_resumeTimer->setSingleShot(true);
    m_resumeTimer->setInterval(2500);
    connect(m_resumeTimer, &QTimer::timeout, this, [this]
    {
        m_userScrolling = false;
        updateTarget(true);
    });

    // 时间列是「按需出现」的：最后一滚之后再撑 3s 就收起，平时不占位置
    m_timeHideTimer = new QTimer(this);
    m_timeHideTimer->setSingleShot(true);
    m_timeHideTimer->setInterval(3000);
    connect(m_timeHideTimer, &QTimer::timeout, this, [this] { setTimeColumnVisible(false); });

    const AppSettings &st = AppSettings::get();
    setTextPixelSize(13);
    setColors(st.textColor(), st.subTextColor());
}

// 只改交互开关：不动内容，但时间列可见与否会改变折行宽度，要重新折行。
void LyricsView::setSeekable(bool on)
{
    if (m_seekable == on)
        return;
    m_seekable = on;

    // 可交互时必须接收鼠标事件；否则恢复透明，让点击去拖窗口
    setAttribute(Qt::WA_TransparentForMouseEvents, !on);
    setMouseTracking(on);
    if (!on) {
        m_hoverRow = -1;
        m_userScrolling = false;
        m_timeVisible = false;   // 收起时间列，小档不给它留位置
        if (m_resumeTimer)
            m_resumeTimer->stop();
        if (m_timeHideTimer)
            m_timeHideTimer->stop();
        unsetCursor();
    }

    // 时间列占用宽度，正文折行宽度跟着变，所以要 rebuild
    rebuild();
    updateTarget(false);
    update();
}

// 时间列的显隐：会改变折行宽度所以必须 rebuild，但**不调 updateTarget** ——
// 用户正在翻阅，重算滚动目标会把他滚到的位置拽回「当前句居中」。
void LyricsView::setTimeColumnVisible(bool on)
{
    if (m_timeVisible == on)
        return;
    m_timeVisible = on;
    rebuild();
    update();
}

// 字号决定了折行结果与行高，没变就没必要重建 —— 拖窗口时 applyMetrics 会高频调用本函数。
void LyricsView::setTextPixelSize(int px)
{
    if (px == m_textPx)
        return; // 字号没变就别把整首歌词重新折行（拖窗口时会很卡）
    m_textPx = px;

    const AppSettings &st = AppSettings::get();
    m_font = st.uiFont(qMax(9, px));
    m_fontBold = st.uiFont(qMax(9, px), QFont::DemiBold);
    // 上下文（唱过/没唱到）比当前句小一号，突出正在唱的那句
    m_fontSmall = st.uiFont(qMax(8, px - 2));
    // 时间列比正文再小两号：它是辅助信息，不能和歌词抢视觉重心
    m_fontTime = st.uiFont(qMax(8, px - 4));
    // 行高统一按粗体算 +3 行距：用当前句字高当基准，避免高亮行贴住相邻行
    m_rowH = QFontMetrics(m_fontBold).height() + 3;
    rebuild();
    updateTarget(false);
    update();
}

// 纯颜色变化，不影响布局，不需要 rebuild。
void LyricsView::setColors(const QColor &current, const QColor &other)
{
    m_curColor = current;
    m_otherColor = other;
    update();
}

// 提示语由外部（MusicWidget）驱动，只在真正变化时重绘。
void LyricsView::setPlaceholder(const QString &text)
{
    if (m_placeholder == text)
        return;
    m_placeholder = text;
    update();
}

// 清空要连 m_plain / m_plainText 一起复位，并停掉动画，否则残留的滚动值会让
// 下一次 setLyrics 从半截位置开始。
void LyricsView::clear()
{
    m_texts.clear();
    m_times.clear();
    m_plain = false;
    m_plainText.clear();
    m_currentLine = -1;
    m_anim->stop();
    m_scrollRow = 0;
    m_targetRow = 0;
    rebuild();
    update();
}

// 拆成两条并行数组（时间 / 文本）是绘制热路径的需要：按行取文本、按时间查当前句，
// 用不着每帧解包 QPair。
void LyricsView::setLyrics(const QVector<QPair<qint64, QString>> &lines)
{
    m_plain = false;
    m_plainText.clear();
    m_times.clear();
    m_texts.clear();
    m_times.reserve(lines.size());
    m_texts.reserve(lines.size());
    for (const auto &l : lines) {
        m_times << l.first;
        m_texts << l.second;
    }
    m_currentLine = -1; // 交给下一次 setPosition 重新定位
    rebuild();
    updateTarget(false);
    update();
}

// 纯文本分支：没有时间轴，因此不高亮、不滚动，只做折行后整屏静态显示。
void LyricsView::setPlainText(const QString &text)
{
    m_plain = true;
    m_plainText = text;
    m_times.clear();
    m_texts.clear();
    m_currentLine = -1;
    rebuild();
    updateTarget(false);
    update();
}

// rebuild 只重建行结构，不动滚动位置；调用方负责随后 updateTarget。
void LyricsView::rebuild()
{
    m_rows.clear();
    m_lineFirstRow.clear();

    // 时间列占用右侧一条窄列：不开交互或时间列收起时为 0，正文占满整宽。
    // 宽度按「胶囊 + 间距」算 —— 胶囊恒宽（各行不跳动），所以这里固定按 "88:88" 量。
    m_gutter = 0;
    m_timeGap = 0;
    if (m_seekable && m_timeVisible) {
        const QFontMetrics ftm(m_fontTime);
        m_timeGap = qMax(10, qRound(m_textPx * 0.7));
        const int capPad = qMax(6, m_textPx / 3);   // 胶囊内左右留白
        m_gutter = ftm.horizontalAdvance(QStringLiteral("88:88")) + capPad * 2 + m_timeGap;
    }

    // 留 4px 余量，避免最右一个字压在边缘被抗锯齿削掉一半
    const int w = width() - 4 - m_gutter;

    if (m_plain) {
        // 纯文本模式：没有句的概念，所有行 line 都是 -1，不参与高亮
        const QStringList lines = m_plainText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            for (const QString &piece : wrapText(line.trimmed(), m_fontBold, w))
                m_rows.push_back({ piece, -1 });
        }
        return;
    }

    // 记录每句的首行下标：m_lineFirstRow[i] 是第 i 句在 m_rows 里的起点，
    // 相邻两项之差就是这句折出来的行数（最后一句要用 m_rows.size() 收尾）
    for (int i = 0; i < m_texts.size(); ++i) {
        m_lineFirstRow << int(m_rows.size());
        for (const QString &piece : wrapText(m_texts.at(i), m_fontBold, w))
            m_rows.push_back({ piece, i });
    }
}

// 宽度变化等于全部重新折行，行号索引会整体失效，必须重建 + 立即重算目标位置。
void LyricsView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    rebuild();
    updateTarget(false);
    update();
}

// 150ms 的提前量：接口上报进度本身有延迟，加上它高亮切换才不会比听感慢半拍。
// 找不到（还没到第一句）时返回 0 而不是 -1，这样一进歌就有句子被点亮。
int LyricsView::currentLineFor(qint64 ms) const
{
    if (m_times.isEmpty())
        return -1;

    int idx = -1;
    for (int i = 0; i < m_times.size(); ++i) {
        if (m_times.at(i) <= ms + 150)
            idx = i;
        else
            break;
    }
    return idx < 0 ? 0 : idx;
}

// 热点路径：300ms 一次的进度回调都走这里，句号没变就直接返回。
void LyricsView::setPosition(qint64 ms)
{
    if (m_plain || m_rows.isEmpty())
        return;

    const int idx = currentLineFor(ms);
    if (idx == m_currentLine)
        return;

    m_currentLine = idx;
    // 用户正在用滚轮翻阅：只更新高亮，不把视图拽回当前句（否则滚不动）
    if (m_userScrolling) {
        update();
        return;
    }
    updateTarget(true);
    update();
}

// 紧凑模式切换会改变「是否滚动」，必须把目标重算一遍并复位，否则会带着旧偏移绘制。
void LyricsView::setCompact(bool on)
{
    if (m_compact == on)
        return;
    m_compact = on;
    updateTarget(false);
    update();
}

// 只保留水平方向的三种对齐，垂直居中由各处的 AlignVCenter 固定负责。
void LyricsView::setTextAlignment(Qt::Alignment a)
{
    Qt::Alignment h = a & Qt::AlignHorizontal_Mask;
    if (h != Qt::AlignLeft && h != Qt::AlignHCenter && h != Qt::AlignRight)
        h = Qt::AlignHCenter;   // 没给水平位（或给了非水平位）时按默认居中
    if (m_align == h)
        return;
    m_align = h;
    update();   // 折行结果与行号都没变，只需要重画
}

/**
 * 重算滚动目标并（可选）平滑过去。
 * @param animate true = 用 220ms 动画过渡（换句时）；false = 直接跳到位
 *                （换歌/改字号/改宽度时用，避免整屏滑一次）
 * 紧凑模式下无条件归零：只画一句，滚动没有意义。
 * 副作用：写 m_targetRow，必要时启停 m_anim 并触发 update()。
 */
void LyricsView::updateTarget(bool animate)
{
    // 紧凑模式只显示当前句，不需要滚动
    if (m_compact) {
        m_anim->stop();
        m_scrollRow = 0;
        m_targetRow = 0;
        update();
        return;
    }

    // 可视行数按「控件高度 / 行高」估算；高度可能为 0（刚构造），夹到至少 1
    const int visibleRows = qMax(1, height() / qMax(1, m_rowH));

    qreal target = 0;
    if (!m_rows.isEmpty() && m_currentLine >= 0 && m_currentLine < m_lineFirstRow.size()) {
        // 多行时当前句放在正中间（上下都能看到上下文）；只有一行时本来就在中线
        target = m_lineFirstRow.at(m_currentLine) - (visibleRows - 1) * 0.5;
        // 上限 = 滚到底时首行还能贴住的最后一个位置，多加 1 是为了末句也能居中偏下的余量
        const qreal maxScroll = qMax(0, int(m_rows.size()) - visibleRows + 1);
        target = qBound<qreal>(0, target, maxScroll);
    }

    m_targetRow = target;

    // qFuzzyCompare 传的是 +1 之后的绝对值，避免 0 附近比较失效
    if (!animate || qFuzzyCompare(1.0 + m_scrollRow, 1.0 + target)) {
        m_anim->stop();
        m_scrollRow = target;
        update();
        return;
    }

    m_anim->stop();
    m_anim->setStartValue(m_scrollRow);
    m_anim->setEndValue(target);
    m_anim->start();
}

// 三套绘制分支：无内容画提示语 / 紧凑模式只画当前句 / 普通模式画所有可见行。
// 行坐标统一用「行号 × m_rowH − 滚动像素」，所以 m_rowH 必须是常量。
void LyricsView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHints(QPainter::TextAntialiasing | QPainter::Antialiasing);

    if (m_rows.isEmpty()) {
        // 无内容：画提示语，水平居中 + 垂直居中，超宽用省略号，不折行（提示语都很短）
        p.setFont(m_font);
        p.setPen(m_otherColor);
        const QFontMetrics fm(m_font);
        const QString text = fm.elidedText(m_placeholder, Qt::ElideRight, width());
        p.drawText(QRect(0, 0, width(), height()), m_align | Qt::AlignVCenter, text);
        return;
    }

    const qreal top = scrollTop();

    const int curRow = (m_currentLine >= 0 && m_currentLine < m_lineFirstRow.size())
                           ? m_lineFirstRow.at(m_currentLine)
                           : -1;

    // ---- 紧凑模式：只画当前句（可能折成多行），整体垂直居中 ----
    // 刚设好歌词、还没收到播放位置时 m_currentLine 可能是 -1，这里按第一句处理
    if (m_compact && !m_plain && !m_lineFirstRow.isEmpty()) {
        const int line = (m_currentLine >= 0 && m_currentLine < m_lineFirstRow.size())
                             ? m_currentLine
                             : 0;
        const int firstRow = m_lineFirstRow.at(line);
        // 最后一句没有下一句的首行可减，边界取总行数
        const int lastRow = (line + 1 < m_lineFirstRow.size())
                                ? m_lineFirstRow.at(line + 1)
                                : int(m_rows.size());
        const int maxRows = qMax(1, height() / qMax(1, m_rowH));
        const int wrapped = qMax(1, lastRow - firstRow);

        p.setClipRect(rect());
        p.setFont(m_fontBold);
        p.setPen(m_curColor);

        // 只有一行高度但这句折成了多行：整句压成一行 + 省略号，比只显示前半句好
        if (maxRows == 1 && wrapped > 1) {
            const QString full = m_texts.value(line);
            const QString shown = QFontMetrics(m_fontBold).elidedText(full, Qt::ElideRight, width());
            p.drawText(QRectF(0, 0, width(), height()),
                       m_align | Qt::AlignVCenter, shown);
            return;
        }

        // 只画放得下的行数（n），并按总高度居中，超出部分直接丢掉
        const int n = qMin(wrapped, maxRows);
        const qreal total = n * m_rowH;
        const qreal y0 = qMax(0.0, (height() - total) / 2.0);
        for (int k = 0; k < n; ++k) {
            p.drawText(QRectF(0, y0 + k * m_rowH, width(), m_rowH),
                       m_align | Qt::AlignVCenter, m_rows.at(firstRow + k).text);
        }
        return;
    }

    // ---- 普通模式：画所有可见行，当前句加粗高亮，其余按距离淡出 ----
    for (int i = 0; i < m_rows.size(); ++i) {
        const qreal y = i * m_rowH - top;
        // 顶部露一半的行直接不画：半截字比留白更难看
        if (y < -1)
            continue;
        if (y > height())
            break;

        const Row &row = m_rows.at(i);
        // 纯文本模式 row.line 恒为 -1，永远不会命中高亮
        const bool isCurrent = (row.line >= 0 && row.line == m_currentLine);

        QColor color = m_otherColor;
        if (isCurrent) {
            color = m_curColor;
        } else if (curRow >= 0) {
            // 离当前句越远越淡，形成层次：每远一行降 55 alpha，但最低保留 70 以保证可读
            const int dist = qAbs(i - curRow);
            const int alpha = qBound(70, 255 - dist * 55, 255);
            color.setAlpha(alpha);
        }
        // 悬停行整体提亮，提示「这行可以点」
        if (i == m_hoverRow)
            color = QColor(m_curColor.red(), m_curColor.green(), m_curColor.blue(), 230);

        // 时间只在**鼠标悬停的那一行**右侧出现，做成胶囊：
        // 平时不显示（不然每行都挂一串时间太吵），滚轮翻阅时指哪行看哪行。
        if (m_gutter > 0 && i == m_hoverRow && row.line >= 0 && row.line < m_times.size()) {
            const int capW = m_gutter - m_timeGap;     // 恒宽：各行时间胶囊长度一致，不会跳
            const int capH = qMax(14, m_rowH - 8);
            const qreal capX = width() - capW - 2;     // 右边留 2px，别贴死窗口边
            const qreal capY = y + (m_rowH - capH) / 2.0;
            const QRectF cap(capX, capY, capW, capH);
            const QColor base(color.red(), color.green(), color.blue());

            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            QColor fill = base;
            fill.setAlpha(26);                          // 极淡的底，只为了撑出胶囊形状
            QColor stroke = base;
            stroke.setAlpha(120);
            p.setPen(QPen(stroke, 1.0));
            p.setBrush(fill);
            // 半径取高度一半 = 正胶囊
            p.drawRoundedRect(cap.adjusted(0.5, 0.5, -0.5, -0.5), capH / 2.0, capH / 2.0);

            p.setBrush(Qt::NoBrush);
            p.setFont(m_fontTime);
            QColor tc = base;
            tc.setAlpha(215);
            p.setPen(tc);
            p.drawText(cap, Qt::AlignCenter, timeText(m_times.at(row.line)));
            p.restore();
        }

        p.setFont(isCurrent ? m_fontBold : m_fontSmall);
        p.setPen(color);
        p.drawText(QRectF(0, y, width() - m_gutter, m_rowH),
                   m_align | Qt::AlignVCenter, row.text);
    }
}

// 绘制与命中测试共用的顶部偏移。
// 歌词不够一屏时整体垂直居中（否则会全部贴顶，看起来像被裁掉了上半部分）；
// top <= 0.5 这个条件保证只在没有真正滚动时才居中，滚动中始终以行号为基准。
qreal LyricsView::scrollTop() const
{
    qreal top = m_scrollRow * m_rowH;
    const int visibleRows = qMax(1, height() / qMax(1, m_rowH));
    if (int(m_rows.size()) <= visibleRows && top <= 0.5)
        top = -(height() - int(m_rows.size()) * m_rowH) / 2.0;
    return top;
}

// scrollTop 的逆运算：控件坐标 → 视觉行号。
int LyricsView::rowAtY(int y) const
{
    if (m_rows.isEmpty() || m_rowH <= 0)
        return -1;
    const qreal row = (y + scrollTop()) / qreal(m_rowH);
    const int i = int(std::floor(row));
    if (i < 0 || i >= m_rows.size())
        return -1;
    return i;
}

// mm:ss；超过一小时的曲目也会退化成 mm:ss，歌词里没人关心小时
QString LyricsView::timeText(qint64 ms)
{
    const qint64 total = qMax<qint64>(0, ms) / 1000;
    return QStringLiteral("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

void LyricsView::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_seekable) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    const int i = rowAtY(int(e->position().y()));
    // 只有「属于某一句」的行才可点；纯文本行的 line 是 -1
    const int hover = (i >= 0 && m_rows.at(i).line >= 0) ? i : -1;
    if (hover != m_hoverRow) {
        m_hoverRow = hover;
        update();
    }
    // 手型只在时间胶囊上出现：点歌词正文不跳转，别给出可点的错觉
    const bool onTime = hover >= 0 && m_gutter > 0
                        && int(e->position().x()) >= width() - m_gutter;
    setCursor(onTime ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(e);
}

void LyricsView::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_seekable || e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    // 只有点**右侧的时间胶囊**才跳转；点歌词正文一律不响应
    if (m_gutter <= 0 || int(e->position().x()) < width() - m_gutter) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    const int i = rowAtY(int(e->position().y()));
    const int line = (i >= 0) ? m_rows.at(i).line : -1;
    if (line < 0 || line >= m_times.size()) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    // 点了就立刻恢复跟随，否则 seek 之后视图还停在用户翻的位置，看起来像没跳
    m_userScrolling = false;
    if (m_resumeTimer)
        m_resumeTimer->stop();
    emit seekRequested(m_times.at(line));
    e->accept();
}

void LyricsView::leaveEvent(QEvent *e)
{
    if (m_hoverRow != -1) {
        m_hoverRow = -1;
        update();
    }
    // 鼠标移开就立刻收起时间列（不等 3s 自然超时）
    setTimeColumnVisible(false);
    if (m_timeHideTimer)
        m_timeHideTimer->stop();
    unsetCursor();
    QWidget::leaveEvent(e);
}

void LyricsView::wheelEvent(QWheelEvent *e)
{
    // 没开交互就把滚轮让出去：窗口层可能用它切换组合成员
    if (!m_seekable || m_rows.isEmpty() || e->angleDelta().y() == 0) {
        QWidget::wheelEvent(e);
        return;
    }
    const int visibleRows = qMax(1, height() / qMax(1, m_rowH));
    const qreal maxScroll = qMax(0, int(m_rows.size()) - visibleRows + 1);
    // 一格（120）滚一行，向下滚 = 向后看
    const qreal step = e->angleDelta().y() / 120.0;
    m_anim->stop();
    m_scrollRow = qBound<qreal>(0, m_scrollRow - step, maxScroll);
    m_targetRow = m_scrollRow;
    m_userScrolling = true;
    if (m_resumeTimer)
        m_resumeTimer->start();

    // 时间列「滚动才出现」：第一下滚动展开，之后每滚一下把 3s 计时器重新压满，
    // 连续翻阅期间不会中途消失
    setTimeColumnVisible(true);
    if (m_timeHideTimer)
        m_timeHideTimer->start();

    // 滚轮本身不产生 mouseMove，但内容在鼠标底下移动了 —— 悬停行必须跟着重算，
    // 否则胶囊会停在已经滚走的旧行上。
    if (underMouse()) {
        const QPoint local = mapFromGlobal(QCursor::pos());
        const int hi = rowAtY(local.y());
        m_hoverRow = (hi >= 0 && m_rows.at(hi).line >= 0) ? hi : -1;
    }

    update();
    e->accept();
}
