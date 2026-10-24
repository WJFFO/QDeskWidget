/**
 * @file WidgetRegistry.cpp
 * 组件注册表的实现：把内置组件与 QPluginLoader 扫出来的插件统一成 WidgetPanel，
 * 再通过一个适配器（PluginPanel）让「只有普通 QWidget 的插件」也能当面板用。
 *
 * 坑：
 *  - 插件 DLL 一旦 unload，之前拿到的 IWidgetPlugin* 立刻变野指针，所以扫描状态是
 *    进程级单例、只在 reloadPlugins() 里重建，不要在别处 delete loader；
 *  - 适配器里的插件控件没有布局，尺寸完全靠 resizeEvent 手动 setGeometry 铺满；
 *  - 插件返回的控件如果有自定义尺寸需求，必须靠 minimumSizeHint()/sizeHint() 表达，
 *    否则这里会退回到 170x110 的保守默认值。
 */
#include "widgets/WidgetRegistry.h"

#include "core/AppSettings.h"
#include "core/Paths.h"
#include "widgets/IWidgetPlugin.h"
#include "widgets/MusicWidget.h"
#include "widgets/WeatherWidget.h"

#include <QDebug>
#include <QDir>
#include <QJsonObject>
#include <QMenu>
#include <QPluginLoader>

namespace
{

/**
 * 把插件返回的普通 QWidget 适配成面板。
 * 插件接口只认「一个 QWidget」，而窗口需要的是 WidgetPanel（标题 / 角标 / 菜单 / 持久化），
 * 这一层负责把两边的调用互相转发；插件为 nullptr 时退化成标题「组件」的空面板。
 *
 * 适配后的尺寸约束：最小尺寸取插件控件的 minimumSizeHint()（拿不到再退 sizeHint()，
 * 都没有就用 170x110），所以插件想让窗口更小/更大，只能在自己的控件上表达。
 */
class PluginPanel : public WidgetPanel
{
public:
    /**
     * @param plugin 已加载的插件实例（生命周期由 RegistryState 持有，本类不负责释放）
     * @param cfg    盒子配置，用其中的 widgetState 给插件控件做首次恢复
     * @param parent 父控件，交给 WidgetPanel
     */
    PluginPanel(IWidgetPlugin *plugin, const BoxConfig &cfg, QWidget *parent)
        : WidgetPanel(parent)
        , m_plugin(plugin)
        , m_cfg(cfg)
    {
        // 插件控件由插件自己 new 出来，父对象先交给本面板，随后再用 setParent 兜一次
        // （有的插件会忽略传入的 parent，自己传 nullptr）
        QWidget *content = m_plugin ? m_plugin->createWidget(this) : nullptr;
        m_content = content;
        if (m_content) {
            m_content->setParent(this);
            m_content->show();
            // 构造期就把状态灌进去，避免插件先按默认值初始化再闪一下
            m_plugin->restoreWidgetState(m_content, cfg.widgetState);
        }
    }

    QString panelTitle() const override
    {
        // 插件没给标题时退回显示名，都没有再退回通用「组件」，保证标题栏不空
        if (!m_plugin)
            return QStringLiteral("组件");
        const QString t = m_plugin->widgetTitle();
        return t.isEmpty() ? m_plugin->displayName() : t;
    }

    // 插件只报数字（-1 = 无角标），原样透传；标题栏最终显示的是下面补了单位的文字
    int badgeCount() const override { return m_plugin ? m_plugin->badgeCount() : -1; }

    QString badgeText() const override
    {
        // 插件只报数字，单位由这层统一补；负数沿用基类约定 = 不显示
        const int n = badgeCount();
        return n >= 0 ? QStringLiteral("%1 项").arg(n) : QString();
    }

    void refresh() override
    {
        // 控件还没造出来时刷新没有意义，直接跳过（插件接口不接受空指针语义）
        if (m_plugin && m_content)
            m_plugin->refreshWidget(m_content);
    }

    // 持久化同样交给插件自己决定写什么键；这里只是把「控件 + 对象」转交过去
    void saveState(QJsonObject &o) const override
    {
        if (m_plugin && m_content)
            m_plugin->saveWidgetState(m_content, o);
    }

    // o 可能是空对象（首次创建）或旧版本写的，容错责任在插件实现里
    void restoreState(const QJsonObject &o) override
    {
        if (m_plugin && m_content)
            m_plugin->restoreWidgetState(m_content, o);
    }

    void buildMenu(QMenu *menu) override
    {
        if (!m_plugin)
            return;
        menu->addAction(QStringLiteral("刷新"), this, [this] { refresh(); });
        // 灰色不可点的条目只为把 pluginId 显示出来，方便用户排查「这个窗格是哪个 DLL」
        menu->addAction(QStringLiteral("插件：%1").arg(m_plugin->pluginId()), [] {})
            ->setEnabled(false);
    }

