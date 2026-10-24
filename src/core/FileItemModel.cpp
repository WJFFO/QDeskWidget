#include "core/FileItemModel.h"
#include "core/ShellActions.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <algorithm>
#include <QTimer>

// FileItemModel 实现：把 QDir 枚举结果做成「快照 + 整表 reset」的列表模型。
// 数据流：directoryChanged → 防抖 → refresh() → rebuild() 抄快照 → applySort() 排序 → 发信号。
// 坑：所有 refresh/rebuild 都走 beginResetModel，会丢失选中项与滚动位置，
// 所以不要把它接在高频信号上（已用 kDebounceMs 合并目录事件）。

namespace
{
// 目录变更事件的合并窗口：一次复制/解压会连续触发几十次 directoryChanged，
// 350ms 足够把这些抖动收成一次 rebuild，又不至于让用户感觉到延迟。
constexpr int kDebounceMs = 350;
}

// 构造只搭好「监听 + 防抖」管线，不加载任何数据；真正的内容要等 setFolder()。
FileItemModel::FileItemModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &FileItemModel::scheduleRefresh);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &FileItemModel::refresh);
}

// watcher / 定时器都以 this 为父，Qt 会自己回收，这里不需要手写 delete。
FileItemModel::~FileItemModel() = default;

// 切目录时先摘掉所有旧监听（watcher 里可能还留着已被删掉的路径），再挂新目录。
// 目录不存在时先 mkpath 出来，否则 watcher 注册不上、界面也没内容可显示。
// 先 cleanPath 再比较，避免 "C:/a" 与 "C://a" 被当成两个目录反复重建。
void FileItemModel::setFolder(const QString &folder)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    if (clean == m_folder)
        return;

    if (!m_folder.isEmpty()) {
        const QStringList watched = m_watcher->directories();
        if (!watched.isEmpty())
            m_watcher->removePaths(watched);
    }
    m_folder = clean;

    if (!m_folder.isEmpty()) {
        QDir().mkpath(m_folder);
        m_watcher->addPath(m_folder); // 放到 mkpath 之后：目录不存在时 addPath 会直接失败
    }
    m_iconCache.clear(); // 缓存里有按完整路径做 key 的项，换目录后必须整体丢弃
    refresh();
}

// 只重排现有快照，不重新扫盘：排序方式变化不影响目录内容。
void FileItemModel::setSortMode(int mode)
{
    if (mode == m_sortMode)
        return;
    m_sortMode = mode;
    if (m_entries.isEmpty()) {
        emit contentsChanged();
        return;
    }
    beginResetModel();
    applySort();
    endResetModel();
    emit contentsChanged();
}

// 隐藏文件是枚举阶段的过滤器，所以必须整表重建，不能在快照上过滤。
void FileItemModel::setShowHidden(bool on)
{
    if (on == m_showHidden)
        return;
    m_showHidden = on;
    refresh();
}

// 非手动排序时先只记下来：此刻重排会被当前排序规则覆盖，等真正切到手动模式时才会用上。
void FileItemModel::setManualOrder(const QStringList &order)
{
    m_manualOrder = order;
    if (m_sortMode == 0 && !m_entries.isEmpty()) {
        beginResetModel();
        applySort();
        endResetModel();
    }
}

// 导出的是「当前这一屏的顺序」，所以直接遍历已排好序的 m_entries，
// 无需再按 m_manualOrder 拼装（那只是持久化副本）。
QStringList FileItemModel::currentOrder() const
{
    QStringList out;
    out.reserve(m_entries.size());
    for (const Entry &e : m_entries)
        out << e.name;
    return out;
}

// 重启而非启动定时器：连续事件总是再等一个完整窗口，避免大批量拷贝时刷新个没完。
void FileItemModel::scheduleRefresh()
{
    if (m_debounce)
        m_debounce->start();
}

// 两种失败路径分开处理：目录没了就清空并报 folderMissing，让上层决定提示还是重建目录。
void FileItemModel::refresh()
{
    if (m_folder.isEmpty())
        return;
    QFileInfo dirInfo(m_folder);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        beginResetModel();
        m_entries.clear();
        endResetModel();
        emit contentsChanged();
        emit folderMissing(m_folder); // 注意这里直接 return，watcher 不会再被自动挂上
        return;
    }
    // 目录可能被重建，重新注册监听
    if (!m_watcher->directories().contains(m_folder))
        m_watcher->addPath(m_folder);

    beginResetModel();
    rebuild();
    applySort();
    endResetModel();
    emit contentsChanged();
}

