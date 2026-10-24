/**
 * @file WeatherWidget.cpp
 * 天气组件的实现：一次性绘制整个面板 + Open-Meteo 两步请求（地理编码 → 预报）+ 磁盘缓存。
 *
 * 坑：
 *  - 面板完全自绘，任何布局改动都要同时顾及三档版式（compact / wide / 竖排），
 *    否则某一档会溢出或留白；
 *  - 网络回调里不要直接 delete reply：lambda 里先 deleteLater() 再进处理函数，
 *    处理期间对象仍然有效；
 *  - refresh() 用 m_busy 防重入，所以地理编码成功回调里必须先把 m_busy 置 false 再调 refresh()。
 */
#include "widgets/WeatherWidget.h"

#include "core/AppSettings.h"
#include "core/Paths.h"
#include "widgets/WeatherIcons.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace
{

/**
 * WMO 天气代码 → 中文短描述。
 * @param code 接口 weather_code，Open-Meteo 用的是 WMO 4677 的子集
 * @return 固定几个字的描述（用于单行显示）；未知代码返回「未知」而不是空串，
 *         因为空串会被上层当成「还没有数据」
 */
QString weatherText(int code)
{
    // 区间分组沿用 WMO 定义：51-57 毛毛雨、61-67 雨、71-77 雪、80-82 阵雨、85/86 阵雪、95-99 雷暴
    if (code == 0)
        return QStringLiteral("晴");
    if (code == 1)
        return QStringLiteral("晴间多云");
    if (code == 2)
        return QStringLiteral("多云");
    if (code == 3)
        return QStringLiteral("阴");
    if (code == 45 || code == 48)
        return QStringLiteral("雾");
    if (code >= 51 && code <= 57)
        return QStringLiteral("毛毛雨");
    if (code >= 61 && code <= 67)
        return QStringLiteral("雨");
    if (code >= 71 && code <= 77)
        return QStringLiteral("雪");
    if (code >= 80 && code <= 82)
        return QStringLiteral("阵雨");
    if (code == 85 || code == 86)
        return QStringLiteral("阵雪");
    if (code == 95)
        return QStringLiteral("雷阵雨");
    if (code == 96 || code == 99)
        return QStringLiteral("雷暴伴冰雹");
    return QStringLiteral("未知");
}

/**
 * 逐日预报的日期标签。
 * @param isoDate 接口返回的 "yyyy-MM-dd"；offset 为 1/2 时直接返回「明天/后天」不解析日期
 * @param offset  相对今天的天数，0 表示今天（本函数不处理，由调用方写成「今天」）；
 *                1 = 明天、2 = 后天，之后按星期几显示
 * @return 中文标签；日期解析失败返回 "--"
 */
QString weekdayName(const QString &isoDate, int offset)
{
    // 下标跟 QDate::dayOfWeek()（1=周一 … 7=周日）对齐，所以下面用 dayOfWeek() % 7 取值：
    // 7 % 7 = 0 正好落在「周日」
    static const QStringList names = { QStringLiteral("周日"), QStringLiteral("周一"),
                                       QStringLiteral("周二"), QStringLiteral("周三"),
                                       QStringLiteral("周四"), QStringLiteral("周五"),
                                       QStringLiteral("周六") };
    if (offset == 1)
        return QStringLiteral("明天");
    if (offset == 2)
        return QStringLiteral("后天");
    const QDate d = QDate::fromString(isoDate, QStringLiteral("yyyy-MM-dd"));
    return d.isValid() ? names.value(d.dayOfWeek() % 7) : QStringLiteral("--");
}

/**
 * 天气图标改由 WeatherIcons 绘制（Meteocons 矢量图标集，随程序内置），
 * 本文件只负责版式与文字，不再维护任何矢量图形代码。
 */

} // namespace

// 构造只搭骨架：网络管理器 + 自动刷新定时器 + 先把磁盘缓存铺上屏；
// 真正的网络请求推迟到事件循环首轮（末尾的 singleShot），等宿主灌完 BoxConfig 再发，
// 否则会拿全局默认城市去地理编码，白跑一次请求
WeatherWidget::WeatherWidget(QWidget *parent)
    : WidgetPanel(parent)
{
    // 本控件不是「全不透明」：留白处要让父窗口的玻璃背景透出来，所以把
    // WA_OpaquePaintEvent 显式置 false，由 Qt 先画父控件再画自己
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);

    m_net = new QNetworkAccessManager(this);
    // 初始城市取全局设置，后面 restoreState() 可能用盒子自己的配置覆盖它
    m_cityName = AppSettings::get().weatherCity;
    m_updatedText = QStringLiteral("正在获取…");

    const int minutes = AppSettings::get().widgetRefreshMin;
    m_timer = new QTimer(this);
    // 0 表示用户关掉了自动刷新，此时只保留构造末尾的那次单次刷新和菜单里的手动刷新
    if (minutes > 0) {
        // 设置里存的是分钟，定时器要毫秒
        m_timer->setInterval(minutes * 60 * 1000);
        connect(m_timer, &QTimer::timeout, this, &WeatherWidget::refresh);
        m_timer->start();
    }

    // 先把磁盘缓存画上屏，避免空白等网络；缓存里的城市与配置不符时会被 loadCache 丢弃
    loadCache();
    // 让 restoreState 先跑完（构造后立即调用），再发起网络请求
    // 用 singleShot(0) 而不是直接调：此刻宿主还没把 BoxConfig.widgetState 灌进来，
    // 立刻请求会用错城市去地理编码
    QTimer::singleShot(0, this, &WeatherWidget::refresh);
}

