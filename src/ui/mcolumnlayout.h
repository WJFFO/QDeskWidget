/**
 * @file mcolumnlayout.h
 * @brief MColumnLayout：纵向容器，用 QVBoxLayout 排子控件，同时自己画圆角背景 / 边框 /
 *        悬停与按下态。
 *
 * 与 MRowLayout 的关键差异（别把两边当对称的看）：
 *  - 本类不维护尾部弹簧：ColumnAddWidget() 不会补 stretch，也不会清旧弹簧，
 *    需要占位/顶底请显式调 ColumnAddStretch()；
 *  - ColumnCount() 返回的是布局项总数（含弹簧），与 MRowLayout::RowCount() 只数控件不同；
 *  - 没有 Pin 系列接口。
 * 常用姿势：new MColumnLayout(host) 后立刻调一次 ColumnFillHost(host)，让本列铺满宿主，
 * 之后所有子控件都交给它纵向排布，业务代码不再需要裸 QVBoxLayout。
 */
#ifndef MCOLUMNLAYOUT_H
#define MCOLUMNLAYOUT_H

#include <QWidget>
#include <QVBoxLayout>
#include <QColor>
#include <QPainter>
#include <QMouseEvent>

/**
 * 纵向排列容器：子控件由内部 QVBoxLayout 排布，背景/边框/圆角/悬停态由本类自绘。
 *
 * 与 MRowLayout 一样是 QWidget（不是 QLayout），因此能设边距、能接收鼠标事件发 clicked()。
 */
class MColumnLayout : public QWidget
{
    Q_OBJECT

public:
    /** @param parent 父控件；内部会立刻建好 QVBoxLayout 并把边距/间距置 0。 */
    explicit MColumnLayout(QWidget *parent = nullptr);
    ~MColumnLayout() override;

    // ================= 布局管理 =================
    /**
     * @brief 追加控件到列尾。
     * @param w 待添加控件；为 nullptr 时直接返回。
     * @param stretch 纵向拉伸系数，0 = 不拉伸（保持 sizeHint）；要让某行占满剩余高度就传 1。
     * @param alignment 列内对齐方式；默认全填充。
     *
     * 注意本方法不会追加尾部弹簧，控件之外的剩余空间保持空白（不像 MRowLayout 会补 stretch）。
     */
    void ColumnAddWidget(QWidget *w, int stretch = 0, Qt::Alignment alignment = Qt::Alignment());
    /**
     * @brief 在指定位置插入控件。
     * @param index 布局索引，0 = 最上；越界由 QVBoxLayout 自行夹取。
     * @param w 待插入控件；为 nullptr 时直接返回。
     * @param stretch 纵向拉伸系数。
     * @param alignment 列内对齐方式。
     */
    void ColumnInsertWidget(int index, QWidget *w, int stretch = 0, Qt::Alignment alignment = Qt::Alignment());
    /** @brief 把控件从布局中摘除（控件本身不销毁、父对象不变）。@param w 为 nullptr 时空操作。 */
    void ColumnRemoveWidget(QWidget *w);
    /** @brief 清空布局项，控件 setParent(nullptr) 后被移出（生命周期交给调用方）。 */
    void ColumnClear();
    /**
     * @brief 统计布局项数量。
     * @return 布局项总数，**包含**弹簧占位（与 MRowLayout::RowCount() 的语义不同）。
     */
    int  ColumnCount() const;

    /**
     * @brief 在列尾添加弹性空间。
     * @param stretch 弹簧的拉伸系数，0 表示固定 0 高（等于没加）。
     */
    void ColumnAddStretch(int stretch = 0);

    /**
     * @brief 把本列作为 host 的根容器铺满整个 host（零边距、零间距）。
     * @param host 宿主控件；为空或等于 this 时直接返回。
     *
     * 用法：new MColumnLayout(host) 后调用一次，host 的界面就完全由本列组织，
     * 调用方不需要再自己写 QVBoxLayout。host 没有布局时先建一个零边距的
     * QVBoxLayout 作包装；已挂过则幂等返回。副作用：会把 this 的 parent 改成 host。
     */
    void ColumnFillHost(QWidget *host);