// 一次性把 QFileInfo 里需要的东西抄进 Entry，之后 data() 就不再碰磁盘。
// filters 用 NoSort：排序全部交给 applySort，保证「文件夹在前」这类规则只有一处实现。
void FileItemModel::rebuild()
{
    m_entries.clear();
    QDir::Filters filters = QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot;
    if (m_showHidden)
        filters |= QDir::Hidden;

    const QFileInfoList list = QDir(m_folder).entryInfoList(filters, QDir::NoSort);
    m_entries.reserve(list.size());
    for (const QFileInfo &fi : list) {
        Entry e;
        e.name  = fi.fileName();
        e.path  = fi.absoluteFilePath();
        e.isDir = fi.isDir();
        e.size  = e.isDir ? 0 : fi.size(); // 目录不递归求大小，否则遇到大目录会卡住界面
        e.mtime = fi.lastModified();
        e.type  = ShellActions::typeName(e.path, e.isDir);
        if (e.isDir && e.name.endsWith(QStringLiteral(".pile"), Qt::CaseInsensitive)) {
            e.isPile = true;
            // 堆内计数固定包含隐藏/系统项：否则「打开堆才看见 3 个、这里显示 2 个」会不一致
            const QFileInfoList inner = QDir(e.path).entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);
            e.pileCount = int(inner.size());
            e.type = QStringLiteral("堆 · %1 项").arg(e.pileCount); // 覆盖掉 typeName 给的「文件夹」
        }
        m_entries.push_back(e);
    }
}

// 排序规则一览（文件夹永远排在同组最前，见 lambda 开头）：
//   0 手动：在 manualOrder 里的按记录顺序；不在的排在后面、彼此按文件名；
//   1 名称：localeAwareCompare，中文按系统语言的拼音/笔画顺序而非字节序；
//   2 时间：新的在前（降序）；
//   3 类型：先比小写扩展名，同扩展名再比名字；
//   4 大小：大的在前（降序），同样大小再比名字。
// 用 stable_sort 是为了让「相等」的项保持上一次的顺序，避免刷新后视觉跳动。
void FileItemModel::applySort()
{
    // 文件夹始终在前
    const bool manual = (m_sortMode == 0);

    // 手动模式预建 文件名→序号 的哈希，避免比较函数里每次都线性查 manualOrder。
    QHash<QString, int> orderIndex;
    if (manual) {
        orderIndex.reserve(m_manualOrder.size());
        for (int i = 0; i < m_manualOrder.size(); ++i)
            orderIndex.insert(m_manualOrder.at(i), i);
    }

    std::stable_sort(m_entries.begin(), m_entries.end(), [&](const Entry &a, const Entry &b)
    {
        if (a.isDir != b.isDir)
            return a.isDir;

        switch (m_sortMode) {
        case 0: { // 手动
            const bool ha = orderIndex.contains(a.name);
            const bool hb = orderIndex.contains(b.name);
            if (ha != hb)
                return ha; // 已记录的排在未记录的前面（新出现的文件自然落到末尾）
            if (ha && hb)
                return orderIndex.value(a.name) < orderIndex.value(b.name);
            return QString::localeAwareCompare(a.name, b.name) < 0;
        }
        case 1:
            return QString::localeAwareCompare(a.name, b.name) < 0;
        case 2:
            return a.mtime > b.mtime;
        case 3: {
            // 只取后缀、不查 MIME：省一次 shell 调用，且对「一堆同名不同扩展名」的文件足够直观
            const QString ea = QFileInfo(a.name).suffix().toLower();
            const QString eb = QFileInfo(b.name).suffix().toLower();
            if (ea != eb)
                return ea < eb;
            return QString::localeAwareCompare(a.name, b.name) < 0;
        }
        case 4:
            if (a.size != b.size)
                return a.size > b.size;
            return QString::localeAwareCompare(a.name, b.name) < 0;
        default:
            return false; // 未知模式：不参与比较，只保留上面「文件夹在前」的效果
        }
    });
}

// 冻结当前视觉顺序为手动顺序：不重新排序，仅把现状记下来，改完排序模式就变了。
void FileItemModel::syncManualOrder()
{
    m_manualOrder = currentOrder();
    m_sortMode = 0;
}

// 平铺模型：任何有效父索引都说明调用方在问子层级，直接给 0。
int FileItemModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_entries.size());
}

// 图标缓存 key 的三档策略：
//   - 目录统一用 "::dir"（同名冒号前缀不可能与扩展名冲突）；
//   - exe / lnk / ico 用完整路径，因为每个可执行文件的图标都不一样；
//   - 其余文件用小写扩展名，同类型共用一个图标。
// 对 Windows 来说每次取 shell 图标都是跨进程开销，所以这里必须缓存。
QIcon FileItemModel::iconFor(const Entry &e) const
{
    const QString key = e.isDir ? QStringLiteral("::dir")
                                : (e.path.endsWith(".exe", Qt::CaseInsensitive)
                                   || e.path.endsWith(".lnk", Qt::CaseInsensitive)
                                   || e.path.endsWith(".ico", Qt::CaseInsensitive)
                                       ? e.path
                                       : QFileInfo(e.path).suffix().toLower());
    auto it = m_iconCache.constFind(key);
    if (it != m_iconCache.constEnd())
        return it.value();

    const QIcon icon = m_iconProvider.icon(QFileInfo(e.path));
    m_iconCache.insert(key, icon);
    return icon;
}

