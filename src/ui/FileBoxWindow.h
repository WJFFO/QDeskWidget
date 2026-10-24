/**
 * @file FileBoxWindow.h
 * 桌面上的「整理盒」窗口：一个 FileBoxWindow 对应一个盒子（文件整理盒或小组件），
 * 自带标题栏、内容区、折叠/胶囊形态、拖动吸附与右键菜单；组合时同一窗口承载多个成员。
 *
 * 关键约定 / 坑：
 *  - 窗口尺寸只有一个出口 FileBoxWindow::applyGeometryForState()，别在别处零散 setFixedSize；
 *  - m_cfg 永远是「当前显示的成员」，m_hostId 才是窗口归属，两者在组合里可能不同；
 *  - 锁定大小 / 胶囊态必须在 nativeEvent 里显式返回 HTCLIENT，否则 WS_THICKFRAME 仍会给出缩放边。
 */
#pragma once

#include "core/BoxConfig.h"
#include "mrowlayout.h"

#include <QIcon>
#include <QLabel>
#include <QResizeEvent>
#include <QVector>
#include <QWidget>
#include <functional>

class FileItemModel;
class FileListView;
class StackCard;
class StackPopup;
class MColumnLayout;
class WidgetPanel;
class QLineEdit;
class QPropertyAnimation;
class QToolButton;
class QTimer;

/**
 * 自动省略号的标题标签（随宽度变化重新省略）。
 *
 * sizeHint 用「完整文本」的宽度而不是当前显示文本的宽度：布局按完整标题分配空间，
 * 放不下时才由 updateElide() 按实际宽度省略；否则会出现「省略变短 → sizeHint 更小 →
 * 分到的空间更少 → 更省略」的自我收缩，标题会一路缩到只剩「…」。
 */
class ElideLabel : public QLabel
{
    Q_OBJECT

public:
    /**
     * @brief 构造标题标签。
     * @param parent 父控件，通常是 BoxHeaderBar；为空则成为独立窗口。
     *
     * 尺寸策略取 Preferred（而非 Ignored），让标题的 sizeHint 参与布局分配，
     * 否则弹性空间会全部让给弹簧，标题只能分到一半宽度。
     */
    explicit ElideLabel(QWidget *parent = nullptr);

    /**
     * @brief 设置完整标题：同时更新 tooltip 并按当前宽度重算省略文本。
     * @param text 未省略的原始标题，允许为空串。
     */
    void setFullText(const QString &text);

    /** @return 未省略的完整标题（不是界面上当前显示的省略文本）。 */
    QString fullText() const { return m_text; }

    /**
     * @brief 按完整文本给出建议尺寸，避免布局因「已省略」而收缩。
     * @return 宽度为完整文本的 advance、高度为当前字体行高。
     */
    QSize sizeHint() const override;

    /**
     * @brief 允许被压缩到 0 宽（标题可以被完全挤掉），高度保持一行字体高。
     * @return 宽度固定 0、高度为字体行高。
     */
    QSize minimumSizeHint() const override;

protected:
    /**
     * @brief 宽度变化后重新计算省略文本。
     * @param e 尺寸变化事件，先交给 QLabel 处理再重算。
     */
    void resizeEvent(QResizeEvent *e) override;

private:
    /** 按当前宽度重新省略并写回 QLabel::setText；外部只应通过 setFullText 改标题。 */
    void updateElide();

    QString m_text;                              // 未省略的完整标题，sizeHint 与重命名框初值都用它
    Qt::TextElideMode m_mode = Qt::ElideRight;   // 省略位置，目前固定右侧且未对外暴露 setter
};

/**
 * 整理盒标题栏 —— 基于 MRowLayout 的单行布局：
 *   [图标] [标题(可展开)]  ……  [1/3] [项数] [折叠] [更多]
 * 折叠成胶囊时切换为紧凑布局，只留图标 + 标题 + 数量。
 * 「1/3」是小组件组合的成员指示；在标题栏上滚轮可直接切换成员。
 *
 * 交互语义：按下只发 pressedAt（由窗口记录起点），移动超过 4px 才开始发 draggedTo，
 * 松手时按「是否拖动过」二选一发 dragFinished 或 clicked —— 因此单击与拖动不会互相误触。
 * 标题栏自身不移动窗口，拖动位移全部由 FileBoxWindow 计算，这里只负责报告全局坐标。
 */