// 纯计算：只读当前宽高与 m_headerHidden，不写任何成员。所以同一次 paint 里多次调用结果一致，
// 也就不需要「版式变了别忘了置脏」这种失效逻辑
WeatherWidget::Metrics WeatherWidget::metrics() const
{
    Metrics m;
    const int w = width();
    const int h = height();

    // 窄于 200 / 矮于 110 时收紧留白，把像素让给内容；隐藏标题栏后上下各多留 2px，
    // 因为没有标题栏做视觉分隔，贴边会显得内容「顶着窗口边」
    m.marginX = w < 200 ? 10 : 14;
    m.marginY = h < 110 ? 7 : (m_headerHidden ? 12 : 10);

    // 下限 40/30 是为了让后面的比例运算不至于算出 0 或负数（极端小尺寸时窗口还没被
    // 最小尺寸兜住，这里先自保）
    const int availW = qMax(40, w - 2 * m.marginX);
    const int availH = qMax(30, h - 2 * m.marginY);

    // 字号与图标一起随可用空间缩放，并受宽度约束，避免「字大框小」被截断
    // 每个尺寸都取「高度比例」和「宽度比例」中的较小值：前者保证竖着放得下，
    // 后者保证横着不溢出；再 qBound 夹到各自的可读区间（太小看不清，太大比例失真）
    m.tempPx  = qBound(18, qMin(int(availH * 0.32), int(availW * 0.30)), 54);
    m.glyph   = qBound(14, qMin(int(availH * 0.42), int(availW * 0.23)), 60);
    m.descPx  = qBound(9, qMin(int(availH * 0.100), int(availW * 0.075)), 15);
    m.detailPx = qBound(9, qMin(int(availH * 0.080), int(availW * 0.056)), 12);
    m.dayPx    = qBound(9, qMin(int(availH * 0.076), int(availW * 0.054)), 12);
    m.updatedPx = qBound(8, qMin(int(availH * 0.066), int(availW * 0.048)), 11);

    // 定档阈值是实测出来的「刚好放不下第二行」的界线：高度不足 86 时描述行会压到
    // 图标上；宽度不足 186 时「图标 + 温度 + 描述」一行会互相挤
    m.compact = (availH < 86) || (availW < 186);
    // 两栏要够宽（左栏至少放得下图标 + 温度），且高度不能太高 —— 太高的两栏会留下
    // 大片空白，竖排反而能塞下逐日预报
    m.wide = !m.compact && availW >= 340 && availH >= 100 && availH <= 215;
    return m;
}

