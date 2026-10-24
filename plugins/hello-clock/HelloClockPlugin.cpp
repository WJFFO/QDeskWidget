// 示例组件插件：一个时钟。
//
// 要点：
//  1. 继承 QObject 和 IWidgetPlugin，用 Q_PLUGIN_METADATA / Q_INTERFACES 声明；
//  2. createWidget() 返回任意 QWidget，宿主会把它铺满内容区；
//  3. 组件自己管自己的刷新定时器，不需要宿主帮忙；
//  4. 想记住状态就实现 saveWidgetState / restoreWidgetState（会存进 boxes.json）。
//  5. 被 qobject_cast 的控件必须带 Q_OBJECT，否则转换一律返回 nullptr（本文件的
//     refreshWidget 等就依赖这个转换，删掉 Q_OBJECT 会「刷新点了没反应」）；
//  6. .cpp 里定义的 QObject 子类没有单独的 moc 输入，所以文件末尾要 #include "HelloClockPlugin.moc"。
//
// 本文件可以直接当模板抄：改 pluginId / displayName、把 ClockWidget 换成自己的控件即可。
// 刻意不覆写 badgeCount()（默认 -1 = 不显示角标）和 minimumSizeHint()（宿主退回 170x110），
// 因为这两个钩子对时钟没有意义；你的组件若需要，按 IWidgetPlugin.h 里的签名补上即可。
//
// 同目录的 metadata.json 是给 Qt 读的（构建时由 moc 嵌进 DLL），改名/挪位置会让
// Q_PLUGIN_METADATA 编译失败；里面的 name/version/author 只是元信息，宿主不读。

#include "widgets/IWidgetPlugin.h"

#include <QDateTime>
#include <QFont>
#include <QLabel>
#include <QTimer>

/**
 * 时钟控件本体：一个居中的两行 QLabel，每秒刷新一次。
 *
 * 刻意用 QLabel + 手动 setGeometry 而不是布局：宿主拿到控件后会把它铺满内容区，
 * 内部再套一层布局反而多一层约束，也不方便演示「控件自己缩放」。
 */
class ClockWidget : public QWidget
{
    Q_OBJECT

public:
    /**
     * 构造：建标签、设字体，并启动 1 秒定时器（立即先 tick 一次，避免第一秒空白）。
     * @param parent 宿主传进来的父控件，交给 QWidget
     */
    explicit ClockWidget(QWidget *parent)
        : QWidget(parent)
    {
        // 不画底色，让宿主的玻璃背景透出来；否则会出现一块不透明的灰块
        setAttribute(Qt::WA_NoSystemBackground);
        m_label = new QLabel(this);
        m_label->setAlignment(Qt::AlignCenter);

        // 字体与颜色写死而不读宿主主题：插件不链接宿主的核心库，拿不到 AppSettings
        QFont f(QStringLiteral("Microsoft YaHei UI"));
        f.setPixelSize(26);
        f.setWeight(QFont::DemiBold);
        m_label->setFont(f);
        m_label->setStyleSheet(QStringLiteral("color:#E8ECF4;background:transparent;"));

        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &ClockWidget::tick);
        // 1 秒粒度刚好对上「显示到秒」：更密没意义，更疏会跳秒或卡在整分
        timer->start(1000);
        // 立刻先走一次，避免控件刚出现时空白一秒
        tick();
    }

    /**
     * 切换是否显示秒；切换后立刻重绘一次，不必等下一秒。
     * @param on true = 「HH:mm:ss」，false = 「HH:mm」
     */
    void setSecondsVisible(bool on)
    {
        m_showSeconds = on;
        tick();
    }

    /// 当前是否显示秒，供 saveWidgetState() 持久化
    bool secondsVisible() const { return m_showSeconds; }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        // 没有布局，标签要手动铺满；先让基类处理完再改子控件几何
        QWidget::resizeEvent(e);
        m_label->setGeometry(rect());
    }

