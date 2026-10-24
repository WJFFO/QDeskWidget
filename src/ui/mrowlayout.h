/**
 * @file mrowlayout.h
 * @brief MRowLayout：横向容器，用 QHBoxLayout 排子控件，同时自己画圆角背景 / 边框 /
 *        悬停与按下态（相当于「一行 + 一个可点击面」）。
 *
 * 与裸 QHBoxLayout 的关键差异（改代码前务必读）：
 *  - RowAddWidget() 每次都在行尾补一个 stretch(1)，所以多个控件会自然靠左挤在一起；
 *  - 想让某个控件占满整行 → 追加完所有控件后调 RowClearSpacers() 清掉尾部弹簧；
 *  - 想靠右对齐 → 用「某个控件 stretch=1 + 其余控件 + RowClearSpacers()」，
 *    不要用 RowPinToRight()：它补的弹簧会和拉伸控件按 1:1 抢空间，右对齐会跑偏；
 *  - 根容器用 ColumnFillHost()（纵向）/ RowFillHost() 铺满宿主，别在宿主上再套一层裸布局。
 */
#ifndef MROWLAYOUT_H
#define MROWLAYOUT_H

#include <QWidget>
#include <QHBoxLayout>
#include <QColor>
#include <QPainter>
#include <QMouseEvent>

/**
 * 横向排列容器：子控件由内部 QHBoxLayout 排布，背景/边框/圆角/悬停态由本类自绘。
 *
 * 注意它是 QWidget 而不是 QLayout 的子类——对外表现为「一行」这个控件，
 * 因此可以设固定高度、参与父级布局，也能接收鼠标事件发出 clicked()。
 */
class MRowLayout : public QWidget
{
    Q_OBJECT

public:
    /** @param parent 父控件；内部会立刻建好 QHBoxLayout 并把边距/间距置 0。 */
    explicit MRowLayout(QWidget *parent = nullptr);
    ~MRowLayout() override;

    // ================= 布局管理 =================
    /**
     * @brief 追加控件到行尾，并自动在行尾补一个 stretch(1)。
     * @param w 待添加控件；为 nullptr 时直接返回（不做任何事）。
     * @param stretch 该控件在行内的拉伸系数，0 = 不拉伸（保持 sizeHint）。
     * @param alignment 行内的对齐方式，如 Qt::AlignVCenter；默认全填充。
     *
     * 尾部弹簧保证「多个控件靠左紧挨」，代价是控件不会自动铺满整行——
     * 需要铺满就追加完后调 RowClearSpacers()。
     */
    void RowAddWidget(QWidget *w, int stretch = 0, Qt::Alignment alignment = Qt::Alignment());
    /**
     * @brief 在指定位置插入控件（不补尾部弹簧）。
     * @param index 布局索引，0 = 最左；越界由 QHBoxLayout 自行夹取。
     * @param w 待插入控件；为 nullptr 时直接返回。
     * @param stretch 拉伸系数，0 = 不拉伸。
     * @param alignment 行内对齐方式。
     */
    void RowInsertWidget(int index, QWidget *w, int stretch = 0, Qt::Alignment alignment = Qt::Alignment());
    /** @brief 把控件从布局中摘除（控件本身不销毁、父对象不变）。@param w 为 nullptr 时空操作。 */
    void RowRemoveWidget(QWidget *w);
    /** @brief 清空布局项，所有控件 setParent(nullptr) 后被移出（生命周期交给调用方）。 */
    void RowClear();
    /**
     * @brief 只删布局项里的弹簧，不动控件。
     *
     * 典型用途：拖动换位重排前清一次，否则会残留旧的 stretch，位置就串了；
     * 另一用途是追加完控件后清一次，让控件按 stretch 瓜分整行。
     */
    void RowClearSpacers();
    /**
     * @brief 统计行内真实控件数量。
     * @return 控件个数；行尾 stretch 等弹簧项不计入。
     */
    int  RowCount() const;

