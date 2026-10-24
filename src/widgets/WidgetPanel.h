/**
 * @file WidgetPanel.h
 * 小组件面板的抽象基类：只负责「内容区」，不画背景、不画标题栏。
 *
 * 关键约定：
 *  - 玻璃背景 / 圆角 / 标题栏 / 折叠都由 FileBoxWindow 统一处理，面板一旦自绘底色就会
 *    盖掉外层玻璃，所以基类把 paintEvent 写成空实现（透出父窗口）；
 *  - 子类必须实现 panelTitle()，其余都是可选钩子；不实现就等于「不支持该能力」；
 *  - 面板要按可用尺寸自排版，窗口直接拿 minimumSizeHint()/maximumSizeHint() 去
 *    setMinimumSize/setMaximumSize，返回不合理的值会得到不可用的窗口。
 */
#pragma once

#include <QJsonObject>
#include <QSize>
#include <QWidget>

class QMenu;

/**
 * 小组件面板的基类。
 *
 * 面板只负责「内容区」，窗口外框（玻璃背景、圆角、标题栏、折叠）由整理盒窗口统一处理，
 * 所以面板本身不画背景、也不需要知道材质与主题细节。
 *
 * 尺寸自适应约定：面板必须自己根据可用宽高切换版式（而不是把控件缩小硬塞），
 * 保证任何尺寸下文字与图标都不被截断或互相遮挡。窗口会把 minimumSizeHint() 当作
 * 展开态的最小尺寸，因此这个值应当是「最紧凑版式仍能完整显示」的下限。
 *
 * 宿主会在这些时机回调面板，子类只需覆写自己关心的钩子：
 *  - 尺寸变化 / 主题切换 / 数据到达 → paintEvent()（面板自己决定画什么）；
 *  - 切换「隐藏标题栏」、或应用外观配置时 → setHeaderHidden()；
 *  - 右键菜单 → buildMenu()，往宿主给的菜单里 addAction，不要自己 popup；
 *  - 创建面板后立即 → restoreState()；窗口关闭 / 切成员时 → saveState()。
 *
 * 线程约定：面板只在 GUI 线程创建、绘制与回调，基类不做任何线程隔离；refresh() 里的
 * 耗时工作必须转成异步（网络 / 读盘回调），结果回到主线程后再 update() 或发 panelChanged()。
 * 面板自身也由宿主布局托管（create() 后被加进内容区），所以不要自己 show()/setFixedSize()。
 */
class WidgetPanel : public QWidget
{
    Q_OBJECT

public:
    /**
     * 构造面板：只清掉系统背景，让外层玻璃透上来；子类构造末尾应自行准备数据与定时器。
     * @param parent 父控件（实际就是 FileBoxWindow 的内容区容器），可为 nullptr
     */
    explicit WidgetPanel(QWidget *parent = nullptr);
    /**
     * 虚析构：宿主一律按 WidgetPanel* 持有面板，靠它保证子类析构正常执行。
     * @note 面板随父窗口一起销毁（Qt 父子关系），宿主不需要也不能手动 delete
     */
    ~WidgetPanel() override = default;

    /**
     * 标题栏显示的文字（纯虚，子类必须实现）。
     * @return 组件名，例如「天气 · 杭州」；返回空串时窗口用通用标题兜底
     */
    virtual QString panelTitle() const = 0;

    /**
     * 标题栏右侧的数字角标。
     * @return 要显示的数量；返回负数（约定为 -1）表示不显示，此时才会看 badgeText()
     *
     * @note 当前窗口（FileBoxWindow::updateCount/updateHeader）其实只读 badgeText()，
     *       本函数是为「只报数量的简单实现」保留的旧入口；新面板请直接覆写 badgeText()，
     *       否则角标不会出现在标题栏上。
     */
    virtual int badgeCount() const { return -1; }

    /**
     * 标题栏右侧的文字角标，优先于 badgeCount()，用于带单位的展示（如「20°」「3 首」）。
     * @return 要显示的文字；空串表示不显示。返回值原样进标题栏，不要再套引号或空格
     */
    virtual QString badgeText() const { return QString(); }

    /**
     * 定时刷新 / 菜单「立即刷新」的入口。
     * 会被自动刷新定时器周期性调用，必须轻量、可重入：重活（网络、磁盘）应走异步，
     * 且要自行防重入，避免上一次还没回来又叠一次请求。
     *
     * @note 宿主本身没有全局定时器，周期刷新是靠组件自己建 QTimer 驱动的
     *       （WeatherWidget 就是这么做的，间隔取 AppSettings::widgetRefreshMin）；
     *       宿主只在建窗 / 应用配置 / 菜单「刷新」时显式调一次。
     */
    virtual void refresh() {}