// 统一的越界防御在这里做一次，下面各分支就不用再判了。
// 注意 Qt::DisplayRole 与 NameRole 同值，所以这里只需要写一次 case。
QVariant FileItemModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};

    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole: // 与 NameRole 同值
        return e.name;
    case PathRole:
        return e.path;
    case IsDirRole:
        return e.isDir;
    case SizeRole:
        return e.size;
    case MTimeRole:
        return e.mtime;
    case TypeRole:
        return e.type;
    case IsPileRole:
        return e.isPile;
    case PileCountRole:
        return e.pileCount;
    case IconRole:
        return iconFor(e);
    case Qt::ToolTipRole:
        // 堆显示「名称 \n 堆 · N 项」，普通项显示「名称 \n 文件夹/大小」，鼠标悬停即可看全名
        if (e.isPile)
            return e.name + QLatin1Char('\n') + QStringLiteral("堆 · %1 项").arg(e.pileCount);
        return e.name + QLatin1Char('\n')
               + (e.isDir ? QStringLiteral("文件夹") : ShellActions::humanSize(e.size));
    default:
        return {};
    }
}

// 无效索引（例如拖到空白处）也给基础 flags，但只有目录行能当放置目标，
// 拖进目录就交给 FileListView 走「移动到子目录」分支。
Qt::ItemFlags FileItemModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.isValid() && index.row() < m_entries.size() && m_entries.at(index.row()).isDir)
        f |= Qt::ItemIsDropEnabled;
    return f;
}

// 声明支持三种动作，实际用哪种由 FileListView 根据修饰键与来源决定。
Qt::DropActions FileItemModel::supportedDropActions() const
{
    return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
}

// 供 QML / 按名字取值的场景使用；改 role 名等于改对外接口，要同步检查调用方。
QHash<int, QByteArray> FileItemModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { PathRole, "path" },
        { IsDirRole, "isDir" },
        { SizeRole, "size" },
        { MTimeRole, "mtime" },
        { TypeRole, "type" },
        { IconRole, "icon" },
        { IsPileRole, "isPile" },
        { PileCountRole, "pileCount" }
    };
}

// 三个查询函数共用同一套边界检查模板：无效/越界返回「空值」，不抛异常也不断言。
bool FileItemModel::isDirIndex(const QModelIndex &idx) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_entries.size())
        return false;
    return m_entries.at(idx.row()).isDir;
}

// 返回绝对路径而不是 Entry 引用，外部调用方不用关心模型内部的序号稳定性。
QString FileItemModel::pathAt(const QModelIndex &idx) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_entries.size())
        return {};
    return m_entries.at(idx.row()).path;
}

// 两侧都 cleanPath，能容忍多余分隔符和 ./.. 这类写法；只比较路径字符串，不解析软链。
QModelIndex FileItemModel::indexOfPath(const QString &path) const
{
    const QString clean = QDir::cleanPath(path);
    for (int i = 0; i < m_entries.size(); ++i)
        if (QDir::cleanPath(m_entries.at(i).path) == clean)
            return index(i, 0);
    return {};
}

// 与 currentOrder() 的区别：这里给的是绝对路径，用于「全选 / 批量删除」之类要操作磁盘的场景。
QStringList FileItemModel::allPaths() const
{
    QStringList out;
    out.reserve(m_entries.size());
    for (const Entry &e : m_entries)
        out << e.path;
    return out;
}

// from/to 归一化后先处理「本来就在原位」：to == from 与 to == from + 1 在插入坐标里
// 等价于没动，此时只需保证排序模式切到手动（否则用户拖一下会感觉排序被重置了）。
// 真正的移动必须让 beginMoveRows 的 destinationChild 用「移动前」的坐标，
// 所以移动完成后要把 to 减 1 才得到 takeAt 之后的插入下标。
bool FileItemModel::moveRow(int from, int to)
{
    if (from < 0 || from >= m_entries.size())
        return false;
    if (to < 0)
        to = 0;
    if (to > m_entries.size())
        to = m_entries.size();

    if (to == from || to == from + 1) {
        if (m_sortMode != 0) {
            beginResetModel();
            syncManualOrder();
            endResetModel();
        }
        return true;
    }

    if (m_sortMode != 0) {
        // 从其它排序方式切到手动：先冻结当前视觉顺序
        // （syncManualOrder 不改 m_entries，所以下面 from/to 的行号仍然有效）
        beginResetModel();
        syncManualOrder();
        endResetModel();
    }

    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), to))
        return false;
    const Entry e = m_entries.takeAt(from);
    const int dest = (to > from) ? to - 1 : to; // 取出元素后，右侧的下标整体前移了一位
    m_entries.insert(dest, e);
    endMoveRows();

    // 移动结果必须回写成手动顺序，否则下次 refresh 重排会把刚拖好的位置丢掉。
    syncManualOrder();
    return true;
}