// 整个面板一次画完：没有子控件、没有布局，所以尺寸一变直接重进这里重排版。
// 三档版式互斥且提前 return，任何一档都不会画到别人的区域
void WeatherWidget::paintEvent(QPaintEvent *)
{
    // 刻意不给形参起名：重绘区域一律忽略，整块重画 —— 版式随尺寸整体变化，
    // 局部更新省下的那点时间不值得冒「旧版式残留」的风险
    // 绘制顺序：算版式 → 算内容矩形 → （可选）右上角城市标签 → 无数据则只画状态行 →
    // 否则按 compact / wide / 竖排 三选一排版。所有文字都走 elidedText，宁可省略也不溢出。
    // 取引用而不是拷贝：AppSettings 是进程级单例，颜色/字体在每次 paint 时现读，
    // 所以主题切换只要宿主 update() 一次就会生效，不需要额外的「重设主题」入口
    const AppSettings &st = AppSettings::get();
    const Metrics m = metrics();

    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    const QRect content(m.marginX, m.marginY,
                        qMax(0, width() - 2 * m.marginX),
                        qMax(0, height() - 2 * m.marginY));
    if (content.width() < 36 || content.height() < 20)
        return; // 连一个图标加温度都放不下，直接不画（此时窗口通常正被拖到极小）

    const QColor text = st.textColor();
    const QColor sub = st.subTextColor();

    // 每个字号档位各建一份 QFont + QFontMetrics：后面要反复量文本宽高（elide、右对齐），
    // 复用度量对象比每处临时构造便宜
    const QFont tempF = st.uiFont(m.tempPx, QFont::DemiBold);
    const QFont descF = st.uiFont(m.descPx);
    const QFont detailF = st.uiFont(m.detailPx);
    const QFont dayF = st.uiFont(m.dayPx);
    const QFont updF = st.uiFont(m.updatedPx);

    const QFontMetrics fmTemp(tempF);
    const QFontMetrics fmDesc(descF);
    const QFontMetrics fmDetail(detailF);
    const QFontMetrics fmDay(dayF);
    const QFontMetrics fmUpd(updF);

    // 隐藏标题栏后没有地方显示城市，这里补一个右上角的小标签
    const bool showCity = m_headerHidden && !m_cityName.isEmpty();
    int cityW = 0;
    if (showCity) {
        // 最多占内容区宽度的 40%，否则长城市名会把第一行的温度挤没；
        // 文字宽度 +4 是给 elide 留判定余量，避免最后一个字被误判为放不下
        cityW = qMin(int(content.width() * 0.40), fmUpd.horizontalAdvance(m_cityName) + 4);
        p.setFont(updF);
        p.setPen(sub);
        p.drawText(QRect(content.right() - cityW, content.top(), cityW, fmUpd.height()),
                   Qt::AlignRight | Qt::AlignVCenter,
                   fmUpd.elidedText(m_cityName, Qt::ElideRight, cityW));
    }
    // 第一行的可用右边界：扣掉城市标签和它与内容之间的 10px 间距
    const int firstRowRight = content.right() - (showCity ? cityW + 10 : 0);

    // ---- 还没有数据 ----
    // 用 m_descText 是否为空判断而不是单独的 bool：它和「有完整实况」是等价的，
    // 少维护一个状态位就少一处不一致
    if (m_descText.isEmpty()) {
        p.setFont(descF);
        p.setPen(sub);
        const QString tip = m_updatedText.isEmpty() ? QStringLiteral("正在获取…") : m_updatedText;
        p.drawText(content, Qt::AlignLeft | Qt::AlignVCenter,
                   fmDesc.elidedText(tip, Qt::ElideRight, firstRowRight - content.left() + 1));
        return;
    }

    const QString tempText = QStringLiteral("%1°").arg(m_tempValue);
    // 详情行固定用「 · 」分隔，整体只占一行，放不下就省略尾部（风速先被截掉）
    const QString detailText = QStringLiteral("体感 %1° · 湿度 %2% · 风 %3km/h")
                                   .arg(m_feels)
                                   .arg(m_humidity)
                                   .arg(m_wind);
    // 图标与文字之间的间距随图标大小走（图标越大，留白也要越大才不显拥挤），下限 6px
    const int gap = qMax(6, m.glyph / 5);
    const int tempW = fmTemp.horizontalAdvance(tempText) + 2;
    // 第一行高度取图标与温度字高的较大者：两者并排，必须都能完整放下
    const int row0H = qMax(m.glyph, fmTemp.height());

    // 富余高度拿来当留白用，避免高个子组件内容全挤在顶部
    // 250 以下是「内容本来就能填满」的区间，超过后每多 12px 才加 1px 间距，上限 10px，
    // 这样窗口拖得再高也不会把行距拉得离谱
    const int spread = qBound(0, (content.height() - 250) / 12, 10);

    // ---- 紧凑版式：一行 [图标][温度][描述] ----
    if (m.compact) {
        // 三样东西都垂直居中于内容区，因为它们高度不同，靠基线对齐反而会错位
        const int iconY = content.top() + (content.height() - m.glyph) / 2;
        WeatherIcons::paint(p, QRectF(content.left(), iconY, m.glyph, m.glyph), m_code, st.isDark());

        const int tx = content.left() + m.glyph + gap;
        p.setFont(tempF);
        p.setPen(text);
        // 温度矩形宽度同时受「文字实际宽度」和「剩余空间」限制，后者不够时会被裁掉
        const QRect tempRect(tx, content.top() + (content.height() - fmTemp.height()) / 2,
                             qMin(tempW, qMax(0, firstRowRight - tx + 1)), fmTemp.height());
        p.drawText(tempRect, Qt::AlignLeft | Qt::AlignVCenter, tempText);

        const int dx = tx + tempRect.width() + gap;
        const int dw = firstRowRight - dx + 1;
        // 少于 28px 只够显示一两个字，等于没信息量，干脆留白
        if (dw >= 28) {
            p.setFont(descF);
            p.setPen(sub);
            p.drawText(QRect(dx, content.top(), dw, content.height()),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       fmDesc.elidedText(m_descText, Qt::ElideRight, dw));
        }
        return;
    }

    // 更新时间固定贴底右对齐，所以先把它占的高度从可用高度里预留出来，
    // 否则逐日预报会画到它身上；+2 是行间距
    const int updatedH = fmUpd.height();
    const int reserveBottom = m_updatedText.isEmpty() ? 0 : updatedH + 2;

    /**
     * 在内容区底部画一行右对齐的状态文字（时间 / 错误提示），放不下就省略。
     * @param left       可用区域左边界
     * @param rightLimit 可用区域右边界（含）
     */
    auto drawUpdated = [&](int left, int rightLimit)
    {
        if (m_updatedText.isEmpty())
            return;
        p.setFont(updF);
        p.setPen(sub);
        const int w = qMax(0, rightLimit - left + 1);
        p.drawText(QRect(left, content.bottom() - updatedH + 1, w, updatedH),
                   Qt::AlignRight | Qt::AlignVCenter,
                   fmUpd.elidedText(m_updatedText, Qt::ElideRight, w));
    };

    // ---- 宽扁版式：左栏温度与描述，右栏详情与预报 ----
    if (m.wide) {
        // 左栏宽度按内容区 40% 走，但夹在 96..212：太窄放不下图标 + 温度，太宽右栏就没地方了
        const int leftW = qBound(96, int(content.width() * 0.40), 212);

        WeatherIcons::paint(p, QRectF(content.left(), content.top(), m.glyph, m.glyph),
                            m_code, st.isDark());

        const int tempLeft = content.left() + m.glyph + gap;
        const int tempAvail = qMax(0, leftW - m.glyph - gap);
        // 剩余不足 30px 就跳过温度，硬画会被裁成「2…」这种误导性内容
        if (tempAvail >= 30) {
            p.setFont(tempF);
            p.setPen(text);
            p.drawText(QRect(tempLeft, content.top(), tempAvail, row0H),
                       Qt::AlignLeft | Qt::AlignVCenter, tempText);
        }

        // 描述紧接着第一行往下排，spread/3 是随窗口变高而增加的一点喘息空间
        int ly = content.top() + row0H + 2 + spread / 3;
        // 放不下就整行不画：宁可少一行，也不要压住下面的逐日列表
        if (ly + fmDesc.height() <= content.bottom()) {
            p.setFont(descF);
            p.setPen(text);
            p.drawText(QRect(content.left(), ly, leftW, fmDesc.height()),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       fmDesc.elidedText(m_descText, Qt::ElideRight, leftW));
        }

        // 右栏从「左栏 + 14px 栏间距」开始，宽度至少 46px 才有意义（一行「今天 12° / 3°」的量级）
        const int rx = content.left() + leftW + 14;
        int rw = content.right() - rx + 1;
        if (rw < 46)
            return;
        // 城市标签在右上角，右栏要主动让开它，否则两段文字会叠在一起
        if (showCity)
            rw = qMax(40, rw - cityW - 10);

        int ry = content.top();
        if (ry + fmDetail.height() <= content.bottom() - reserveBottom) {
            p.setFont(detailF);
            p.setPen(sub);
            p.drawText(QRect(rx, ry, rw, fmDetail.height()),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       fmDetail.elidedText(detailText, Qt::ElideRight, rw));
            ry += fmDetail.height() + 4 + spread / 3;
        }

        const int dayLineH = fmDay.height() + 2;
        // 行高不够就不画小图标：< 16px 的行高只够塞 14px 图标，图标会顶到行框上下边
        // iconD 取「行高 - 2」再夹到 14..22，既保证不撑破行框，也不会小到看不清
        const int iconD = dayLineH >= 16 ? qBound(14, dayLineH - 2, 22) : 0;
        for (const Day &d : m_days) {
            if (!d.valid)
                continue;
            // 逐日行会一路排到底部，必须先让出更新时间的位置
            if (ry + dayLineH > content.bottom() - reserveBottom)
                break;
            if (iconD > 0)
                WeatherIcons::paint(p, QRectF(rx, ry + (dayLineH - iconD) / 2.0, iconD, iconD),
                                    d.code, st.isDark());
            // 温度固定右对齐，先量出它的宽度，剩下的才是标签能用多宽
            const QString temps = QStringLiteral("%1° / %2°").arg(d.hi).arg(d.lo);
            const int tw = fmDay.horizontalAdvance(temps);
            p.setFont(dayF);
            const int labelX = rx + (iconD > 0 ? iconD + 6 : 0);
            const int labelW = qMax(0, (rx + rw - tw - 8) - labelX);
            // 少于 20px 时「明天」这类标签会被省略成「…」，索性不画
            if (labelW > 20) {
                p.setPen(text);
                p.drawText(QRect(labelX, ry, labelW, dayLineH),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           fmDay.elidedText(d.label, Qt::ElideRight, labelW));
            }
            p.setPen(sub);
            p.drawText(QRect(rx + rw - tw, ry, tw, dayLineH),
                       Qt::AlignRight | Qt::AlignVCenter, temps);
            ry += dayLineH + spread / 2;
        }

        if (content.bottom() - ry >= updatedH - 1)
            drawUpdated(rx, rx + rw - 1);
        return;
    }

    // ---- 竖排版式：先算能放几行，再把富余高度均分到行间距 ----
    // 各行的固定高度（不含行间距），后面用它们累加判断还能塞下几行
    const int hHead = row0H;
    const int hDesc = fmDesc.height();
    const int hDetail = fmDetail.height();
    const int hDay = fmDay.height() + 2;
    const int hSep = 4;       // 详情行与逐日列表之间那条分隔线的占位高度
    int gDesc = 2;            // 头部 → 描述 的间距
    int gDetail = 1;          // 描述 → 详情 的间距
    int gSep = 3;             // 分隔线上下的间距
    int gDay = 0;             // 逐日行之间：默认贴紧，靠下面均分的 extra 撑开

    const int usable = content.height() - reserveBottom;
    const int lineRight = content.right();
    const int lineW = lineRight - content.left() + 1;

    // 先按「头部 + 描述 + 详情」算最小高度；放不下就砍掉详情行（体感/湿度/风），
    // 这是信息密度最低的一档，砍掉后仍保留温度和逐日预报
    int base = hHead + gDesc + hDesc + gDetail + hDetail;
    const bool detailFits = (base <= usable);
    if (!detailFits)
        base = hHead + gDesc + hDesc;

    // 贪心从上往下试放逐日行：每次加上「行高 + 间距」，超出可用高度就停。
    // 第一天前面如果要画分隔线，还要额外算上 hSep + gSep
    int nDays = 0;
    int total = base;
    for (int i = 0; i < m_days.size(); ++i) {
        if (!m_days.at(i).valid)
            continue;
        const int add = hDay + ((nDays == 0 && detailFits) ? hSep + gSep : gDay);
        if (total + add > usable)
            break;
        total += add;
        ++nDays;
    }

    // 余量均分给行间，避免高个子组件「上挤下空」
    // rowCount 是实际会画的行数；上限 18px 防止窗口很高时行距大到失去关联
    const int rowCount = 2 + (detailFits ? 1 : 0) + nDays;
    const int extra = qBound(0, (usable - total) / qMax(1, rowCount), 18);
    gDesc += extra;
    gDetail += extra;
    gSep += extra;
    gDay += extra;

    int y = content.top();
    WeatherIcons::paint(p, QRectF(content.left(), y, m.glyph, m.glyph), m_code, st.isDark());
    const int tempLeft = content.left() + m.glyph + gap;
    if (firstRowRight - tempLeft >= 30) {
        p.setFont(tempF);
        p.setPen(text);
        p.drawText(QRect(tempLeft, y, firstRowRight - tempLeft + 1, hHead),
                   Qt::AlignLeft | Qt::AlignVCenter, tempText);
    }
    y += hHead + gDesc;

    if (y + hDesc <= content.bottom() - reserveBottom) {
        p.setFont(descF);
        p.setPen(text);
        p.drawText(QRect(content.left(), y, lineW, hDesc), Qt::AlignLeft | Qt::AlignVCenter,
                   fmDesc.elidedText(m_descText, Qt::ElideRight, lineW));
        y += hDesc + gDetail;
    }

    if (detailFits) {
        p.setFont(detailF);
        p.setPen(sub);
        p.drawText(QRect(content.left(), y, lineW, hDetail), Qt::AlignLeft | Qt::AlignVCenter,
                   fmDetail.elidedText(detailText, Qt::ElideRight, lineW));
        y += hDetail;
    }

    // 与宽扁版式同一套规则：行高 >= 16 就给图标（下限 14px），上限 22px
    const int iconD = hDay >= 16 ? qBound(14, hDay - 2, 22) : 0;
    int dayIndex = 0;
    for (const Day &d : m_days) {
        if (!d.valid)
            continue;
        // 只画前面算出来的 nDays 行，多出来的（理论上不该有）直接丢弃
        if (dayIndex >= nDays)
            break;

        // 第一行逐日预报前画一条分隔线，把「实况」和「预报」两块分开；
        // 太窄（<=80px）时线短得没意义，就不画也不占位
        if (dayIndex == 0 && detailFits && lineW > 80) {
            p.setPen(QPen(st.borderColor(), 1));
            p.drawLine(QPointF(content.left(), y + hSep / 2.0),
                       QPointF(lineRight, y + hSep / 2.0));
            y += hSep + gSep;
        } else if (dayIndex > 0) {
            y += gDay;
        }

        if (iconD > 0)
            WeatherIcons::paint(p, QRectF(content.left(), y + (hDay - iconD) / 2.0, iconD, iconD),
                                d.code, st.isDark());
        const QString temps = QStringLiteral("%1° / %2°").arg(d.hi).arg(d.lo);
        const int tw = fmDay.horizontalAdvance(temps);
        p.setFont(dayF);
        const int labelX = content.left() + (iconD > 0 ? iconD + 6 : 0);
        const int labelW = qMax(0, (lineRight - tw - 6) - labelX);
        // 18px 比宽扁版式更紧：通栏时标签区右边界紧贴温度，只要还放得下一两个字就画
        if (labelW > 18) {
            p.setPen(text);
            p.drawText(QRect(labelX, y, labelW, hDay), Qt::AlignLeft | Qt::AlignVCenter,
                       fmDay.elidedText(d.label, Qt::ElideRight, labelW));
        }
        p.setPen(sub);
        p.drawText(QRect(lineRight - tw, y, tw, hDay),
                   Qt::AlignRight | Qt::AlignVCenter, temps);
        y += hDay;
        ++dayIndex;
    }

    if (!m_updatedText.isEmpty())
        drawUpdated(content.left(), lineRight);
}

