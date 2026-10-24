/**
 * @file App.h
 * 应用控制器（进程内唯一）：托盘图标与菜单、盒子窗口的生命周期、组合关系重建、
 * 设置对话框、以及 boxes.json 的防抖落盘。
 *
 * 关键约定 / 坑：
 *  - m_windows 只保存「宿主盒子」的窗口；组合成员不单独建窗口，由宿主窗口承载并切换显示；
 *  - 组合关系变化一律走 rebuildWindows() 整体重建（旧窗口 deleteLater），不做增量迁移，
 *    因此调用方不要跨调用缓存 FileBoxWindow*；
 *  - 关窗 ≠ 退出：盒子窗口只是隐藏到托盘，进程靠 setQuitOnLastWindowClosed(false) 常驻；
 *  - 单实例靠 QLocalServer 的命名管道实现，第二个进程只负责把已有实例的窗口叫出来。
 */
#pragma once

#include "core/BoxConfig.h"

#include <QHash>
#include <QObject>

class BoxManager;
class FileBoxWindow;
class SettingsDialog;
class QSystemTrayIcon;
class QTimer;
class QLocalServer;

/**
 * 应用控制器：管理托盘、盒子窗口生命周期、设置窗口与持久化。
 *
 * 组合（Group）的存储约定：宿主盒子的 groupMembers 里放其它成员 id，
 * 成员窗口不单独创建；成员的 currentMember 记录上一次显示的是谁。
 *
 * 补充：App 自身不持有 BoxConfig，所有盒子数据都在 m_boxes（BoxManager）里，
 * 窗口只把「当前显示的成员状态」经 stateChanged 回写，因此数据真值永远是 BoxManager。
 */
class App : public QObject
{
    Q_OBJECT

public:
    /** @param parent Qt 父对象，正常传 nullptr（App 是 main 里的栈对象，生命周期即进程） */
    explicit App(QObject *parent = nullptr);

    /**
     * 启动应用：抢单实例锁 → 加载 boxes.json → 按数据重建全部窗口 → 建托盘。
     * @return false 表示已有实例在运行（本进程已通知它 show 并应立即退出）；
     *         true  表示本进程成为主实例，可以继续进入事件循环。
     * 副作用：会创建 QLocalServer、读取磁盘配置、创建并 show 出所有盒子窗口。
     */
    bool start();

    /**
     * 开发/自检用：把所有盒子渲染成 PNG 保存到 dir。
     * @param dir 输出目录（不存在会被创建）；文件名为 box_<盒子id>.png
     * 副作用：组合窗口会被逐个切换成员并额外存 _memberN.png，切换次数等于成员数，
     *        因此所有截图跑完后成员位置绕一圈回到原处。
     */
    void saveSnapshots(const QString &dir);

    /**
     * 开发/自检用：按顺序切换材质并截图，用来验证切换一致性。
     * @param dir       输出目录，同时写入 material_check.txt 记录每步的 DWM 背板类型
     * @param materials 材质序列，元素取值 0 亚克力 / 1 云母 / 2 纯色（对应 AppSettings::MaterialMode）
     * 异步执行：每步间隔 1500ms，全部走完后自动 quit()，调用方不必自己收尾。
     */
    void runMaterialSequence(const QString &dir, const QList<int> &materials);

    /**
     * 开发/自检用：把主题切到另一档、落盘并刷新所有窗口（`--theme-flip <毫秒>` 调用）。
     * 用来验证「运行中换主题」时组件面板有没有重新上色 —— 面板里 QSS / setTextColor /
     * 染色图标这些颜色都是设置那一刻固定的，不重设就一直是旧主题的配色。
     */
    void toggleThemeForTest();

    /**
     * 开发/自检用：逐个删除所有盒子（不走对话框）。
     * 每删一个都 processEvents()，模拟事件循环真正销毁窗口的时序。
     */
    void removeAllBoxes();

    /**
     * 开发/自检用：走一遍「删除…」对话框并自动点确认（复现用户路径）。
     * 只处理 m_windows 里的第一个窗口；没有窗口时静默返回。
     * 时序：300ms 后触发删除 → 600ms 后在模态循环里找按钮文本含「删除整理盒」的按钮并点击。
     */
    void deleteLastBoxViaUi();

