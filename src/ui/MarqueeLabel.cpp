#include "ui/MarqueeLabel.h"

#include <QFontMetrics>
#include <QPainter>
#include <QResizeEvent>

// MarqueeLabel 实现：定时器逐帧累加像素偏移，绘制时同一段文字画两遍（第二遍错开
// 一个「文字宽 + 间隔」），形成首尾相接的无缝滚动；两遍之间画一个小圆点做视觉分隔。
// 是否滚动只看 textWidth() > width()，所以父布局必须给它真实的可压缩宽度，
// 一旦父级用 sizeHint 把它撑开就永远不会滚。

namespace
{
// 30ms ≈ 33 帧/s：再快肉眼只是抖动，再慢明显掉帧。速度按帧算，不做时间补偿。
constexpr int kTickMs = 30;      // 约 33 帧
constexpr qreal kSpeed = 1.4;    // 每帧像素（约 46px/s）
constexpr int kHoldFrames = 40;  // 开头停顿约 1.2s
}

// 构造只装定时器，不启动：是否要滚取决于文本宽度与控件宽度的比较，等 resize/show 时再定。
MarqueeLabel::MarqueeLabel(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMinimumWidth(0);

    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, [this]
    {
        // 开头停顿：只消耗帧数，不推进 offset，让用户先看清文本起始部分
        if (m_hold > 0) {
            --m_hold;
            return;
        }
        m_offset += kSpeed;
        // 走满一个「文本 + 间隔」周期就回到起点，并重新停顿
        const qreal cycle = textWidth() + gap();
        if (m_offset >= cycle) {
            m_offset = 0;
            m_hold = kHoldFrames;
        }
        update();
    });
}

// 文本没实质变化时直接返回：否则同一段文字被重复 set 会把跑马灯打回开头（restart 复位偏移）。
void MarqueeLabel::setText(const QString &text)
{
    if (m_text == text)
        return;
    m_text = text;
    setToolTip(text);
    restart();
    updateGeometry();
    update();
}

// 只改颜色，不需要复位滚动位置，避免换主题时文字「跳回开头」。
void MarqueeLabel::setTextColor(const QColor &color)
{
    m_color = color;
    update();
}

// 开关切换后必须 restart：从滚动切到静态时要立刻停表并清掉偏移。
void MarqueeLabel::setScrollingEnabled(bool enabled)
{
    if (m_scrolling == enabled)
        return;
    m_scrolling = enabled;
    restart();
    update();
}

// 用不加省略的完整宽度，父布局才能算出「需不需要滚」的临界点。
QSize MarqueeLabel::sizeHint() const
{
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(m_text), fm.height());
}

// 高度用字体行高而不是 0：被压窄时可以无限缩，但纵向仍要占住一行，否则会跟相邻控件重叠。
QSize MarqueeLabel::minimumSizeHint() const
{
    return QSize(0, QFontMetrics(font()).height());
}

// 不省略的完整宽度：溢出判定、滚动周期、sizeHint 都以它为准，三者必须一致。
int MarqueeLabel::textWidth() const
{
    return QFontMetrics(font()).horizontalAdvance(m_text);
}

// 间隔至少 24px，大字号时按 2 个字宽留白，保证两遍文本不会挤在一起。
int MarqueeLabel::gap() const
{
    return qMax(24, font().pixelSize() * 2);
}

void MarqueeLabel::restart()
{
    m_offset = 0;
    m_hold = kHoldFrames;
    refreshScroll();
    update();
}

// 唯一决定定时器开停的地方：不满足条件时必须把 offset 归零，
// 否则从「溢出」变「放得下」后文字会停在半截位置。
void MarqueeLabel::refreshScroll()
{
    const bool overflow = textWidth() > width();
    if (overflow && m_scrolling && isVisible()) {
        if (!m_timer.isActive())
            m_timer.start();
    } else {
        m_timer.stop();
        m_offset = 0;
    }
}

void MarqueeLabel::paintEvent(QPaintEvent *)
{
    if (m_text.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHints(QPainter::TextAntialiasing | QPainter::Antialiasing);
    p.setFont(font());
    p.setPen(m_color);
    p.setClipRect(rect()); // 静态分支也可能因字号大于控件高度而溢出

    const QFontMetrics fm(font());
    // 手动算基线：AlignVCenter 用的是控件矩形，字号超出高度时会上下不对称地裁
    const int y = (height() + fm.ascent() - fm.descent()) / 2;

    const int tw = fm.horizontalAdvance(m_text);
    if (tw <= width() || !m_scrolling) {
        // 放得下就整段画；放不下但禁止滚动时退化成右侧省略号
        const QString shown = (tw <= width())
                                  ? m_text
                                  : fm.elidedText(m_text, Qt::ElideRight, width());
        p.drawText(QRect(0, 0, width(), height()), Qt::AlignLeft | Qt::AlignVCenter, shown);
        return;
    }

    // 跑马灯：整体左移；两遍之间留一个圆点，接口处不至于糊成一句
    const qreal gapW = gap();
    const qreal x = -m_offset;
    p.drawText(QPointF(x, y), m_text);
    p.drawText(QPointF(x + tw + gapW, y), m_text);

    // 圆点画在两遍之间的空隙中央
    const qreal dotX = x + tw + gapW / 2.0;
    // 只在这段空隙进入可视区（含 10px 余量）时才画，省掉无效绘制
    if (dotX > -10 && dotX < width() + 10) {
        QColor dotColor = m_color;
        dotColor.setAlpha(120);
        p.setPen(Qt::NoPen);
        p.setBrush(dotColor);
        p.drawEllipse(QPointF(dotX, height() / 2.0), 1.8, 1.8);
    }
}

// 宽度变化会改变「是否溢出」的判定，必须重新 refreshScroll，否则缩窄后不会开始滚。
void MarqueeLabel::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    refreshScroll();
    update();
}

// 隐藏时定时器已停，重新显示要复位并重新判定（隐藏期间布局可能改过宽度）。
void MarqueeLabel::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    restart();
}

// 不可见时继续跑纯属浪费 CPU：停表即可，offset 由下次 showEvent 复位。
void MarqueeLabel::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_timer.stop();
}