// 只在真正变化时重绘：宿主会在每次应用配置时调用，重复 update() 会白白重排一次版式
void WeatherWidget::setHeaderHidden(bool hidden)
{
    if (m_headerHidden == hidden)
        return;
    m_headerHidden = hidden;
    update();
}

// 标题固定带城市：多城市并排时，光看「天气」两个字分不清是哪个盒子
QString WeatherWidget::panelTitle() const
{
    // 城市名可能来自全局设置（还没 restoreState），为空时只显示组件类型
    return m_cityName.isEmpty() ? QStringLiteral("天气")
                                : QStringLiteral("天气 · %1").arg(m_cityName);
}

// 只是给「不支持文字角标」的通用标题栏留的兼容出口；天气实际走 badgeText() 的「20°」
int WeatherWidget::badgeCount() const
{
    // -1 哨兵：没有数据时不显示角标；有数据时返回温度整数（会被 badgeText 覆盖掉）
    return m_descText.isEmpty() ? -1 : m_tempValue;
}

// 空串而不是 -1：文字角标的「不显示」约定是空串，两者不要混用
QString WeatherWidget::badgeText() const
{
    // 温度本身没有「数量」的含义，所以走文本角标带上度数
    return m_descText.isEmpty() ? QString() : QStringLiteral("%1°").arg(m_tempValue);
}