    /**
     * @brief 在行尾添加弹性空间。
     * @param stretch 弹簧的拉伸系数，0 表示固定为 0 宽（等于没加）。
     *
     * 注意 RowAddWidget() 已经会自动补 1 个 stretch(1)，连续调用会叠加。
     */
    void RowAddStretch(int stretch = 0);

    /**
     * @brief 把 w 钉在行尾（右侧）：先摘掉行内已有的 stretch，把 w 从布局里取出，
     *        再插一段弹簧后重新追加 w，末尾不再补 stretch。
     * @param w 目标控件；已在布局中会先被取出再重排，不在布局中则直接追加。
     * @param alignment 行内对齐方式，通常 Qt::AlignVCenter。
     *
     * 任务栏的回收站就是这么贴到右边的。
     * 注意：如果行内还有 stretch=1 的控件，它会和新加的弹簧 1:1 平分空白，
     * w 只能停在中间偏右——这种场景应改用「拉伸控件 + RowClearSpacers()」。
     */
    void RowPinToRight(QWidget *w, Qt::Alignment alignment = Qt::Alignment());

    /**
     * @brief 把本行作为 host 的根容器铺满整个 host（零边距、零间距）。
     * @param host 宿主控件；为空或等于 this 时直接返回。
     *
     * 用法：new MRowLayout(host) 后调用一次，host 的界面就完全由本行组织，
     * 调用方不需要再自己写 QHBoxLayout。host 没有布局时会先建一个零边距的
     * QHBoxLayout 作包装；已挂过则幂等返回。副作用：会把 this 的 parent 改成 host。
     */
    void RowFillHost(QWidget *host);

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
    /** @brief 子控件之间的间距（px）。@param spacing 0 = 紧贴。 */
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
     * @param enabled true = 响应 enter/leave/press/release 并重绘；false = 忽略这些事件并复位状态。
     *
     * 关闭时 clicked()/pressed()/released() 也不会再发出（事件处理里有一并判断）。
     */
    void SetHoverEnabled(bool enabled);
    /** @return 悬停高亮是否开启。 */
    bool IsHoverEnabled() const;

protected:
    /** @brief 自绘圆角背景（按下 > 悬停 > 常态）+ 可选边框。 */
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
    void clicked();          // 行被点击（释放时触发）
    void pressed();          // 行被按下
    void released();         // 行被释放

private:
    /** @brief 仅声明、无实现也无调用点（历史遗留）；背景色选择逻辑实际写在 paintEvent() 里。 */
    void updateBackground();
    // 行尾始终保留一个 stretch：否则 QBoxLayout 会把多余空间平均分给每个
    // 条目，三个图标会被摊到整条任务栏上，而不是靠左排。
    /**
     * @brief 从行尾连续摘除弹簧项，直到最后一个不是弹簧为止。
     *
     * 只处理「尾部连续」的弹簧：中间位置的弹簧（例如 RowPinToRight 加的）
     * 不会被这里删掉，需要全清请用 RowClearSpacers()。
     */
    void removeTrailingStretch();

private:
    QHBoxLayout *m_row;              // 内部排版用布局，父对象为 this；构造时创建，之后不替换

    // 背景颜色
    QColor m_backgroundColor;   // 默认背景；无效 QColor = 不填背景
    QColor m_hoverColor;        // 悬停背景；无效值则沿用默认背景
    QColor m_pressedColor;      // 按下背景；无效值则回落到悬停/默认背景
    QColor m_borderColor;       // 边框颜色；仅在 m_borderWidth > 0 时参与绘制
    int    m_borderWidth = 0;   // 边框线宽（px），0 = 无边框
    int    m_radius = 0;        // 圆角半径（px），0 = 直角

    // 当前状态
    bool m_hoverEnabled = false; // 是否启用悬停/按下高亮，默认关闭（纯布局用途时不响应鼠标）
    bool m_isHovered = false;    // 鼠标当前是否在本行内
    bool m_isPressed = false;    // 左键是否在本行按下且尚未释放；clicked() 的判定依据
};

#endif // MROWLAYOUT_H