class BoxHeaderBar : public MRowLayout
{
    Q_OBJECT

public:
    /**
     * @brief 构造标题栏并搭好「图标 / 标题 / 右侧动作组」三段结构。
     * @param parent 父控件，通常是 FileBoxWindow；构造后高度固定为 kHeight。
     */
    explicit BoxHeaderBar(QWidget *parent = nullptr);

    /**
     * @brief 设置标题（完整文本交给 ElideLabel，过长时按当前宽度省略）。
     * @param title 未省略的标题文本。
     */
    void setTitle(const QString &title);

    /**
     * @brief 设置项目数文本，供文件整理盒使用。
     * @param count 项目数；传负数表示该盒子没有「计数」概念（组件），直接隐藏计数区。
     *              0 显示「空」，正数显示「N 项」。
     */
    void setCount(int count);

    /**
     * @brief 组件自定义徽标，空表示隐藏。
     * @param text 直接显示的文本（如温度、曲名缩写）；与 setCount 共用同一个 QLabel，后调用者生效。
     */
    void setBadge(const QString &text); // 组件自定义徽标，空表示隐藏

    /**
     * @brief 只切换折叠指示箭头的字形，不改窗口状态。
     * @param collapsed true 显示「▸」（可展开），false 显示「▾」（可折叠）。
     */
    void setCollapsed(bool collapsed);

    /**
     * @brief 设置头部图标。
     * @param icon 文件夹或组件的图标；内部按当前形态取 16 或 20 px 的 pixmap，并缓存原图。
     */
    void setFolderIcon(const QIcon &icon);

    /**
     * @brief 切换胶囊 / 常规形态的排版（高度、边距、按钮可见性、图标尺寸）。
     * @param capsule true 进入胶囊：高 kCapsuleHeight、隐藏折叠与「⋯」、图标缩到 16px。
     *                状态未变化时直接返回，幂等。
     */
    void setCapsuleMode(bool capsule);

    /**
     * @brief 设置组合成员指示（如 "1/3"），空字符串表示隐藏。
     * @param text 成员指示文本；真正是否可见还要过 updateResponsive() 的宽度门槛。
     */
    void setMemberBadge(const QString &text); // 空字符串表示隐藏

    /** 在标题位置浮出内联 QLineEdit 开始重命名；已处于编辑态时直接返回。提交/取消见 finishEdit()。 */
    void beginEditTitle();

    /** 按当前主题与形态（常规/胶囊）重设字体与样式表；换肤或切换形态后必须调用。 */
    void applyTheme();

    /**
     * 胶囊模式下按内容计算合适的宽度
     * @return 胶囊宽度像素值，已夹在 [96, 360] 区间内（窗口侧还会再夹到 340）。
     */
    int capsuleWidthHint() const;

    static constexpr int kHeight = 42;         // 展开态标题栏高度；窗口最小高度也以它为基准
    static constexpr int kCapsuleHeight = 34;  // 胶囊态总高度（胶囊本身没有内容区）

signals:
    /** 左键按下 @param globalPos 按下点的全局坐标，窗口据此记录拖动起点 */
    void pressedAt(const QPoint &globalPos);
    /** 越过拖动阈值后的每次移动 @param globalPos 当前鼠标全局坐标，窗口做 1:1 跟手位移 */
    void draggedTo(const QPoint &globalPos);
    /** 松手且确实拖动过；窗口在此时吸附网格并落盘 */
    void dragFinished();
    void clicked();               // 按下后未拖动就松开；折叠态下窗口据此展开
    void collapseRequested();     // 点了「▾/▸」
    /** 需要弹出菜单 @param globalPos 菜单的全局弹出位置（右键点或「⋯」按钮右下角） */
    void menuRequested(const QPoint &globalPos);
    /** 内联重命名提交 @param title 去掉首尾空白后的新标题（非空且与原标题不同才会发出） */
    void titleEdited(const QString &title);
    void memberStepRequested(int delta); // 滚轮切换组合成员

protected:
    /** @brief 左键按下记录起点并发 pressedAt；右键直接发 menuRequested。@param e 鼠标事件 */
    void mousePressEvent(QMouseEvent *e) override;
    /** @brief 按住左键且位移超过 4px 才进入拖动，之后逐帧发 draggedTo。@param e 鼠标事件 */
    void mouseMoveEvent(QMouseEvent *e) override;
    /** @brief 松手时按是否拖动过发 dragFinished 或 clicked。@param e 鼠标事件 */
    void mouseReleaseEvent(QMouseEvent *e) override;
    /** @brief 左键双击进入标题内联编辑，并清掉按下/拖动状态。@param e 鼠标事件 */
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    /** @brief 标题栏任意位置右键都弹同一个菜单。@param e 上下文菜单事件 */
    void contextMenuEvent(QContextMenuEvent *e) override;
    /** @brief 尺寸变化后重跑响应式收敛，并让编辑框跟随标题位置。@param e 尺寸变化事件 */
    void resizeEvent(QResizeEvent *e) override;
    /** @brief 有成员徽标时滚轮切换成员，否则交回 MRowLayout。@param e 滚轮事件 */
    void wheelEvent(QWheelEvent *e) override;
    /** @brief 只接管编辑框的 Esc（取消编辑），其余交回基类。@param obj 事件接收者 @param e 事件 */
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    /**
     * @brief 结束内联编辑。
     * @param commit true 提交（非空且与原标题不同才发 titleEdited），false 丢弃（Esc）。
     */
    void finishEdit(bool commit);
    void updateResponsive(); // 窄标题栏时隐藏徽标

