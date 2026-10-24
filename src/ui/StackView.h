/**
 * @file StackView.h
 * @brief 「堆叠」视图的两个部件：盒子里那张堆叠卡 StackCard，以及点开后铺开的
 *        StackPopup 弹出网格。
 *
 * 二者共享同一个 FileItemModel 类型但实例独立：StackCard 只读第一个条目用于预览，
 * StackPopup 自己 new 一个模型并复用 FileListView 来完成全部交互（拖放/重命名/删除）。
 * 关键约定：Popup 是无边框置顶的 Tool 窗口，靠 WindowDeactivate 自动关闭；菜单打开
 * 或拖拽期间必须用 m_suppressHide 屏蔽这一逻辑，否则一点菜单窗口就消失。
 */
#pragma once

#include <QWidget>

class FileItemModel;
class FileListView;
class QLabel;

/**
 * 堆叠卡片：整理盒处于「堆叠」视图时显示在盒子里的那一叠。
 * 点击后由 FileBoxWindow 打开 StackPopup 弹出网格。
 *
 * 只做展示与点击上报，自身不实现任何拖放事件：拖入由外层 FileBoxWindow::dropEvent
 * 兜底，拖出/排序等在 StackPopup 的 FileListView 里完成。
 */
class StackCard : public QWidget
{
    Q_OBJECT

public:
    /** @param parent 父控件，通常为 FileBoxWindow 的内容区。 */
    explicit StackCard(QWidget *parent = nullptr);

    /**
     * @brief 绑定要预览的模型（只读）。
     * @param model 外部持有的模型，本卡不接管所有权；为空时按「空 · 拖入文件」绘制。
     */
    void setModel(FileItemModel *model);

    /**
     * @brief 设置预览图标尺寸。
     * @param px 期望边长（px），内部夹到 [24,128]，再按卡片实际高度二次夹取。
     */
    void setIconSize(int px);

signals:
    /** @brief 左键在卡片内释放。上层据此弹出 StackPopup。 */
    void clicked();

protected:
    /** @brief 自绘卡片堆：背景层数 = min(3, 条目数)，最前面一张画首项图标与名称。 */
    void paintEvent(QPaintEvent *e) override;
    /** @brief 仅左键且落点在 rect 内才发 clicked()，避免拖出窗口后误触。 */
    void mouseReleaseEvent(QMouseEvent *e) override;
    /** @brief 进入即置 m_hover，最前面那张卡的描边会变强调色。 */
    void enterEvent(QEnterEvent *e) override;
    /** @brief 离开复位 hover 并重绘。 */
    void leaveEvent(QEvent *e) override;

private:
    FileItemModel *m_model = nullptr;  // 仅用于读取首项与总数；不拥有所有权，可为 nullptr
    int m_iconSize = 64;               // 预览图标期望边长（px），有效范围 [24,128]，默认 64
    bool m_hover = false;              // 鼠标是否悬停在本卡上，决定最前层描边用强调色还是普通边框色
};

/**
 * 弹出式展开窗口：把堆叠里的文件按 3×3 / 5×5 网格铺开。
 * 支持拖出、打开、重命名、删除；失去焦点或 Esc 关闭。
 *
 * 内部就是一个 FileListView，因此这条链上的拖放/快捷键行为与整理盒主视图完全一致，
 * 区别只在于模型是临时建的、文件夹由 showNear() 传进来。
 */
class StackPopup : public QWidget
{
    Q_OBJECT

public:
    /** @param parent 传 nullptr 即可：窗口带 Qt::Tool 标志，生命周期由 FileBoxWindow 手动管理。 */
    explicit StackPopup(QWidget *parent = nullptr);
    ~StackPopup() override;

    /**
     * @brief 绑定数据源并计算尺寸，然后在 anchor 附近弹出。
     * @param anchorGlobal 触发者（堆叠卡）的全局矩形，用于决定弹在它上方还是下方。
     * @param title 标题文字；过长会按窗口宽度中部省略，原始文本不会被保存。
     * @param folder 要展示的磁盘目录（绝对路径）。
     * @param gridCols 期望列数，只认 5，其余一律按 3 处理。
     * @param iconSize 图标边长（px），透传给委托夹取。
     * @param nameLines 文件名行数，透传给委托夹到 [1,2]。
     * @param sortMode 模型排序方式：0=手动，1=名称，2=时间，3=类型，4=大小。
     *
     * 会重置模型文件夹（触发一次重建），因此每次弹出都是新内容。
     */
    void showNear(const QRect &anchorGlobal, const QString &title, const QString &folder,
                  int gridCols, int iconSize, int nameLines, int sortMode);

    /**
     * @brief 外部把文件移入/移出后，重新读盘并重算窗口尺寸。
     *
     * 顺序固定为先 refresh 再 rebuildSize：条目数变了窗口大小也要跟着变，
     * 反过来的话会按旧数量算出一个不合身的尺寸。
     */
    void refresh();

signals:
    /** @brief 弹窗内发生了文件增删（删除/拖入），上层需刷新盒子并保存。 */
    void filesMutated();

protected:
    /** @brief 只画一层玻璃底板，内容全部由子控件负责。 */
    void paintEvent(QPaintEvent *e) override;
    /** @brief Esc 关闭窗口；其余按键交回基类（输入焦点通常在 FileListView 上，键盘交互由它自己处理）。 */
    void keyPressEvent(QKeyEvent *e) override;
    /** @brief 监听 WindowDeactivate，延时自动关闭（菜单/拖拽期间由 m_suppressHide 屏蔽）。 */
    bool event(QEvent *e) override;
    /** @brief 尺寸变化后重设 12px 圆角窗口遮罩。 */
    void resizeEvent(QResizeEvent *e) override;

private:
    /** @brief 按当前条目数与列数固定窗口尺寸，并刷新标题省略与「N 项」计数。 */
    void rebuildSize();
    /**
     * @brief 构建并弹出条目右键菜单。
     * @param path 被右键的条目绝对路径（菜单动作都以它为目标）。
     * @param globalPos 弹出位置（全局坐标）。
     *
     * 只作用于单个条目；批量删除用的是 m_view->selectedPaths()，两者可能不一致。
     */
    void buildContextMenu(const QString &path, const QPoint &globalPos);

    FileItemModel *m_model = nullptr;   // 弹窗私有的模型，父对象为 this；每次 showNear 换 folder
    FileListView *m_view = nullptr;     // 复用主视图实现，父对象为 this
    QLabel *m_title = nullptr;          // 标题，左对齐并占满剩余宽度
    QLabel *m_count = nullptr;          // 「N 项」计数，位于标题右侧、关闭按钮左侧
    int m_gridCols = 3;                 // 每行列数，取值仅 3 或 5；同时决定窗口宽度
    int m_iconSize = 64;                // 透传给委托的图标边长（px），实际生效值由委托夹取
    bool m_sized = false;               // 历史遗留字段，当前没有任何读写点（不影响行为）
    bool m_suppressHide = false;        // true 时忽略 WindowDeactivate 自动关闭，菜单/拖拽期间置为 true
};