    /**
     * 把组件状态写进配置，宿主保存到 BoxConfig.widgetState（落在 boxes.json）。
     * @param o 输出参数：宿主已备好的对象，直接往里塞键值即可；不要 clear，那里可能已有别的字段
     *
     * @note 传进来的是 widgetState 的副本（宿主 persist() 里拷的），里面可能残留旧版本写过、
     *       现在已经不认识的键；只增改自己认识的键，读的一方（restoreState）必须容忍多余字段。
     */
    virtual void saveState(QJsonObject &o) const { Q_UNUSED(o) }

    /**
     * 从配置恢复状态。
     * @param o 上次 saveState() 写出的内容；首次创建或旧配置时可能是空对象，必须容忍缺字段
     *
     * 时序：宿主在 new 出面板后立刻调用，此时窗口尺寸往往还没确定，需要重绘的
     * 逻辑不要依赖 width()/height()。
     */
    virtual void restoreState(const QJsonObject &o) { Q_UNUSED(o) }

    /**
     * 往窗口右键菜单里追加组件自己的条目。
     * @param menu 宿主提供的菜单，非空；只做 addAction，不要 popup/delete 它
     */
    virtual void buildMenu(QMenu *menu) { Q_UNUSED(menu) }

    /**
     * 最紧凑版式所需的尺寸；窗口据此限制最小尺寸。
     * @return 内容区最小宽高（像素），默认 150x80；窗口还会另外加上标题栏高度，
     *         所以这里不要自己把标题栏算进去
     *
     * @note 窗口只对宽度兜底 140px（防止面板自报过窄不可用），高度是原样相加、没有兜底，
     *       所以 height 必须自己给足；隐藏标题栏时窗口会跳过标题栏高度那一项。
     */
    QSize minimumSizeHint() const override { return QSize(150, 80); }

    /**
     * 最大尺寸（内容区大小）；返回空 QSize 表示不限制。
     * @return 建议上限；QSize() 或任一维 <= 0 都按「不限制」处理（窗口会放开 QWIDGETSIZE_MAX）
     *
     * @note 窗口最终会 setMaximumSize()，所以这个值同时是「用户能拖到多大」的天花板；
     *       只在内容确实画不满时给上限（如音乐组件 880x680），否则留空更灵活。
     */
    virtual QSize maximumSizeHint() const { return QSize(); }

    /**
     * 标题栏被隐藏时通知面板（可以据此调整边距、或把原本靠标题栏传达的信息画到内容区）。
     * @param hidden true = 标题栏已隐藏、内容铺满整窗；false = 标题栏可见
     */
    virtual void setHeaderHidden(bool hidden) { Q_UNUSED(hidden) }

    /**
     * 主题 / 材质变了：重新读一遍 AppSettings 的配色令牌。
     *
     * 这个钩子是给「把颜色缓存在控件里」的面板用的：用 QSS（`UiStyle::panelCss()`）、
     * `setTextColor()`、染色图标这些做法，颜色在设置那一刻就固定了，不重设就一直是旧主题的颜色。
     * 完全自绘（paintEvent 里现读 `AppSettings`）的面板不需要覆写，基类默认什么都不做。
     *
     * 调用时机：`FileBoxWindow::applyAppearance()` —— 也就是设置面板发 appearanceChanged、
     * 或换了成员之后。**不要**在这里改几何，尺寸那一路由窗口负责。
     */
    virtual void applyTheme() {}

signals:
    /**
     * 标题或计数变化，窗口据此刷新标题栏；触发前要保证 panelTitle()/badgeText() 已是新值。
     *
     * @note 基类不会自动发射：子类改完会体现在标题/角标上的状态后要自己 emit 一次；
     *       窗口收到后会 updateHeader() + updateCount() + persist()（配置落盘走 App 的
     *       合并保存），所以高频状态变化（如播放进度）不要每次 emit。
     */
    void panelChanged();

protected:
    /**
     * 空实现：什么都不画，让外层的玻璃背景透上来。
     * @param e 重绘区域，基类忽略；子类做局部重绘优化时才需要看它
     */
    void paintEvent(QPaintEvent *e) override;
};
