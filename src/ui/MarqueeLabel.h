#pragma once

#include <QColor>
#include <QTimer>
#include <QWidget>

// 单行自绘文字标签：文字宽度超出控件宽度时用定时器做横向跑马灯，否则静态绘制。
// 定长定时器（30ms）逐帧累加像素偏移，所以实际速度取决于帧间隔是否稳定，不做时间补偿；
// 控件本身不开鼠标事件（点击穿透给父级），靠 tooltip 兜底让用户看到完整文本。
// 绘制时必须 setClipRect：字号可能大于控件高度，不裁会画到相邻控件上。

/**
 * 单行文字标签：放不下时自动横向滚动（跑马灯），否则静态显示。
 *
 * 典型用途是歌名 / 歌手 —— 名字一长就会被裁掉，这里让它慢慢从右往左跑，
 * 起点和终点各停一下，读完再从头来。
 */
class MarqueeLabel : public QWidget
{
    Q_OBJECT

public:
    /**
     * 构造：固定宽度策略（Ignored）+ 30ms 步进定时器，定时器按需启停。
     * @param parent 父控件；本控件对鼠标透明，tooltip 是唯一能看到完整文本的途径
     */
    explicit MarqueeLabel(QWidget *parent = nullptr);

    /**
     * 设置显示文字。
     * @param text 单行文本；内部不折行，换行符会被原样当成普通字符（不建议传）
     * 副作用：文本真正变化时复位滚动位置、回填 tooltip、触发 updateGeometry。
     */
    void setText(const QString &text);
    /** @return 当前完整文本（不受省略号/滚动位置影响） */
    QString text() const { return m_text; }

    /**
     * 设置文字颜色。
     * @param color 绘制色；无默认值，构造后若没调用会使用 QColor 默认构造的无效色（Qt 按黑色画）
     */
    void setTextColor(const QColor &color);
    /**
     * 总开关：关掉后超长文字只做省略号（用于极小的组件）。
     * @param enabled false = 永不滚动，立即停表并把偏移归零；true = 恢复按需滚动
     */
    void setScrollingEnabled(bool enabled);

    /** @return 整段文字宽度 × 字体行高，不省略；父布局据此判断是否真的放得下 */
    QSize sizeHint() const override;
    /** @return 宽 0（允许压到任意窄）× 字体行高（纵向仍占一行，否则会与邻居重叠） */
    QSize minimumSizeHint() const override;

protected:
    /**
     * 两个分支：放得下（或禁止滚动）就静态画一遍，否则画两遍做无缝跑马灯。
     * @param e 重绘区域，实现里忽略（有无裁剪区都必须整块重画，滚动位置每帧都在变）
     */
    void paintEvent(QPaintEvent *e) override;
    /**
     * 宽度变化会改变「是否溢出」的判定，必须重新决定定时器开停。
     * @param e resize 事件，仅用于转发给基类
     */
    void resizeEvent(QResizeEvent *e) override;
    /** @param e show 事件；重新显示时复位偏移（隐藏期间布局可能改过宽度） */
    void showEvent(QShowEvent *e) override;
    /** @param e hide 事件；只停表，偏移留给下次 showEvent 的 restart 复位 */
    void hideEvent(QHideEvent *e) override;

private:
    /** 复位偏移并重新判断是否需要滚动（换文本、重新显示时调用） */
    void restart();
    /** 按「是否溢出 + 开关 + 是否可见」决定启动还是停止定时器；停表时把偏移归零 */
    void refreshScroll();
    /** 整段文字的像素宽度（当前 font 下，不含省略） */
    int textWidth() const;
    /** 两遍滚动文字之间的间隔像素，同时用于放置分隔圆点 */
    int gap() const;

    QTimer m_timer;          // 30ms 步进；仅在「溢出 + 允许滚动 + 可见」时运行
    QString m_text;          // 完整文本；绘制时按需要省略或滚动，自身永不截断
    QColor m_color;          // 文字色；未调用 setTextColor 时是无效色，Qt 会按黑色画
    qreal m_offset = 0;      // 已滚动的水平像素，取值 [0, textWidth()+gap())
    int m_hold = 0;          // 停在开头的帧数
    bool m_scrolling = true; // 总开关：false 时永不滚动，超长直接省略号
};
