/**
 * @file FileListView.h
 * @brief 整理盒内容区的主视图（QListView 子类），承载图标 / 列表两种排布模式。
 *
 * 除展示外，本类集中处理四类交互：从资源管理器拖入（默认复制、Shift 移动）、拖出到
 * 资源管理器或另一个盒子、盒内拖动排序（会顺带把模型切到手动排序）、拖到文件夹图标上
 * 视为移入该文件夹。另有 F2 内联重命名、Delete 移入回收站、Enter / 双击打开、右键菜单。
 *
 * 关键约定：本类只负责改磁盘 + 发 contentsMutated()，配置（viewMode / iconSize）的
 * 读写落盘由 FileBoxWindow 完成；「堆叠」视图（viewMode 2）不归本类渲染，调用方必须
 * 把它折叠成 0 再传进来（见 FileBoxWindow::applyAppearance）。
 */
#pragma once

#include <QListView>

class FileItemModel;
class FileItemDelegate;
class QLineEdit;

/**
 * 整理盒内容视图：图标/列表两种模式，支持
 *  - 从资源管理器拖入（默认复制，Shift 移动）
 *  - 拖出到资源管理器（Windows 默认移动/复制判定）
 *  - 内部拖动排序（自动切到手动排序）
 *  - 拖到文件夹图标上 = 移动到该文件夹
 *  - F2 内联重命名、Delete 删除、Enter 打开
 *
 * 视图本身不持有「当前文件夹」：文件夹由 FileItemModel 管，本类只转发路径。
 */
class FileListView : public QListView
{
    Q_OBJECT

public:
    /**
     * @brief 构造视图并完成一次性样式/交互配置。
     * @param parent 父控件，随父控件析构；通常为 FileBoxWindow。
     *
     * 构造时即创建并安装 FileItemDelegate（本类所有尺寸计算都依赖它，故不可为 null）。
     * 编辑触发器被设为 NoEditTriggers：重命名走自绘的 QLineEdit（见 beginRename），
     * 不走 Qt 原生编辑器，否则会盖掉委托画的选中底色。
     */
    explicit FileListView(QWidget *parent = nullptr);

    /**
     * @brief 绑定数据模型与盒子 ID，二者成对出现，缺一不可。
     * @param model 已有的 FileItemModel，本类接管其所有权（setParent 到 this）。
     * @param boxId 盒子唯一 ID，仅用于内部拖放的 MIME 载荷，判断「是不是从同一个盒子拖来的」。
     *
     * 会连接 model 的 contentsChanged：既刷新网格，也向上发 contentsMutated()。
     * 重复调用会把模型换成新的，旧模型仍挂在 this 下但不再收到本类的刷新逻辑。
     */
    void attach(FileItemModel *model, const QString &boxId);

    /** @return 当前绑定的模型；attach() 之前为 nullptr。 */
    FileItemModel *fileModel() const { return m_model; }

    /**
     * @brief 应用外观参数，同时驱动委托与视图自身的排布模式。
     * @param viewMode 0=图标网格（自由移动、自动换行），1=列表（固定行高、纵向滚动）。
     *                 传入 2（堆叠）会掉进 else 分支按列表处理，调用方需自行折叠成 0/1。
     * @param iconSize 图标边长（px），委托内部会夹到 [24,192]。
     * @param nameLines 文件名最多显示行数，委托内部夹到 [1,2]。
     *
     * 副作用：会重设 gridSize 并 doItemsLayout()（强制重新排布所有条目）。
     */
    void applyAppearance(int viewMode, int iconSize, int nameLines);

    /** @return 委托记录的视图模式 0/1（不是 QListView::viewMode()，后者枚举值不同）。 */
    int viewMode() const;

    /** @return 委托当前使用的图标边长（px），已含 [24,192] 的夹取。 */
    int iconSizeValue() const;

    /**
     * @return 当前模式下的单元格尺寸（px）；图标模式按图标边长与名称行数算出，列表模式固定。
     *
     * 与 FileItemDelegate::cellSize() 必须是同一个值：视图拿它设 gridSize，委托拿它回答
     * sizeHint，两边分叉就会出现条目被拉伸 / 点击热区偏移。
     * 委托缺失时退回 94×126 兜底常量（构造后委托必然存在，正常路径不会命中）。
     */
    QSize gridCellSize() const;

    /**
     * @brief 收集当前选中项对应的绝对路径。
     * @return 本地路径列表，顺序沿用 selectionModel()->selectedIndexes()（通常即模型行序）；
     *         未 attach、无选择模型或未选中时返回空列表。
     *
     * 依赖 m_model 反查路径，索引失效的项会被跳过（结果可能少于选中项数）。
     */
    QStringList selectedPaths() const;

    /**
     * @brief 在指定单元格上浮出内联重命名编辑器。
     * @param idx 目标项索引；无效、越界或对应路径为空时直接返回，不做任何事。
     *
     * 只预选主文件名（不含最后一个 '.' 之后的部分），点号在首位时（隐藏文件）全选。
     */
    void beginRename(const QModelIndex &idx);