// 全局只有一份缓存文件（靠文件内的 city 字段判断归属），不按城市分文件：
// 用户通常只关心当前城市，多份文件反而要额外做清理
QString WeatherWidget::cacheFile() const
{
    return QDir(Paths::dataDir()).filePath(QStringLiteral("weather_cache.json"));
}

// 只改「城市名 + 坐标是否可信」，不发请求也不落盘：调用方决定何时 refresh()、何时写全局设置。
// 拆开是为了换城市时可以「先改界面、再等网络」，面板不会卡在空状态
void WeatherWidget::setCity(const QString &city, bool coordinateKnown)
{
    m_cityName = city;
    m_hasCoord = coordinateKnown;
    // 标题栏显示了城市名，这里只需要重绘
    update();
    // 标题栏文字变了，通知宿主刷新标题
    emit panelChanged();
}

// 两条路径：有坐标就直接取预报（一次往返），没坐标就先地理编码（两次往返），
// 地理编码成功后由 onGeocodeDone 再调回本函数，此时走上面那条快路径
void WeatherWidget::refresh()
{
    // 定时器与菜单/右键可能同时触发，m_busy 保证同一时刻只有一个请求在飞；
    // 注意：这样「刷新」在网络慢时会被静默丢弃，是刻意的取舍（比堆请求好）
    if (m_busy)
        return;
    m_busy = true;
    // 先改状态再发请求：网络慢时界面立刻变成「正在更新…」，用户看得到反馈；
    // 失败时再由 showError() 覆盖成错误文案，不会卡在这个中间态
    m_updatedText = QStringLiteral("正在更新…");
    update();

    if (m_hasCoord) {
        // 已知经纬度：直接请求预报，跳过地理编码这一步
        QUrl url(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
        QUrlQuery q;
        // 保留 4 位小数≈10m 精度，再高对天气没意义还会让 URL 变长
        q.addQueryItem(QStringLiteral("latitude"), QString::number(m_lat, 'f', 4));
        q.addQueryItem(QStringLiteral("longitude"), QString::number(m_lon, 'f', 4));
        // current 段：气温 / 相对湿度 / 体感 / 天气代码 / 10m 风速
        q.addQueryItem(QStringLiteral("current"),
                       QStringLiteral("temperature_2m,relative_humidity_2m,apparent_temperature,"
                                      "weather_code,wind_speed_10m"));
        q.addQueryItem(QStringLiteral("daily"),
                       QStringLiteral("weather_code,temperature_2m_max,temperature_2m_min"));
        q.addQueryItem(QStringLiteral("timezone"), QStringLiteral("auto"));
        // forecast_days 含今天，所以 4 天 = 今天 + 未来三天，与界面上的 4 个槽位对应
        q.addQueryItem(QStringLiteral("forecast_days"), QStringLiteral("4"));
        url.setQuery(q);

        QNetworkReply *reply = m_net->get(QNetworkRequest(url));
        // 先 deleteLater 再处理：删除被推迟到事件循环空闲，处理函数里 reply 仍然有效；
        // 放在 lambda 第一行还能保证任何提前 return 的分支都不会漏删
        connect(reply, &QNetworkReply::finished, this, [this, reply]
        {
            reply->deleteLater();
            onForecastDone(reply);
        });
        return;
    }

    // 还没坐标：先用城市名做地理编码（Open-Meteo 的 search 接口，不需要密钥）
    QUrl geo(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
    QUrlQuery gq;
    gq.addQueryItem(QStringLiteral("name"), m_cityName);
    // count=1 只取最匹配的一个，避免同名城市让用户再选一次
    gq.addQueryItem(QStringLiteral("count"), QStringLiteral("1"));
    gq.addQueryItem(QStringLiteral("language"), QStringLiteral("zh"));
    gq.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    geo.setQuery(gq);

    QNetworkReply *reply = m_net->get(QNetworkRequest(geo));
    connect(reply, &QNetworkReply::finished, this, [this, reply]
    {
        reply->deleteLater();
        onGeocodeDone(reply);
    });
}

// 地理编码只做一件事：把城市名换成经纬度，然后立刻复用 refresh() 的预报流程；
// 两种失败（网络、城市不存在）都只改底部提示，不清已有数据
void WeatherWidget::onGeocodeDone(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        // 失败必须清 m_busy，否则后面再也刷不了
        m_busy = false;
        showError(QStringLiteral("网络不可用"));
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    // 地理编码响应很小，整包读进内存再解析即可，不需要流式处理；
    // 解析失败时 object() 给空对象，下面的 results 自然为空，走「找不到城市」分支
    // results 为空 = 城市名没匹配到（拼错、或接口改了命名），不是网络问题
    const QJsonArray results = root.value(QStringLiteral("results")).toArray();
    if (results.isEmpty()) {
        m_busy = false;
        showError(QStringLiteral("找不到城市「%1」").arg(m_cityName));
        return;
    }
    const QJsonObject first = results.first().toObject();
    // 只取经纬度，地名用用户输入的名字（接口返回的可能是别的语种写法）
    m_lat = first.value(QStringLiteral("latitude")).toDouble();
    m_lon = first.value(QStringLiteral("longitude")).toDouble();
    m_hasCoord = true;
    // 先清 busy 再 refresh：refresh() 开头会因为 busy 直接返回
    m_busy = false;
    refresh();
}

// 预报回调：只有成功才落盘缓存（失败请求不该把上次的好数据冲掉），失败时界面保留旧数据，
// 仅把底部状态行换成错误提示
void WeatherWidget::onForecastDone(QNetworkReply *reply)
{
    // 无论成败都要放开 busy，否则下次定时刷新会被挡掉
    m_busy = false;
    if (reply->error() != QNetworkReply::NoError) {
        showError(QStringLiteral("获取失败（%1）").arg(reply->errorString()));
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    // 接口把参数错误放在顶层 "error" 字段里，HTTP 状态却可能是 200
    if (root.contains(QStringLiteral("error"))) {
        showError(QStringLiteral("接口返回错误"));
        return;
    }
    applyPayload(root, false);
    // 只在成功时覆盖缓存：失败请求不应把上次的好数据冲掉
    writeCache(root);
}

// 唯一的「写界面」入口：网络成功与读缓存两条路径都汇到这里，显示逻辑只有一份；
// 副作用是 update() + emit panelChanged()（后者让窗口刷新标题栏的「20°」角标）
void WeatherWidget::applyPayload(const QJsonObject &root, bool fromCache)
{
    const QJsonObject current = root.value(QStringLiteral("current")).toObject();
    const QJsonObject daily = root.value(QStringLiteral("daily")).toObject();
    // 下面统一用 value().toXxx() 取值：字段缺失时 QJsonValue 会给 0 / 空串，
    // 正好等于成员默认值，所以坏数据只会让界面显示 0°，不会崩

    // 接口给的是 double，界面只显示整数：温度四舍五入，湿度本来就是整数
    const double temp = current.value(QStringLiteral("temperature_2m")).toDouble();
    m_code = current.value(QStringLiteral("weather_code")).toInt();
    const double feels = current.value(QStringLiteral("apparent_temperature")).toDouble();
    m_humidity = current.value(QStringLiteral("relative_humidity_2m")).toInt();
    m_wind = qRound(current.value(QStringLiteral("wind_speed_10m")).toDouble());

    m_tempValue = qRound(temp);
    // m_descText 非空同时充当「已有数据」的开关，所以必须在这里赋值而不是绘制时算
    m_descText = weatherText(m_code);
    m_feels = qRound(feels);

    // daily 里每个字段都是等长数组，但缓存可能被截断/接口少返字段，所以逐个判长度
    const QJsonArray dates = daily.value(QStringLiteral("time")).toArray();
    const QJsonArray codes = daily.value(QStringLiteral("weather_code")).toArray();
    const QJsonArray maxs = daily.value(QStringLiteral("temperature_2m_max")).toArray();
    const QJsonArray mins = daily.value(QStringLiteral("temperature_2m_min")).toArray();

    m_days.clear();
    // 固定产出 4 个槽位而不是「有几个写几个」，绘制侧就不用再判断数组长度
    for (int i = 0; i < 4; ++i) {
        const int idx = i; // 0 是今天
        Day d;
        // 只校验 dates/maxs 这两个必需数组，codes 缺失时退化成晴（code 0），mins 缺失时 toDouble 给 0
        if (idx < dates.size() && idx < maxs.size()) {
            d.label = (i == 0) ? QStringLiteral("今天")
                               : weekdayName(dates.at(idx).toString(), i);
            d.code = codes.size() > idx ? codes.at(idx).toInt() : 0;
            d.hi = qRound(maxs.at(idx).toDouble());
            d.lo = qRound(mins.at(idx).toDouble());
            d.valid = true;
        }
        m_days << d;
    }

    QString when = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
    if (fromCache) {
        // 缓存路径：时间戳由 loadCache 注入到 _fetchedAt（秒）。用「月-日 时:分」
        // 而不是只有时分，因为隔天再看缓存会很误导
        const qint64 ts = qint64(root.value(QStringLiteral("_fetchedAt")).toDouble());
        if (ts > 0)
            when = QDateTime::fromSecsSinceEpoch(ts).toString(QStringLiteral("MM-dd HH:mm"));
        m_updatedText = QStringLiteral("离线缓存 · %1").arg(when);
    } else {
        m_updatedText = QStringLiteral("更新于 %1").arg(when);
    }

    update();
    // 角标温度变了，通知宿主刷新标题栏
    emit panelChanged();
}

// 错误只落在底部状态行，不弹窗、不清 m_descText：
// 断网时保留上次天气比给一个全空的「出错了」有用得多
void WeatherWidget::showError(const QString &text)
{
    // 两种文案刻意区分：没数据 → 引导重试；有数据 → 说明画面上的仍是旧数据
    if (m_descText.isEmpty()) {
        m_updatedText = QStringLiteral("%1 · 右键可重试").arg(text);
    } else {
        m_updatedText = QStringLiteral("%1 · 显示上次数据").arg(text);
    }
    update();
}

// 只在构造时读一次，纯粹为了「别让用户对着空白等网络」；任何一处不可信
//（文件不在、城市不符、payload 缺失）都静默放弃，等网络结果覆盖
void WeatherWidget::loadCache()
{
    QFile f(cacheFile());
    // 首次运行没有缓存是正常路径，静默返回，界面会保持「正在获取…」
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    // 缓存与当前城市不一致就整份丢弃：换城市后显示旧城市的天气比空着更糟
    if (o.value(QStringLiteral("city")).toString() != m_cityName)
        return;
    const QJsonObject payload = o.value(QStringLiteral("payload")).toObject();
    if (payload.isEmpty())
        return;
    m_lat = o.value(QStringLiteral("lat")).toDouble();
    m_lon = o.value(QStringLiteral("lon")).toDouble();
    // 缓存文件里不存 hasCoord 标志，只能靠坐标反推：(0,0) 是「没有坐标」的哨兵
    // （几内亚湾，实际城市不会精确落在这一点），任一维非 0 就认为坐标可用
    m_hasCoord = (m_lat != 0 || m_lon != 0);

    // 时间戳不在 payload 里（那是接口原始数据），临时注入一份给 applyPayload 决定文案
    QJsonObject withTime = payload;
    withTime[QStringLiteral("_fetchedAt")] = o.value(QStringLiteral("fetchedAt")).toDouble();
    applyPayload(withTime, true);
}

// 整份覆盖写，不做增量合并：payload 是接口原文，合并反而容易拼出半新半旧的 JSON。
// 写失败只损失「下次启动的秒开」，当前这次显示已经由内存数据保证了
void WeatherWidget::writeCache(const QJsonObject &payload)
{
    Paths::ensureDir(Paths::dataDir());
    QJsonObject o;
    // 城市与坐标一起存：loadCache 要靠 city 判断这份缓存是否还适用于当前城市
    o[QStringLiteral("city")] = m_cityName;
    o[QStringLiteral("lat")] = m_lat;
    o[QStringLiteral("lon")] = m_lon;
    o[QStringLiteral("fetchedAt")] = QDateTime::currentSecsSinceEpoch();
    o[QStringLiteral("payload")] = payload;

    QFile f(cacheFile());
    // 写缓存失败不影响显示，直接放弃（数据目录不可写属于环境问题，弹窗也没用）
    if (!f.open(QIODevice::WriteOnly))
        return;
    // 用 Compact 而不是 Indented：这份文件只给程序读，缩进版纯属浪费体积
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// 只存城市与坐标：天气数据本身走独立的缓存文件，没必要塞进 boxes.json 把配置撑大
void WeatherWidget::saveState(QJsonObject &o) const
{
    // 存经纬度是为了下次启动直接请求预报，省掉一次地理编码往返（也避免同名城市选错）
    // o 是宿主 widgetState 的副本，可能还带着别的键；这里只覆盖自己这三个
    o[QStringLiteral("city")] = m_cityName;
    o[QStringLiteral("lat")] = m_lat;
    o[QStringLiteral("lon")] = m_lon;
}

// 三条分支：配置里是别的城市 → 以盒子为准并带上坐标；配置没城市且构造时也没拿到 → 再读一次
// 全局设置（可能刚被设置对话框改过）；其余情况保持构造结果不动，省一次无谓的重绘
void WeatherWidget::restoreState(const QJsonObject &o)
{
    const QString city = o.value(QStringLiteral("city")).toString();
    if (!city.isEmpty() && city != m_cityName) {
        // 配置里的城市与构造时取的全局默认不同：以盒子自己的为准，坐标一并带过来
        m_lat = o.value(QStringLiteral("lat")).toDouble();
        m_lon = o.value(QStringLiteral("lon")).toDouble();
        setCity(city, m_lat != 0 || m_lon != 0);
    } else if (m_cityName.isEmpty()) {
        // 配置里没有城市，构造时拿到的全局城市也是空：再取一次全局值（可能已被设置对话框改过），
        // 让面板至少有个可编辑的起点
        setCity(AppSettings::get().weatherCity, false);
    }
    // 城市相同的情况什么都不做：构造时已按全局城市建好状态，避免无谓重绘
}

// 菜单只有两项：换城市（顺带写成全局默认并立即刷新）与手动刷新。
// 换城市用输入框而不是下拉列表：接口支持任意中英文地名，硬编码列表反而限制用户
void WeatherWidget::buildMenu(QMenu *menu)
{
    menu->addAction(QStringLiteral("更换城市…"), this, [this]
    {
        bool ok = false;
        // 用输入框而不是城市列表：接口支持中英文任意地名，硬编码列表反而限制用户
        const QString city = QInputDialog::getText(this, QStringLiteral("天气"),
                                                   QStringLiteral("城市名（中文或英文）"),
                                                   QLineEdit::Normal, m_cityName, &ok);
        if (!ok || city.trimmed().isEmpty())
            return;
        // coordinateKnown=false：新城市还没有经纬度，必须重新地理编码
        setCity(city.trimmed(), false);
        // 顺手写成全局默认，下次新建天气组件就用这个城市
        AppSettings::get().weatherCity = m_cityName;
        AppSettings::get().save();
        refresh();
    });
    menu->addAction(QStringLiteral("立即刷新"), this, &WeatherWidget::refresh);
}