    ElideLabel *m_title = nullptr;      // 标题，完整文本无法一次显示完时自动省略
    QLabel *m_icon = nullptr;           // 头部图标，胶囊态 16px、展开态 20px
    QLabel *m_count = nullptr;          // 计数/徽标二合一文本（"N 项" / "空" / 组件自定义）
    QLabel *m_member = nullptr;         // 组合成员指示 "1/3"，成员数 ≤1 时文本为空
    QToolButton *m_collapse = nullptr;  // 「▾/▸」折叠按钮，胶囊态隐藏
    QToolButton *m_menu = nullptr;      // 「⋯」更多操作按钮，胶囊态隐藏
    QLineEdit *m_edit = nullptr;        // 内联重命名框，首次 beginEditTitle 时才创建
    MRowLayout *m_actions = nullptr;    // 右侧动作组的子布局：成员/计数/折叠/菜单都挂在这里一起贴右
    QIcon m_folderIcon;                 // 图标原图缓存，切换胶囊/展开时要按新尺寸重新取 pixmap

    QPoint m_pressGlobal;               // 左键按下点的全局坐标，用于判断 4px 拖动阈值
    bool m_pressed = false;             // 左键已按下且未松开；与 m_dragging 共同决定松手发哪个信号
    bool m_dragging = false;            // 已经越过阈值、正在发送 draggedTo
    bool m_editing = false;             // 重命名框显示中，防止 beginEditTitle 重入
    bool m_capsule = false;             // 当前是否胶囊形态，决定字体字号、边距与按钮可见性
    bool m_countWanted = false;         // 业务上要不要显示计数；可见性还要过 updateResponsive 的宽度门槛
    bool m_memberWanted = false;        // 业务上要不要显示成员徽标；同样受宽度门槛约束
};

/**
 * 盒子窗口：可以是文件整理盒，也可以是小组件（搜索 / 天气 / 音乐 / 插件）。
 * 其中搜索组件已移除，KindSearch 仅用于识别并丢弃旧配置。
 * 根容器使用 MColumnLayout：[标题行] / [内容]，无边框、可拖动、可折叠成胶囊。
 *
 * 组合（Group）：一个窗口可以容纳多个盒子配置，Ctrl+Tab / 标题栏滚轮切换成员，
 * 成员配置写回各自的 BoxConfig（窗口本身归属宿主 id）。
 *
 * 内容区三选一（同一时刻只显示一个）：FileListView（图标/列表）、StackCard（堆叠）、
 * WidgetPanel（组件面板）。折叠态下三者全部隐藏，只剩标题栏。
 */
