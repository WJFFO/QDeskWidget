/**
 * @file IWidgetPlugin.h
 * 第三方组件插件接口：用户自己编一个 DLL，就能在整理盒里当小组件用。
 *
 * 关键约定：
 *  - 插件类必须同时继承 QObject 与 IWidgetPlugin，并带 Q_OBJECT / Q_PLUGIN_METADATA /
 *    Q_INTERFACES 三件套，否则宿主 qobject_cast 会失败（表现为「不是 IWidgetPlugin」）；
 *  - 接口本身不继承 QObject，只有被 qobject_cast 的控件与插件类需要 Q_OBJECT；
 *  - IID 必须与这里的 QDeskWidgetPlugin_iid 一致，改它会作废所有已编译的插件；
 *  - 接口方法全部在宿主 GUI 线程调用，插件内部不要假设有其它线程。
 *  - DLL 必须和宿主用同一套 Qt（同大版本、同编译器/运行库）编译，否则 instance() 直接
 *    返回 nullptr，日志里只看到「加载失败」，排查时先怀疑这条。
 */
#pragma once

#include <QJsonObject>
#include <QString>
#include <QtPlugin>

class QWidget;

/**
 * 第三方组件插件接口 —— 用户自己写一个 DLL，就能在整理盒里当组件用。
 *
 * 最小实现：
 *   class MyPlugin : public QObject, public IWidgetPlugin {
 *       Q_OBJECT
 *       Q_PLUGIN_METADATA(IID QDeskWidgetPlugin_iid FILE "metadata.json")
 *       Q_INTERFACES(IWidgetPlugin)
 *   public:
 *       QString pluginId() const override { return "hello"; }
 *       QString displayName() const override { return "你好"; }
 *       QWidget *createWidget(QWidget *parent) override { return new QLabel("hi", parent); }
 *       QString widgetTitle() const override { return "你好"; }
 *   };
 *
 * 编译出的 DLL 放进「数据目录/plugins」（程序菜单里可以直接打开这个目录），
 * 重启程序，或在 设置 → 组件 →「重新扫描插件」后，即可出现在添加组件的列表里。
 *
 * 宿主只承诺三件事：把 createWidget() 的返回值铺满内容区、在合适时机调用
 * refreshWidget()/save-restore、把 widgetTitle()/badgeCount() 显示到标题栏。
 * 标题栏、玻璃背景、折叠、圆角统统由宿主绘制，插件控件保持透明即可。
 */
class IWidgetPlugin
{
public:
    /// 必须虚析构：宿主只持有 IWidgetPlugin*，delete 时要能正确走到 DLL 内的派生类析构
    virtual ~IWidgetPlugin() = default;

    /**
     * 唯一 id，会写进 boxes.json 用来定位插件实现。
     * @return 小写英文与连字符，例如 "hello-clock"；发布后不要改，改了旧配置就找不到插件
     */
    virtual QString pluginId() const = 0;

    /**
     * 新建组件列表里显示的名字。
     * @return 中文短名，建议不超过 10 个字（菜单宽度有限）
     */
    virtual QString displayName() const = 0;

    /**
     * 可选说明。
     * @return 新建菜单里的一行补充说明；默认空串表示不显示
     */
    virtual QString description() const { return QString(); }

    /**
     * 创建组件内容控件；父对象交给宿主，返回 nullptr 表示失败。
     * @param parent 建议直接传给控件构造函数（可为 nullptr，宿主会补 setParent）
     * @return 自绘或自布局的控件；返回 nullptr 时宿主显示一个空面板
     *
     * @note 每新建一个组件窗口就会调一次，所以不能返回单例/静态控件；返回后所有权归宿主
     *       （父对象被设成宿主的面板），插件不要自己 delete。
     * @note 尺寸诉求只能通过控件的 minimumSizeHint()/sizeHint() 表达：宿主会拿它当窗口
     *       最小尺寸的下限；某一维 <= 0 或 > 4000 时该维退回默认值（宽 170 / 高 110），
     *       最后再夹一层 140x80 的硬下限，所以别指望报一个很小的值把窗口缩到极限。
     */
    virtual QWidget *createWidget(QWidget *parent) = 0;

    /**
     * 标题栏文字。
     * @return 组件标题；空串时宿主退回 displayName()
     * @note 宿主每次刷新标题栏（改配置、切成员、收 panelChanged）都会重新问一次，
     *       所以这里只该返回现成的字符串，不要在里面查数据/读文件。
     */
    virtual QString widgetTitle() const = 0;

    /**
     * 标题栏计数，返回负数不显示。
     * @return 非负数量（宿主显示成「N 项」）；-1 表示无角标
     */
    virtual int badgeCount() const { return -1; }

    /**
     * 刷新回调：定时刷新与菜单「刷新」都会走到这里。
     * @param w createWidget() 返回的那个控件（宿主保证非空，但插件要自己判类型）
     * @note 宿主可能连续调用（用户连点菜单），耗时操作要自己防重入，别指望宿主节流。
     */
    virtual void refreshWidget(QWidget *w) { Q_UNUSED(w) }

    /**
     * 把控件状态写进配置。
     * @param w     控件实例
     * @param state 输出参数：宿主已备好的对象，直接塞键值，不要 clear
     */
    virtual void saveWidgetState(QWidget *w, QJsonObject &state)
    {
        Q_UNUSED(w)
        Q_UNUSED(state)
    }

    /**
     * 从配置恢复状态；首次创建时 state 可能是空对象，实现必须容忍缺字段。
     * @param w     控件实例
     * @param state 上次 saveWidgetState() 写出的内容
     */
    virtual void restoreWidgetState(QWidget *w, const QJsonObject &state)
    {
        Q_UNUSED(w)
        Q_UNUSED(state)
    }
};

// 与 Q_PLUGIN_METADATA 的 IID 必须完全一致；改动等于换接口版本，旧 DLL 会加载失败
#define QDeskWidgetPlugin_iid "com.qdeskwidget.IWidgetPlugin/1.0"

QT_BEGIN_NAMESPACE
// 注册接口元信息，qobject_cast<IWidgetPlugin*>(pluginInstance) 靠它工作
Q_DECLARE_INTERFACE(IWidgetPlugin, QDeskWidgetPlugin_iid)
QT_END_NAMESPACE