    // ================= 外观控制 =================
    /** @brief 常态背景色；alpha 为 0 时不填背景（保持透明）。@param color 任意 QColor，无效值等于不画。 */
    void SetBackgroundColor(const QColor &color);
    /** @return 常态背景色；未设置时为无效 QColor。 */
    QColor BackgroundColor() const;

    /** @brief 悬停背景色，仅在 SetHoverEnabled(true) 时生效。@param color 无效值则回落到常态背景。 */
    void SetHoverColor(const QColor &color);
    /** @return 悬停背景色。 */
    QColor HoverColor() const;

    /** @brief 按下背景色，优先级高于悬停色。@param color 无效值则回落到悬停/常态背景。 */
    void SetPressedColor(const QColor &color);
    /** @return 按下背景色。 */
    QColor PressedColor() const;

    /** @brief 边框颜色；只有 SetBorderWidth() > 0 时才会绘制。 */
    void SetBorderColor(const QColor &color);
    /** @brief 边框线宽（px）。@param width 0 表示不画边框；绘制时会内缩半线宽防止被裁。 */
    void SetBorderWidth(int width);
    /** @brief 背景与边框的圆角半径（px）。@param radius 0 = 直角。 */
    void SetRadius(int radius);
    /** @return 当前圆角半径（px）。 */
    int  Radius() const;

    // ================= 间距 / 边距 =================
    /** @brief 子控件之间的纵向间距（px）。@param spacing 0 = 紧贴。 */
    void SetSpacing(int spacing);
    /** @return 当前子控件间距（px）。 */
    int  Spacing() const;
    /** @brief 四边分别设置内边距（px）。 */
    void SetMargin(int left, int top, int right, int bottom);
    /** @brief 四边统一内边距（px）。@param margin 同一个值应用给上下左右。 */
    void SetMargin(int margin);

    // ================= 交互 =================
    /**
     * @brief 是否启用悬停/按下高亮效果（默认关闭）。
     * @param enabled true = 响应 enter/leave/press/release 并重绘；false = 忽略这些事件并复位状态
     *        （clicked()/pressed()/released() 也随之不再发出）。
     */
    void SetHoverEnabled(bool enabled);
    /** @return 悬停高亮是否开启。 */
    bool IsHoverEnabled() const;

protected:
    /** @brief 自绘圆角背景（按下 > 悬停 > 常态）+ 可选边框；画布内缩半线宽。 */
    void paintEvent(QPaintEvent *event) override;
    /** @brief 进入时置 m_isHovered（仅在高亮开启时）。 */
    void enterEvent(QEnterEvent *event) override;
    /** @brief 离开时同时复位 hover 与 pressed，防止「按下后拖出」留下按下态。 */
    void leaveEvent(QEvent *event) override;
    /** @brief 左键按下置 m_isPressed 并 emit pressed()；仅在高亮开启时。 */
    void mousePressEvent(QMouseEvent *event) override;
    /** @brief 左键释放：仅在按下过（wasPressed）时 emit clicked()，并总是 emit released()。 */
    void mouseReleaseEvent(QMouseEvent *event) override;

signals:
    void clicked();          // 列被点击（释放时触发）
    void pressed();          // 列被按下
    void released();         // 列被释放

private:
    QVBoxLayout *m_column;           // 内部排版用布局，父对象为 this；构造时创建，之后不替换

    // 背景颜色
    QColor m_backgroundColor;   // 默认背景；无效 QColor = 不填背景
    QColor m_hoverColor;        // 悬停背景；无效值则沿用默认背景
    QColor m_pressedColor;      // 按下背景；无效值则回落到悬停/默认背景
    QColor m_borderColor;       // 边框颜色；仅在 m_borderWidth > 0 时参与绘制
    int    m_borderWidth = 0;   // 边框线宽（px），0 = 无边框
    int    m_radius = 0;        // 圆角半径（px），0 = 直角

    // 当前状态
    bool m_hoverEnabled = false; // 是否启用悬停/按下高亮，默认关闭（纯布局用途时不响应鼠标）
    bool m_isHovered = false;    // 鼠标当前是否在本列内
    bool m_isPressed = false;    // 左键是否在本列按下且尚未释放；clicked() 的判定依据
};

#endif // MCOLUMNLAYOUT_H