class FileBoxWindow : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 构造盒子窗口：搭 UI、恢复几何、应用外观并加载内容。
     * @param cfg 盒子配置；构造后窗口归属的 hostId 就固定为 cfg.id，之后不再改变。
     * @param parent 父控件，正常为 nullptr —— 盒子是需要独立窗口标志的顶层 Tool 窗口。
     *
     * 注意：这里不会把窗口直接拉到 cfg 里的尺寸，而是先记成展开态尺寸，再交给
     * applyGeometryForState() 按面板下限夹紧；折叠态在构造末尾才真正应用。
     */
    explicit FileBoxWindow(const BoxConfig &cfg, QWidget *parent = nullptr);

    /**
     * @brief 析构：若还挂着 qApp 级事件过滤器（隐藏了头部的组件）就摘掉，避免悬空指针。
     */
    ~FileBoxWindow() override;

    /** @return 窗口归属的宿主盒子 id；组合切换到其它成员时不变。 */
    QString boxId() const { return m_hostId; }
    /** @return 当前显示成员的配置（可能是组合里的非宿主成员）。 */
    const BoxConfig &config() const { return m_cfg; }

    /**
     * @brief 外部（设置面板、App）整体替换当前成员配置并立即重新应用。
     * @param cfg 新配置；若其 id 与当前成员一致，会同步回 m_members 对应槽位。
     */
    void setConfig(const BoxConfig &cfg);

    /**
     * 设置组合成员（第一个必须是宿主自己）；currentId 为初始显示位置
     * @param members 成员配置列表，约定第 0 个是宿主；空列表直接忽略。
     * @param currentId 初始要显示的成员 id（注意是 id 而非下标）；找不到时回退到 0 号成员。
     */
    void setGroupMembers(const QVector<BoxConfig> &members, const QString &currentId);
    /** @return 成员配置列表副本（0 号是宿主），供外部持久化组合关系。 */
    QVector<BoxConfig> groupMembers() const { return m_members; }

    /**
     * 由 App 注入：列出全部盒子，用于「组合」菜单
     * 回调在每次打开菜单时现取，保证列表反映最新的盒子集合，而不是构造时的快照。
     */
    using BoxListProvider = std::function<QVector<BoxConfig>()>;
    /** @brief 注入「全部盒子」查询回调。@param fn 返回当前所有盒子配置的函数，可为空（此时组合菜单无候选项） */
    void setBoxListProvider(BoxListProvider fn) { m_boxListProvider = std::move(fn); }

    /** @return 组合成员数量，恒 ≥1（构造时至少含宿主自己）。 */
    int memberCount() const { return int(m_members.size()); }

    /**
     * @brief 按步长切换组合成员（环形回绕）。
     * @param delta 步长：+1 下一个、-1 上一个；为 0 或成员数 <2 时直接返回。
     *              切换前会先把当前成员的即时状态写回 m_members，避免丢失未落盘的改动。
     */
    void switchMember(int delta);

    /** 重新按 AppSettings 应用外观与表面材质；换肤、改设置后由 App 统一调用。 */
    void applyAppearance();
    /** 重新加载内容：文件盒刷新模型，组件盒刷新面板并更新标题。 */
    void refresh();

    /**
     * 把窗口收缩到组件的最紧凑版式（面板 minimumSizeHint() 决定的下限）。
     * 新建组件时用它落地，避免一上来就是一大块空白。
     * 只对展开态的组件盒生效；文件盒或已折叠时直接返回。
     */
    void shrinkToMinimum();

    /**
     * @brief 切换折叠 / 展开。
     * @param collapsed true 折叠（只剩标题栏，可能变胶囊）、false 展开。
     *
     * 幂等：状态没变时只重跑一次几何计算。隐藏了头部的组件盒不允许折叠（折叠后窗内为空）。
     * 折叠前会把当前尺寸记进 m_expandedWidth/Height，展开时还原。
     */
    void setCollapsed(bool collapsed);
    /** 在折叠 / 展开之间取反。 */
    void toggleCollapsed();

    /**
     * @brief 设置是否隐藏标题栏（仅组件盒有意义，用于让内容铺满整窗）。
     * @param hidden true 隐藏；隐藏时会先强制展开，并安装 qApp 级事件过滤器接管拖动与右键菜单。
     */
    void setHeaderHidden(bool hidden);

    /** 开发自检：走一遍「删除…」对话框路径（含 QMessageBox） */
    void deleteBoxForTest() { deleteBox(); }
    /** @return 当前配置里的折叠标志（展开/折叠的唯一真值来源）。 */
    bool isCollapsed() const { return m_cfg.collapsed; }

    /** 进入标题内联重命名（等价于双击标题栏）。 */
    void beginRenameBox();