private:
    /// 重新格式化当前时间写进标签；由 1 秒定时器与 setSecondsVisible() 调用
    void tick()
    {
        const QDateTime now = QDateTime::currentDateTime();
        // 日期那行用 ddd：本地化的短星期名（如「周一」），跟随系统区域设置
        const QString time = now.toString(m_showSeconds ? QStringLiteral("HH:mm:ss")
                                                        : QStringLiteral("HH:mm"));
        m_label->setText(time + QLatin1Char('\n') + now.toString(QStringLiteral("M月d日 ddd")));
    }

    QLabel *m_label = nullptr;  // 唯一子控件，父对象是本控件
    bool m_showSeconds = true;  // 是否显示秒；默认开，可从 boxes.json 恢复
};

/**
 * 插件入口类：负责向宿主描述自己，并把宿主回调转发给 ClockWidget。
 *
 * Q_PLUGIN_METADATA 的 FILE "metadata.json" 是给 Qt 读的插件元信息文件（构建时嵌进 DLL），
 * IID 必须与 IWidgetPlugin.h 里的 QDeskWidgetPlugin_iid 完全一致。
 */
class HelloClockPlugin : public QObject, public IWidgetPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QDeskWidgetPlugin_iid FILE "metadata.json")
    Q_INTERFACES(IWidgetPlugin)

public:
    /// 稳定不变的插件标识，写进 boxes.json；改了就认不出旧盒子
    QString pluginId() const override { return QStringLiteral("hello-clock"); }

    /// 新建组件菜单里的名字：刻意带「插件」后缀，方便和同名的内置组件区分
    QString displayName() const override { return QStringLiteral("示例时钟（插件）"); }

    /// 可选说明，显示在菜单项下方；不实现也行，只是新建时少一句提示
    QString description() const override
    {
        return QStringLiteral("演示如何写自己的组件插件：一个每秒走字的时钟");
    }

    /// 每次新建组件都会调一次，可以放心地 new（父对象交给宿主管理）
    /// 注意别返回单例/缓存的控件：多个组件窗口共用同一个 QWidget 会让后一个把前一个抢走
    QWidget *createWidget(QWidget *parent) override { return new ClockWidget(parent); }

    /// 标题栏文字：刻意不带「示例/插件」后缀，标题栏位置紧张，只写组件本身的名字
    QString widgetTitle() const override { return QStringLiteral("时钟"); }

    /**
     * 宿主「刷新」时调用。示例里用它演示回调：把「刷新」实现成切换「显示秒」，
     * 换成你自己的组件时，这里应该做真正的重新取数 / 重算。
     * @param w 宿主持有的控件，必须能 cast 成 ClockWidget 才能操作
     */
    void refreshWidget(QWidget *w) override
    {
        // 宿主传回来的可能是别人的控件（理论上不会），所以用 qobject_cast 而不是 static_cast
        if (auto *clock = qobject_cast<ClockWidget *>(w))
            clock->setSecondsVisible(!clock->secondsVisible());
    }

    /**
     * @param w     控件实例
     * @param state 输出参数，宿主的 JSON 对象；这里只存一个布尔字段
     */
    void saveWidgetState(QWidget *w, QJsonObject &state) override
    {
        if (auto *clock = qobject_cast<ClockWidget *>(w))
            state[QStringLiteral("showSeconds")] = clock->secondsVisible();
    }

    /**
     * @param w     控件实例
     * @param state 可能是空对象（首次创建），toBool(true) 的默认值负责兜底
     */
    void restoreWidgetState(QWidget *w, const QJsonObject &state) override
    {
        if (auto *clock = qobject_cast<ClockWidget *>(w))
            clock->setSecondsVisible(state.value(QStringLiteral("showSeconds")).toBool(true));
    }
};

// 本文件里定义了带 Q_OBJECT 的类，但 .cpp 不会被 moc 单独处理，
// 必须包含 moc 生成的实现，否则会出现 vtable/信号相关的链接错误
#include "HelloClockPlugin.moc"