    /** @brief 对当前索引调用 beginRename()；由 F2 触发，无当前项时静默失败。 */
    void beginRenameOnCurrent();

signals:
    /** @brief 请求上层用默认程序/资源管理器打开该路径；双击与 Enter 都会发。 */
    void openRequested(const QString &path);
    /** @brief 在某个条目上右键。@param globalPos 全局坐标，供 popup 定位。 */
    void contextMenuOnItem(const QString &path, const QPoint &globalPos);
    /** @brief 在空白处右键，此时已 clearSelection()。@param globalPos 全局坐标。 */
    void contextMenuOnEmpty(const QPoint &globalPos);
    /**
     * @brief 文件被移动/重命名/删除后发出：上层需要重新读盘、刷新盒子并保存（含手动顺序）。
     *
     * 两条来源：① attach() 把模型的 contentsChanged 直接连到本信号；② 本类在落盘动作后
     * 手动 emit。因为动作里通常先 refresh() 再手动 emit，同一次操作会发两次，
     * 接收方必须按幂等处理（写配置、重建 UI 都不能假设「只发一次」）。
     */
    void contentsMutated();
    /** @brief Ctrl+滚轮改变了图标尺寸，取值已夹在 [32,160]；上层应写回配置。 */
    void iconSizeWheeled(int newSize);

protected:
    /**
     * @brief 自建 QDrag 发起拖出。
     * @param supportedActions 基类传入的允许动作，此处被忽略——实际固定导出
     *        Copy|Move|Link 三种，由系统在落点决定，默认 Move。
     *
     * MIME 同时携带 file:// URL（给资源管理器）和内部 JSON（给另一个整理盒，
     * 含 boxId 以识别来源）。拖拽结束后无条件 refresh()：外部可能已把文件移走。
     */
    void startDrag(Qt::DropActions supportedActions) override;
    /** @brief 判断能否接收：内部格式或带 URL 都放行，内部格式预设 Move，其余 Copy。 */
    void dragEnterEvent(QDragEnterEvent *e) override;
    /** @brief 拖动中实时改写落点动作，并在悬停文件夹时切换当前项做高亮提示。 */
    void dragMoveEvent(QDragMoveEvent *e) override;
    /** @brief 落点分流：同盒内排序 / 移入文件夹 / 复制或移动进当前目录。 */
    void dropEvent(QDropEvent *e) override;
    /** @brief 命中条目则发 openRequested()，空白处交回基类（用于框选等默认行为）。 */
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    /** @brief 条目上右键 → contextMenuOnItem；空白处右键 → 先清选区再 contextMenuOnEmpty。 */
    void contextMenuEvent(QContextMenuEvent *e) override;
    /**
     * @brief 键盘快捷键：F2 重命名、Delete 删到回收站（受 confirmDelete 控制是否二次确认）、
     *        Enter/Return 打开、Esc 取消重命名；其余按键交回基类做光标移动/框选。
     */
    void keyPressEvent(QKeyEvent *e) override;
    /** @brief 仅在列表模式下重算 gridSize——列表行宽要跟随视口宽度。 */
    void resizeEvent(QResizeEvent *e) override;
    /** @brief Ctrl+滚轮调节图标尺寸（步长 8px，夹在 [32,160]），否则走基类滚动。 */
    void wheelEvent(QWheelEvent *e) override;
    /** @brief 失焦即提交重命名；避免编辑器留在界面上但用户已去点别处。 */
    void focusOutEvent(QFocusEvent *e) override;
    /**
     * @brief 只拦截重命名编辑器的 Esc / Enter 键。
     * @param obj 事件目标；只有 obj == m_editor 时才处理。
     * @param e 待过滤事件。
     * @return true 表示已消费（Esc 取消、Enter 提交）；其余情况返回基类结果。
     */
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    /**
     * @brief 提交重命名：取编辑器文本并真正改磁盘。
     *
     * 名字为空、或与原文件名相同则只关闭编辑器不改盘；改名失败弹一次模态警告。
     * 无论成败都会 refresh() 并 emit contentsMutated()（幂等，代价可接受）。
     * 非重命名态调用是安全的空操作（靠 m_renaming 守门）。
     */
    void commitRename();
    /** @brief 放弃重命名：仅隐藏编辑器、复位 m_renaming，不动磁盘。 */
    void cancelRename();
    /**
     * @brief 把拖放点换算成模型插入行号（0..rowCount，右端为追加）。
     * @param pos 视口坐标系下的落点。
     * @return 插入位置；模型为空或未 attach 时返回 0。
     *
     * 命中某一项时以该项的纵向中线为界：上半 → 插到它前面，下半 → 插到它后面。
     * 落在所有项的包围盒之外时，退化为「找中心点最近的项」再按同样规则判断。
     */
    int insertRowAt(const QPoint &pos) const;
    /**
     * @brief 按当前模式重设 gridSize 并重排所有项。
     *
     * 图标模式用委托的固定单元尺寸；列表模式宽度取 max(120, 视口宽)，
     * 让整行可点且右侧元信息有地方放。
     */
    void updateGrid();

    FileItemModel *m_model = nullptr;      // 已 attach 的数据模型，同时是子对象；nullptr = 尚未绑定
    FileItemDelegate *m_delegate = nullptr; // 自绘委托，构造时创建并 setItemDelegate，生命周期随 this
    QString m_boxId;                       // 所属盒子 ID，写入内部拖拽 MIME；空串表示未绑定。"popup" 是 StackPopup 的固定占位值，用于让弹窗内的拖动走「同盒排序」
    QLineEdit *m_editor = nullptr;         // 懒创建的内联重命名编辑器，父对象是 viewport()
    QModelIndex m_renameIndex;             // 发起重命名时的索引；当前只写不读，改盘一律以 m_renamePath 为准
    QString m_renamePath;                  // 发起重命名时的原路径，改盘时以它为准（索引可能已失效）
    bool m_renaming = false;               // 编辑器是否处于「正在重命名」态，commit/cancel 的守门标志
};