signals:
    /** 状态已写回 m_cfg @param id 当前成员 id；宿主据此覆盖存储并安排落盘 */
    void stateChanged(const QString &id);
    /** 用户确认删除 @param id 要删除的盒子 id（宿主删除时 App 会连带清理其成员） */
    void removeRequested(const QString &id);
    /** 菜单里点了「新建整理盒」 */
    void newBoxRequested();
    /** 菜单里点了「整理盒设置…」 */
    void settingsRequested();
    /** 组合成员切换完成 @param hostId 宿主 id @param memberId 新显示的成员 id */
    void currentMemberChanged(const QString &hostId, const QString &memberId);
    /** 请求增删组合成员 @param memberId 目标盒子 id @param add true 加入组合、false 移出组合 */
    void groupChangeRequested(const QString &memberId, bool add);

protected:
    /** @brief 画圆角底色、描边，以及文件盒为空时的居中提示。@param e 绘制事件（未使用） */
    void paintEvent(QPaintEvent *e) override;
    /** @brief 每次显示时重刷材质与圆角蒙版（隐藏期间主题、缩放或 DWM 状态都可能已经变了）。@param e 显示事件 */
    void showEvent(QShowEvent *e) override;
    /**
     * @brief 尺寸变化后夹紧组件盒的上下限，并维持折叠/胶囊的目标高度。
     * @param e 尺寸变化事件
     *
     * 外部 SetWindowPos 或程序内部都可能绕过 minimum/maximum，所以这里兜底再夹一次。
     */
    void resizeEvent(QResizeEvent *e) override;
    /** @brief 非拖动引起的移动（如系统摆放）也要落盘，否则重启后位置回退。@param e 移动事件 */
    void moveEvent(QMoveEvent *e) override;
    /** @brief 键盘兜底切换成员（Ctrl+Tab / Ctrl+PgUp/PgDn）。@param e 按键事件 */
    void keyPressEvent(QKeyEvent *e) override;
    /** @brief 只接受带 URL 的拖入，并固定为 CopyAction（真实移动与否在 drop 时按 Shift 判断）。@param e 拖入事件 */
    void dragEnterEvent(QDragEnterEvent *e) override;
    /** @brief 落盘拖入的文件：默认复制，按住 Shift 改为移动。@param e 放下事件 */
    void dropEvent(QDropEvent *e) override;
    /**
     * @brief 接管 WM_NCHITTEST，手工给出 8 个方向的缩放边（无边框窗口没有系统边框可命中）。
     * @param eventType 平台事件类型，只处理 "windows_generic_MSG"
     * @param message 指向 MSG 的指针（Windows 下）
     * @param result 命中测试结果（HTLEFT/HTCLIENT 等）
     * @return true 表示已处理并填好 result；false 表示交回 QWidget/DefWindowProc
     */
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    /**
     * @brief 隐藏头部时在应用级接管拖动与右键菜单。
     * @param obj 事件接收者，可能是 QWidget，也可能是 QWindow（QWidgetWindow）
     * @param e 事件
     * @return true 表示事件已被消费
     */
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    /** 搭 UI：建模型/视图/堆叠卡/组件面板/标题栏，串好所有信号与快捷键。 */
    void buildUi();
    /** @brief 填「标题栏菜单」。@param menu 由调用方创建并已套好样式的菜单 */
    void buildHeaderMenu(QMenu *menu);
    /** @brief 追加组件专属菜单项（隐藏标题栏、刷新、面板自定义项）。@param menu 目标菜单 */
    void buildWidgetMenu(QMenu *menu);
    /** @brief 追加「组合」子菜单：切换成员、加入、移出。@param menu 目标菜单 */
    void buildGroupMenu(QMenu *menu);
    /** @brief 填「空白处右键菜单」（新建文件夹、粘贴、视图/排序/图标大小等）。@param menu 目标菜单 */
    void buildEmptyMenu(QMenu *menu);
    /** @brief 填「选中项右键菜单」。@param menu 目标菜单 @param path 右键命中的文件路径（用于定位与判断是否 .pile 堆） */
    void buildItemMenu(QMenu *menu, const QString &path);
    /** 重算窗口标志：Tool + 无边框 + 无阴影，并按 alwaysOnTop 加置顶/置底。标志变化会重建原生窗口（需重 show）。 */
    void applyFlags();
    void applySurface();       // 材质 + 圆角
    /** 尺寸总闸：展开态按面板上下限夹紧，折叠态钉成标题栏高，锁定则 setFixedSize。 */
    void applyGeometryForState();
    /**
     * 启动折叠/展开的高度动画（只改 geometry，不动位置）。
     * @param from       起点尺寸（切换前的窗口大小）
     * @param to         终点尺寸（目标状态的大小）
     * @param collapsing true = 正在收起（内容区按「当前高度」钉住，被窗口裁掉）
     */
    void startCollapseAnimation(const QSize &from, const QSize &to, bool collapsing);
    /** 动画收尾：解除内容区高度锁定、按状态隐藏内容、重建上下限、刷表面、落盘。 */
    void finishCollapseAnimation();
    /**
     * 动画期间给内容区钉一个固定高度，避免布局每帧重排（组件面板重排一次要几十毫秒）。
     * @param h 内容区高度（px）；<= 0 表示解除固定、交回布局
     */
    void lockBodyHeight(int h);
    void updateBody();         // 网格/列表/堆叠 ↔ 组件面板
    void updateHeader();       // 标题、图标、计数、成员徽标
    /** 把当前状态写回 m_cfg 并 emit stateChanged；只有宿主成员写 geometry。 */
    void persist();
    /** 刷新头部计数/徽标（文件盒按条目数，组件盒按 badgeText）。 */
    void updateCount();

    /** 按当前成员配置创建组件面板，并把 panelChanged 接到「更新头部 + 落盘」。创建失败时 m_panel 保持空。 */
    void createPanel();
    /** 销毁组件面板：先摘出布局、摘掉父对象，再 deleteLater（避免旧面板继续参与布局/绘制）。 */
    void destroyPanel();
    void applyMember();        // 切换成员后重新加载内容
    void updateHeaderFilter(); // 隐藏头部时接管拖动与右键菜单
    /**
     * @brief 松手时按全局网格步长对齐窗口位置。
     * @param p 待吸附的左上角坐标（通常是当前 pos()）
     * @return 吸附后的坐标；关闭「吸附到网格」时原样返回 p。
     */
    QPoint snappedPos(const QPoint &p) const; // 松手时对齐网格用

    /**
     * @brief 缩放热区的宽度（物理像素）。
     *
     * 无边框窗口没有系统边框，WM_NCHITTEST 全靠自己判，所以这里按 DPI 放大
     * （8 逻辑像素 ≈ Windows 标准尺寸边框），否则高 DPI 下只剩几个像素，很难点到。
     * @return 单边热区宽度，恒 >= kResizeBorder
     */
    int resizeBorderPx() const;
    /** @return 标题栏显示的文字：优先组件自报标题，其次配置标题，最后按 kind/pluginId 查注册表名。 */
    QString headerTitle() const;

    /** @brief 弹出单个文件的右键菜单。@param path 命中文件 @param globalPos 弹出位置 */
    void onItemContextMenu(const QString &path, const QPoint &globalPos);
    /** @brief 弹出空白处右键菜单。@param globalPos 弹出位置 */
    void onEmptyContextMenu(const QPoint &globalPos);
    /** @brief 打开文件 / 文件夹；.pile 结尾的目录改为弹堆叠窗口。@param path 目标路径，空则忽略 */
    void onOpenRequested(const QString &path);
    /** 弹出堆叠视图（整盒内容）；盒子为空时先提示再返回。 */
    void openStackPopup();
    /** @brief 以独立堆叠窗口展开一个 .pile 目录。@param folder 堆目录 @param title 弹窗标题（通常为去掉 .pile 的目录名） */
    void openPile(const QString &folder, const QString &title);

    /** @brief 把选中项移进新建的 .pile 目录。@param paths 要堆叠的绝对路径列表，空则忽略 */
    void makePile(const QStringList &paths);
    /** @brief 把 .pile 目录里的内容移回盒子目录并删除该堆目录。@param pileFolder 堆目录路径，非目录则忽略 */
    void unpackPile(const QString &pileFolder);

    /** @brief 设置排序方式 @param mode 0 手动 / 1 名称 / 2 修改时间 / 3 类型 / 4 大小 */
    void setSortMode(int mode);
    /** @brief 设置视图方式 @param mode 0 图标 / 1 列表 / 2 堆叠（2 只影响内容区显示哪个控件） */
    void setViewMode(int mode);
    /** @brief 设置折叠形态 @param mode -1 跟随全局 / 0 标题条 / 1 胶囊；仅在当前已折叠时立即生效 */
    void setCapsuleMode(int mode);
    /** @brief 设置图标尺寸 @param size 像素值，0 表示跟随全局（实际值走 effectiveIconSize()） */
    void setIconSize(int size);
    /** @brief 更换盒子关联的文件夹 @param folder 新目录（会转成 '/' 分隔） @param managed 是否由程序托管创建 */
    void setBoxFolder(const QString &folder, bool managed);
    /**
     * 弹确认框后请求删除本盒子。
     * 注意：当前实现是同步 emit removeRequested，安全性依赖接收方用 deleteLater 销毁窗口；
     * 若接收方改成同步 delete，必须改成 QTimer::singleShot(0, ...) 延后发出，否则栈上的对话框会被二次释放。
     */
    void deleteBox();
    /** 把系统剪贴板里的路径粘贴进盒子目录：剪切则移动，否则复制。 */
    void pasteIntoBox();
    /** @brief 追加「查看方式」三项 @param menu 目标菜单（通常是子菜单） */
    void addViewMenuActions(QMenu *menu);
    /** @brief 追加「排序方式」五项（带 ✓ 勾选前缀） @param menu 目标菜单 */
    void addSortMenuActions(QMenu *menu);
    /** @brief 追加「图标大小」六档（含「跟随全局」） @param menu 目标菜单 */
    void addSizeMenuActions(QMenu *menu);

    BoxConfig m_cfg;                 // 当前显示的成员配置
    QVector<BoxConfig> m_members;    // 组合成员配置（0 号是宿主）
    int m_cur = 0;                   // 当前成员下标
    QString m_hostId;                // 窗口归属的盒子 id

    FileItemModel *m_model = nullptr;      // 文件模型（组件盒里 folder 被置空，但仍存在：堆叠卡与计数信号都挂在它上面）
    FileListView *m_view = nullptr;        // 图标/列表视图，图标与列表两种模式共存于同一个控件
    StackCard *m_stackCard = nullptr;      // 堆叠视图的封面卡，点击后弹出 StackPopup
    StackPopup *m_popup = nullptr;         // 堆叠展开弹窗，惰性创建、parent 为 nullptr（独立顶层窗口）
    WidgetPanel *m_panel = nullptr;        // 组件内容面板，仅组件盒非空；切换成员时整体重建
    BoxHeaderBar *m_header = nullptr;      // 标题栏；隐藏头部时它只是不可见，仍然存在
    MColumnLayout *m_root = nullptr;       // 根列布局：[标题][内容]
    int m_expandedHeight = 320;            // 展开态高度记忆值（px）；折叠时记录、展开时还原
    int m_expandedWidth = 420;             // 展开态宽度记忆值（px）；同上
    BoxListProvider m_boxListProvider;     // 由 App 注入的「全部盒子」查询回调，供组合菜单使用
    QPoint m_dragWindowStart;   // 按下时的窗口位置（拖动用增量，避免逐帧重算）
    QPoint m_dragPressGlobal;   // 按下时的鼠标全局坐标；与当前全局坐标之差即窗口位移
    bool m_dragging = false;    // 标题栏/过滤器拖动进行中；为 true 时 moveEvent 不落盘（松手才写）
    bool m_dragMoved = false;   // 隐藏头部的过滤器路径里：已越过 startDragDistance，松手才算拖动
    bool m_applyingState = false; // setCollapsed 内部保护，防止重入时递归改写状态
    bool m_filtering = false;     // qApp 上是否装了本窗口的事件过滤器（与 m_cfg.hideHeader 同步）
    /**
     * 当前圆角是不是「窗口蒙版」实现的（false = DWM 系统圆角）。
     * 蒙版是按当前窗口尺寸算出来的位图区域，尺寸一变就得重设；DWM 圆角会自己跟着窗口走。
     * 由 applySurface() 每次施加时写入，resizeEvent 据此决定要不要再刷一次表面。
     */
    bool m_usingMask = false;
    /**
     * 缩放落盘的防抖定时器（250ms 单次）。
     * 拖动边框时 resizeEvent 会连续触发几十次，不可能每次都写 JSON；
     * 停手后再落盘一次，保证「拉完的尺寸能存住」。
     */
    QTimer *m_resizeSaveTimer = nullptr;
    /**
     * 折叠/展开的几何动画（懒创建）。
     * 只动宽高、不动位置；内容区在动画期间被钉成固定高度，靠窗口裁切产生「卷帘」效果。
     */
    QPropertyAnimation *m_collapseAnim = nullptr;
    /**
     * 动画进行中。resizeEvent 见到它为 true 就直接返回：
     * 折叠态那个「高度必须等于标题栏高度」的纠偏会把动画第一帧就按回去。
     */
    bool m_animating = false;
};