    /**
     * 开发/自检用：直接打开设置对话框。
     * 菜单里的入口只能靠鼠标点，命令行没法触发；调设置外观、截图对比时用 `--settings` 走这里。
     */
    void openSettingsForTest() { openSettings(); }

    /**
     * 新建一个组件盒子（走和菜单完全相同的路径，组件按最小尺寸落地）。
     * 公开出来是为了让 `--new-widget <kind>` 自检参数能复用同一条路径。
     * @param kind     组件类型，取 BoxConfig::KindWeather / KindMusic / KindPlugin
     * @param pluginId 插件 id，仅 kind == KindPlugin 时有意义；内置组件传空串
     * @param title    期望标题（重名由 BoxManager 自动加 " (2)" 后缀）
     * 时序保证：createWidget 内部同步 emit boxAdded，窗口在函数返回前已进入 m_windows，
     *          因此紧接着的 shrinkToMinimum() 一定能拿到窗口。
     */
    void createWidgetBox(int kind, const QString &pluginId, const QString &title);

private:
    /** 搭托盘图标与右键菜单（新建整理盒 / 显示隐藏 / 设置 / 退出），并接上单击与双击行为 */
    void buildTray();
    /**
     * 为单个宿主盒子创建窗口并接好所有信号。
     * @param cfg 宿主配置；若其 groupMembers 非空，会一并把成员配置塞进窗口
     * 副作用：窗口插入 m_windows 后立刻 show()。
     * 已存在同 id 窗口时不重建，只 setConfig() 覆盖当前成员配置后返回。
     */
    void createWindow(const BoxConfig &cfg);
    /**
     * 按 BoxManager 里的当前数据整体重建所有窗口：先销毁旧窗口，再清掉指向已删盒子的
     * 「幽灵成员」引用，最后只为非成员（组长 / 独立盒子）建窗口。
     * 组合关系变化后必须调用它，不要试图就地改窗口。
     */
    void rebuildWindows();
    /** @return BoxManager 中全部盒子配置的副本；供窗口的组合菜单现取候选列表 */
    QVector<BoxConfig> allBoxes() const;

    /** 弹「新建整理盒」对话框，用户确认后交给 BoxManager::create 建盒（窗口由 boxAdded 信号建出） */
    void newBox();
    /**
     * 增 / 删组合成员，然后整体重建窗口。
     * @param hostId   组长盒子 id（窗口归属方）
     * @param memberId 被加入或移出的盒子 id；等于 hostId 时忽略
     * @param add      true 加入（会先把 memberId 从其它组合里摘出来，并清空它自己的成员表），
     *                 false 移出
     * 副作用：立即 save() 并 rebuildWindows()。
     */
    void groupChange(const QString &hostId, const QString &memberId, bool add);
    /** 打开（或前置）设置对话框；同一个实例复用，关掉后销毁，下次重新创建 */
    void openSettings();
    /** 托盘单击：全隐 ↔ 全显；以「当前是否存在可见窗口」决定方向 */
    void toggleAllWindows();
    /** 无条件显示所有盒子窗口（多实例唤醒时也走这里） */
    void showAllWindows();
    /** 无条件隐藏所有盒子窗口（进程继续驻留托盘） */
    void hideAllWindows();
    /** 把当前外观设置推到所有窗口（换肤、改材质、改图标尺寸后调用） */
    void applyAppearanceToAll();
    /**
     * 安排一次 450ms 后的落盘。
     * 拖动窗口、改设置这类高频操作会连续触发，靠单次定时器重置实现防抖，
     * 避免每帧都全量重写 boxes.json。
     */
    void scheduleSave();

    BoxManager *m_boxes = nullptr;                 // 盒子数据的唯一权威来源，父子关系挂在 this 上
    QHash<QString, FileBoxWindow *> m_windows;     // 宿主盒子 id → 窗口；组合成员不出现在这里
    QSystemTrayIcon *m_tray = nullptr;             // 托盘图标，进程常驻的入口；随 App 析构
    SettingsDialog *m_settings = nullptr;          // 设置对话框，懒创建；为空表示当前没开，关闭后置空
    QTimer *m_saveTimer = nullptr;                 // 单次定时器，间隔 450ms，超时即 m_boxes->save()（防抖用）
    QLocalServer *m_server = nullptr;              // 单实例命名管道服务端；listen 失败时为空（此时不拦重复启动）
};
