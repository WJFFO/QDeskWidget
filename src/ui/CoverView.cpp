#include "ui/CoverView.h"

#include "core/AppSettings.h"

#include <QPainter>
#include <QPainterPath>

// CoverView 实现。绘制顺序有强依赖：先整圆填实心黑，再把圆图缩进 ring 画进去，
// 反过来做的话黑边会盖住封面；旋转也只包住 drawPixmap 那一段，不能连黑圆一起转。
// 占位音符用固定 52px 设计稿坐标等比缩放，改 inner 尺寸时图形不会变形。

namespace
{
constexpr int kTickMs = 30;
constexpr qreal kDegPerTick = 1.0; // 约 11 秒一圈
}

// 构造只做「静态准备」：定时器装上但先不启动，等 setSpinning(true) 且控件可见才真正转。
CoverView::CoverView(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(m_d, m_d);

    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, [this]
    {
        m_angle += kDegPerTick;
        // 角度归一化到 [0, 360)：不取模的话数值会一直增长（连续播放几小时可达十万级），
        // 既没必要也损失精度
        if (m_angle >= 360.0)
            m_angle -= 360.0;
        update();
    });
}

// 源图变化必定重算圆图；直径不变时也要 rebuild，因为裁切依赖的是源图内容。
void CoverView::setCover(const QPixmap &pixmap)
{
    m_src = pixmap;
    rebuild();
}

// 直径没变且已有缓存时直接返回：拖窗口时 applyMetrics 会高频调用，
// 每次重做 SmoothTransformation 缩放开销很大。
void CoverView::setDiameter(int diameter)
{
    const int d = qMax(20, diameter);
    if (d == m_d && !m_disc.isNull())
        return;
    m_d = d;
    setFixedSize(d, d);
    rebuild();
    updateGeometry();
}

void CoverView::setSpinning(bool spinning)
{
    if (m_spin == spinning)
        return;
    m_spin = spinning;
    refreshTimer();
}

// 尺寸完全由 m_d 决定，不参与布局的拉伸协商（Fixed/Fixed + 固定尺寸，此处只是给 hint 兜底）。
QSize CoverView::sizeHint() const
{
    return QSize(m_d, m_d);
}

QSize CoverView::minimumSizeHint() const
{
    return QSize(20, 20);
}

// 两步：先放大/裁成正方形（KeepAspectRatioByExpanding 保证短边铺满，不会留黑边），
// 再用圆形路径裁剪。黑边不在这里画，因为黑边不参与旋转。
QPixmap CoverView::circularDisc(const QPixmap &src, int size)
{
    QPixmap scaled = src.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                Qt::SmoothTransformation);
    // 放大后可能有一边超出，从中心裁出正方形，保证封面主体在中间
    const int x = qMax(0, (scaled.width() - size) / 2);
    const int y = qMax(0, (scaled.height() - size) / 2);
    scaled = scaled.copy(x, y, size, size);

    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, size, size));
    p.setClipPath(clip);
    p.drawPixmap(0, 0, scaled);
    p.end();
    return out;
}

// 源图为空时清掉缓存而不是留旧图：换歌但拿不到封面时应该显示占位音符，
// 而不是继续放上一首的封面。
void CoverView::rebuild()
{
    if (m_src.isNull()) {
        m_disc = QPixmap();
        update();
        return;
    }
    m_disc = circularDisc(m_src, m_d);
    update();
}

// 定时器开停的唯一判定点：必须同时满足「要转」与「可见」。
// 不要在这里重置 m_angle —— 暂停再播应从原角度继续，而不是跳回 0（视觉上像突然闪一下）。
void CoverView::refreshTimer()
{
    if (m_spin && isVisible()) {
        if (!m_timer.isActive())
            m_timer.start();
    } else {
        m_timer.stop();
    }
}

// 黑环的厚度 ring 只在屏幕坐标系里出现，旋转的是 inner 区域内的封面位图，
// 所以两条绘制路径（占位/封面）都以 inner 为边界，ring 的计算只在这里做一次。
void CoverView::paintEvent(QPaintEvent *)
{
    const AppSettings &st = AppSettings::get();
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

    const QRectF box(0, 0, m_d, m_d);
    // 黑色包边厚度：按直径比例，但小尺寸别太粗、大尺寸别太夸张
    const qreal ring = qBound(2.0, m_d / 14.0, 8.0);
    const QRectF inner = box.adjusted(ring, ring, -ring, -ring);

    // 先铺一层实心黑圆：封面画在里面，露出来的一圈就是黑色外圆
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0));
    p.drawEllipse(box);

    if (m_disc.isNull()) {
        // 占位：暗色内圆 + 音符
        QPainterPath circle;
        circle.addEllipse(inner);
        // 深色/浅色主题用不同极低 alpha 的白/黑，只做出「内凹」感，不抢眼
        p.fillPath(circle, st.isDark() ? QColor(255, 255, 255, 18) : QColor(15, 18, 24, 14));

        // 音符按 52px 设计稿等比缩放：s 是缩放比，o 是内圆左上角（设计稿坐标原点）
        const QColor c = st.subTextColor();
        const double s = inner.width() / 52.0;
        const QPointF o(inner.left(), inner.top());
        p.setPen(QPen(c, qMax(1.4, 2.0 * s), Qt::SolidLine, Qt::RoundCap));
        p.drawLine(o + QPointF(22 * s, 34 * s), o + QPointF(22 * s, 16 * s)); // 左符干
        p.drawLine(o + QPointF(34 * s, 31 * s), o + QPointF(34 * s, 13 * s)); // 右符干
        p.drawLine(o + QPointF(22 * s, 16 * s), o + QPointF(34 * s, 13 * s)); // 符梁
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawEllipse(o + QPointF(19 * s, 34 * s), 4 * s, 3 * s); // 左符头（椭圆更像手写）
        p.drawEllipse(o + QPointF(31 * s, 31 * s), 4 * s, 3 * s); // 右符头
    } else {
        // 封面只画在内圆里，旋转的也是它（黑环不动，转起来不会整块晃）
        p.save();
        p.translate(box.center());
        p.rotate(m_angle);
        p.drawPixmap(QRectF(-inner.width() / 2.0, -inner.height() / 2.0,
                            inner.width(), inner.height()),
                     m_disc, QRectF(m_disc.rect()));
        p.restore();
    }
}

// 显示时才可能开始转：构造期 setSpinning(true) 往往控件还没显示。
void CoverView::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refreshTimer();
}

// 隐藏时直接停表（不走 refreshTimer），避免多一次 isVisible() 判定。
void CoverView::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_timer.stop();
}
