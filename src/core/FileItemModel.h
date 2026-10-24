#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QFileIconProvider>
#include <QFileSystemWatcher>
#include <QHash>
#include <QIcon>
#include <QStringList>
#include <QTimer>

/**
 * @file FileItemModel.h
 * 一个整理盒文件夹的列表模型：把真实磁盘目录映射成 QListView 的行，并管排序 / 图标缓存 / 自动刷新。
 *
 * 关键约定：m_entries 的顺序就是界面顺序，所有排序都是对它的重排，没有任何代理模型过滤；
 * 显示 / 隐藏文件、排序方式都直接作用于下一次 rebuild，改完必然整表 reset。
 * 坑：m_manualOrder 存的是「文件名」而不是路径，目录内重命名会让手动顺序丢一项。
 */

/**
 * 一个整理盒文件夹的内容模型（真实磁盘目录）。
 * 支持图标缓存、排序（手动/名称/时间/类型/大小）、隐藏文件、目录变更自动刷新。
 */
class FileItemModel : public QAbstractListModel
{
    Q_OBJECT

public:
    /// 自定义 role；NameRole 故意等于 Qt::DisplayRole，这样默认 delegate 不用额外配置也能显示名字
    enum Roles {
        NameRole = Qt::DisplayRole, // 文件名（不含路径）
        PathRole = Qt::UserRole + 1, // 绝对路径，全模型内部都用它定位文件
        IsDirRole,      // 是否目录
        SizeRole,       // 字节数；目录恒为 0（不递归统计）
        MTimeRole,      // 最后修改时间
        TypeRole,       // 人类可读的类型名，如「文件夹」「PNG 文件」
        IconRole,       // QIcon，按扩展名/路径走缓存
        IsPileRole,   // 是否为「手动堆叠」（.pile 文件夹）
        PileCountRole // 堆里的项目数
    };

    /** @param parent  Qt 父子关系；watcher 与防抖定时器都以 this 为父，随模型一起析构 */
    explicit FileItemModel(QObject *parent = nullptr);
    ~FileItemModel() override;

    /**
     * 切换监听的目录：清理旧监听 → mkpath → 注册 watcher → 清图标缓存 → 立刻 refresh。
     * @param folder  目录路径，可以是 Windows 原生反斜杠形式，内部会 cleanPath + 转 '/'；
     *                传空字符串表示「不监听任何目录」，此时 refresh 直接返回
     * 副作用：会创建目录（不存在时）、清空图标缓存、整个模型 reset。
     */
    void setFolder(const QString &folder);
    /// 当前监听的目录（已 cleanPath 的 '/' 形式）；空表示未设置
    QString folder() const { return m_folder; }

    /**
     * 切换排序方式，并立即重排现有数据。
     * @param mode  0 手动 / 1 名称 / 2 时间 / 3 类型 / 4 大小；其它值等价于「不排序」，只保留文件夹在前的规则
     * 副作用：整表 reset + 发 contentsChanged；空列表时只发信号不 reset。
     */
    void setSortMode(int mode);
    /// 当前排序方式，取值同 setSortMode
    int sortMode() const { return m_sortMode; }

    /**
     * 是否显示隐藏 / 系统文件。
     * @param on  true = 过滤器加上 QDir::Hidden
     * 副作用：会走一次完整 refresh（reset 模型），因为要重新枚举目录。
     */
    void setShowHidden(bool on);
    /// 当前是否显示隐藏文件
    bool showHidden() const { return m_showHidden; }

    /**
     * 注入持久化的手动顺序。
     * @param order  文件名列表（不是路径），顺序即期望的显示顺序；未列出的文件排在已列出项之后
     * 副作用：仅当当前是手动排序且列表非空时才重排并 reset；否则只记住，等切到手动模式再生效。
     */
    void setManualOrder(const QStringList &order);
    /**
     * 按当前视觉顺序导出文件名列表，用于持久化 manualOrder。
     * @return 文件名（不含路径）列表，顺序与界面一致
     */
    QStringList currentOrder() const;

    /**
     * 重新扫描目录并重排（整表 reset）。
     * 目录不存在 / 不是目录时清空列表、发 folderMissing 而不是抛错；
     * 此时 watcher 注册不上，需要外部再次调用本函数才能从「目录被删」恢复。
     */
    void refresh();