    QSize minimumSizeHint() const override
    {
        // 插件自己给出最紧凑尺寸；拿不到就退回一个保守的默认值
        QSize hint = m_content ? m_content->minimumSizeHint() : QSize();
        if (hint.width() <= 0 || hint.height() <= 0)
            hint = m_content ? m_content->sizeHint() : QSize();
        // 4000 是「明显异常」的阈值：插件若返回 QWIDGETSIZE_MAX 之类的值，会让窗口
        // 最小尺寸直接撑满屏幕，所以这里当无效值处理
        if (hint.width() <= 0 || hint.width() > 4000)
            hint.setWidth(170);
        if (hint.height() <= 0 || hint.height() > 4000)
            hint.setHeight(110);
        // 再兜一层硬下限：140x80 是内容区还能摆下标题与内容的最低要求
        return QSize(qMax(140, hint.width()), qMax(80, hint.height()));
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        // 插件控件不参与布局（没有 QLayout），所以每次尺寸变化都手动铺满内容区；
        // 先调基类让 QWidget 完成自身处理，再覆盖子控件几何
        WidgetPanel::resizeEvent(e);
        if (m_content)
            m_content->setGeometry(rect());
    }

private:
    IWidgetPlugin *m_plugin = nullptr; // 插件实例，归 RegistryState 里的 QPluginLoader 所有
    QWidget *m_content = nullptr;      // 插件创建的控件，父对象是本面板，随面板销毁
    BoxConfig m_cfg;                   // 创建时的配置副本；仅构造期用到，保留下来供以后取原始配置
};

/// 一条扫描结果：DLL 文件名（仅用于日志）+ 它导出的接口指针
struct PluginEntry
{
    QString file;                     // 文件名（不带目录），用于日志定位是哪个 DLL
    IWidgetPlugin *iface = nullptr;   // 接口指针；DLL 一旦 unload 立即失效，不可缓存到别处
};

/**
 * 进程级插件状态。用函数内 static 持有，保证扫描只做一次、且析构顺序在 main 之后。
 */
struct RegistryState
{
    QVector<PluginEntry> plugins;     // 扫描成功且实现了接口的插件
    QVector<QPluginLoader *> loaders; // 与 plugins 一一对应的 loader，用来保活 DLL（不 unload 就一直是常驻内存）
    bool loaded = false;              // false = 还没扫过，任何取用入口都会先触发一次 reloadPlugins()

    ~RegistryState()
    {
        // QPluginLoader 析构会 unload，交给进程退出时回收即可
        // 这里只删 loader 对象本身；此时已有面板都随窗口销毁，不会留下悬空 iface
        qDeleteAll(loaders);
    }
};

RegistryState &state()
{
    // 函数内 static：首次调用时构造，进程结束才析构，天然线程安全（C++11 起）
    static RegistryState s;
    return s;
}

/**
 * 内置组件清单。刻意做成只读静态数组：它同时被 types()（填菜单）和
 * displayNameFor()（按 kind 查名字）使用，两处必须看到同一份数据。
 */
const QVector<WidgetTypeInfo> &builtins()
{
    static const QVector<WidgetTypeInfo> list = {
        { QStringLiteral("weather"), QStringLiteral("天气"),
          QStringLiteral("实况 + 未来三天，界面随尺寸自适应（Open-Meteo，无需密钥）"),
          BoxConfig::KindWeather, false },
        { QStringLiteral("music"), QStringLiteral("音乐"),
          QStringLiteral("接管系统媒体会话：歌名、封面、进度、同步歌词，可切歌"),
          BoxConfig::KindMusic, false }
    };
    return list;
}

} // namespace