    /**
     * @param parent  有有效父索引时返回 0（本模型是平铺列表，不支持子层级）
     * @return         行数，即 m_entries.size()
     */
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    /**
     * @param index  行索引，越界或无效时返回空 QVariant
     * @param role   见 Roles，另支持 Qt::ToolTipRole；未识别的 role 返回空 QVariant
     */
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    /**
     * @param index  行索引；无效索引也会返回可选中/可拖动的基础 flags（不带 DropEnabled）
     * @return         可选中 + 可拖出；仅目录行额外允许拖入（作为放置目标）
     */
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    /// 支持的拖放动作：复制 / 移动 / 建快捷方式
    Qt::DropActions supportedDropActions() const override;
    /// role 名到整型 role 的映射，供 QML 或基于名字取值的场景使用
    QHash<int, QByteArray> roleNames() const override;

    /**
     * @param idx  行索引
     * @return     该行是否为目录；索引无效/越界返回 false
     */
    bool isDirIndex(const QModelIndex &idx) const;
    /**
     * @param idx  行索引
     * @return     该行对应的绝对路径；索引无效/越界返回空字符串
     */
    QString pathAt(const QModelIndex &idx) const;
    /**
     * 按路径反查行索引，两侧都做 cleanPath 后比较。
     * @param path  绝对路径；分隔符混用也能匹配
     * @return      命中返回该行 model index，未命中返回无效 QModelIndex
     */
    QModelIndex indexOfPath(const QString &path) const;
    /// 当前所有条目的绝对路径，顺序与界面一致
    QStringList allPaths() const;
    /// 条目数量（含目录），等价于 rowCount()
    int itemCount() const { return int(m_entries.size()); }

    /**
     * 手动排序：把第 from 行移动到 to 行（to 为插入位置）。
     * to 用的是「移动前」的坐标：to == from 或 to == from + 1 都表示位置没变。
     * 若当前不是手动排序，会先把当前视觉顺序冻结成手动顺序再动。
     * @param from  被移动的行下标，越界直接失败
     * @param to    插入位置，允许等于行数（表示移到末尾）；<0 当作 0，>行数 夹到行数
     * @return      true = 已移动（或本来就在原位）；false = from 越界或 beginMoveRows 被拒
     * 副作用：真移动时发 rowsMoved，从其它排序切过来时会额外整表 reset；成功后 m_sortMode 必为 0。
     */
    bool moveRow(int from, int to);

signals:
    /// 任何一次重建 / 重排 / 目录缺失后都会发出，UI 用它刷新计数、空状态等
    void contentsChanged();
    /// 目录不存在或已不是目录时发出，参数是失效的目录路径；UI 据此提示或重建托管目录
    void folderMissing(const QString &folder);

private:
    /// 模型内部的一行数据；字段都是 rebuild() 时从 QFileInfo 一次性抄下来的快照
    struct Entry
    {
        QString name;       // 文件名（含扩展名），手动顺序也用它匹配
        QString path;       // 绝对路径，唯一标识
        bool isDir = false; // 是否目录；决定排序时是否排在前面
        qint64 size = 0;    // 字节数；目录固定为 0
        QDateTime mtime;    // 最后修改时间，时间排序用它
        QString type;       // 缓存好的类型名（避免每次 data() 都去问 shell）
        bool isPile = false;   // 名字以 .pile 结尾的目录：表示一个手动堆叠
        int pileCount = 0;     // 堆内条目数；非堆恒为 0，统计时包含隐藏项
    };

    /// 重启防抖定时器（不立刻刷新），用于合并一次操作产生的连续目录变更事件
    void scheduleRefresh();
    /// 重新枚举目录填充 m_entries（不排序、不发信号，由调用方包在 begin/endResetModel 里）
    void rebuild();
    /// 按 m_sortMode 就地重排 m_entries；文件夹永远在前，同键时用手动顺序或文件名兜底
    void applySort();
    /// 把当前视觉顺序冻结成手动顺序，并把排序模式切到 0（手动）
    void syncManualOrder();
    /// 取某行的图标，带缓存；缓存 key 见实现处说明
    QIcon iconFor(const Entry &e) const;

    QString m_folder;           // 监听的目录（cleanPath 后的 '/' 形式）；空表示未设置
    int m_sortMode = 0;         // 0-4，含义同 setSortMode；默认手动
    bool m_showHidden = false;  // 是否把隐藏 / 系统项纳入枚举
    QStringList m_manualOrder;  // 手动排序的文件名顺序；只存名字，缺项自动排到末尾
    QVector<Entry> m_entries;   // 当前视图顺序的条目快照，rowCount/data 都直接读它

    QFileSystemWatcher *m_watcher = nullptr;  // 只监听 m_folder 这一层，不递归子目录
    QTimer *m_debounce = nullptr;             // 单次触发，间隔 kDebounceMs
    mutable QFileIconProvider m_iconProvider; // mutable：data()/iconFor() 是 const 但要取图标
    mutable QHash<QString, QIcon> m_iconCache; // 同上；setFolder 时整体清空，避免跨目录串味
};