namespace WidgetRegistry
{

// 路径由 Paths::dataDir() 决定，本函数只做拼接、不创建目录：
// 「没有插件」是正常的初始状态，创建目录是安装插件或首次扫描时才需要的事
QString pluginsDir()
{
    return QDir(Paths::dataDir()).filePath(QStringLiteral("plugins"));
}

// 全量重建而非增量：插件数量少，重建比「对比增删」简单可靠，也不会留下失效的 iface。
// 顺序很关键 —— 先清 plugins 再删 loader，否则清空过程中还会短暂持有已卸载 DLL 的指针
void reloadPlugins()
{
    RegistryState &s = state();
    // 先扔掉旧结果：接口指针在 DLL 卸载后立刻失效，必须先清空再删 loader
    s.plugins.clear();
    qDeleteAll(s.loaders);
    s.loaders.clear();

    QDir dir(pluginsDir());
    // 目录不存在是常态（用户从没装过插件），不算错误
    if (!dir.exists())
        return;

    // 只认 .dll；子目录不递归，避免把依赖库当成插件扫进来
    const QFileInfoList files = dir.entryInfoList({ QStringLiteral("*.dll") }, QDir::Files);
    for (const QFileInfo &fi : files) {
        auto *loader = new QPluginLoader(fi.absoluteFilePath());
        // instance() 才真正 LoadLibrary 并实例化；失败通常是缺依赖或不是 Qt 插件
        QObject *obj = loader->instance();
        if (!obj) {
            qWarning() << "[plugin]" << fi.fileName() << "加载失败:" << loader->errorString();
            // 没加载成功就没什么可卸载的，直接删 loader 即可
            delete loader;
            continue;
        }
        auto *iface = qobject_cast<IWidgetPlugin *>(obj);
        if (!iface) {
            qWarning() << "[plugin]" << fi.fileName() << "不是 IWidgetPlugin";
            // 是 Qt 插件但接口不对：先 unload 再删，否则 DLL 会一直留在进程里
            loader->unload();
            delete loader;
            continue;
        }
        // 只记文件名：日志里够定位，完整路径太长会刷屏
        s.plugins.push_back({ fi.fileName(), iface });
        s.loaders.push_back(loader);
        qInfo() << "[plugin] 已加载" << iface->pluginId() << fi.fileName();
    }
    // 目录存在但没有插件时也要标记已扫过，否则每次 types() 都会重扫磁盘。
    // 注意：目录不存在的分支在上面提前 return 了，不会置位，下次调用会再判一次 exists
    s.loaded = true;
}

// 每次调用都重新拷贝一份返回，调用方可以随手改；内置项固定在前，用户插件按目录枚举顺序追加
QVector<WidgetTypeInfo> types()
{
    RegistryState &s = state();
    if (!s.loaded)
        reloadPlugins();

    // 内置项先拷一份再加插件：菜单里内置组件永远排在用户插件前面
    QVector<WidgetTypeInfo> out = builtins();
    for (const PluginEntry &p : s.plugins) {
        if (!p.iface)
            continue;
        WidgetTypeInfo info;
        info.id = p.iface->pluginId();
        info.name = p.iface->displayName();
        info.description = p.iface->description();
        // 插件没有独立的 kind，统一走 KindPlugin + pluginId 定位
        info.kind = BoxConfig::KindPlugin;
        info.plugin = true;
        out.push_back(info);
    }
    return out;
}

// 按 kind 分派；内置组件在这里就地 new 完再灌 widgetState，插件则转交 PluginPanel 处理。
// 返回 nullptr 是「这类盒子本来就没有面板」的正常结果，调用方据此降级，不要当异常处理
WidgetPanel *create(const BoxConfig &cfg, QWidget *parent)
{
    switch (cfg.kind) {
    case BoxConfig::KindWeather: {
        auto *p = new WeatherWidget(parent);
        // 一造出来就灌状态：面板在构造期只能拿全局设置/默认值初始化，
        // 盒子自己的城市、音量等由这一步覆盖（不灌就会用错配置）
        p->restoreState(cfg.widgetState);
        return p;
    }
    case BoxConfig::KindMusic: {
        auto *p = new MusicWidget(parent);
        p->restoreState(cfg.widgetState);
        return p;
    }
    case BoxConfig::KindPlugin: {
        RegistryState &s = state();
        // 懒加载兜底：正常流程里设置面板已经调过 types()，但直接读配置建窗时
        // 也可能先走到这里，补扫一次才能找到插件
        if (!s.loaded)
            reloadPlugins();
        // 线性查找：插件数量是个位数，没必要建索引
        for (const PluginEntry &e : s.plugins) {
            if (e.iface && e.iface->pluginId() == cfg.pluginId)
                return new PluginPanel(e.iface, cfg, parent);
        }
        // 插件被删了 / pluginId 写错了：返回空，由上层决定降级显示
        return nullptr;
    }
    default:
        // 走到这里说明是文件盒或已废弃的 KindSearch，本来就不该调 create()
        return nullptr;
    }
}

// 只用于「窗口标题兜底」，拿不到精确名字也必须返回一个非空的兜底词，
// 否则标题栏会出现空标题
QString displayNameFor(int kind, const QString &pluginId)
{
    if (kind == BoxConfig::KindPlugin) {
        // 插件必须按 id 精确匹配：可能装了多个插件
        for (const WidgetTypeInfo &t : types())
            if (t.id == pluginId && t.plugin)
                return t.name;
        // 插件没加载出来时至少把 id 显示出来，比一个笼统的「组件」更有助于排查
        return pluginId.isEmpty() ? QStringLiteral("组件") : pluginId;
    }
    // 内置项按 kind 匹配（一个 kind 只对应一个内置组件）
    for (const WidgetTypeInfo &t : builtins())
        if (t.kind == kind)
            return t.name;
    // 文件盒等非组件类型：给一个中性名字，调用方通常还会用 title 覆盖
    return QStringLiteral("整理盒");
}

// 只统计「扫到了且实现了接口」的插件；目录里放着但加载失败的 DLL 不算，符合用户在
// 「插件管理」里看到的可用数量
int pluginCount()
{
    RegistryState &s = state();
    if (!s.loaded)
        reloadPlugins();
    return int(s.plugins.size());
}

} // namespace WidgetRegistry
