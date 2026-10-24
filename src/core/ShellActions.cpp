// -----------------------------------------------------------------------------
// ShellActions —— 文件系统 / Windows Shell 操作实现（ShellActions.h 的实现文件）。
// 职责：批量复制/移动/删除的递归实现、同名避让、回收站、剪贴板与体积格式化。
// 约定：批处理函数一律「尽力而为」——单项失败不打断整体，失败项收集后统一返回，
//       由调用方决定提示文案（本文件不弹任何 UI）。
// 坑1：删除时遇到符号链接必须按文件删，绝不能递归进去，否则会删到链接指向的真实内容。
// 坑2：QFile::rename 只能在同一卷内成功，跨盘要「复制 + 删源」，中途失败会留下半份拷贝。
// -----------------------------------------------------------------------------
#include "core/ShellActions.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QUrl>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace
{
// 进程内剪贴板：只在本程序内部「复制/剪切 → 粘贴」时有效，不经过系统剪贴板。
// 用全局变量而不是单例类，是因为这里只有两份状态、没有生命周期需要管理。
// 未加锁：所有读写都来自 GUI 线程（右键菜单/粘贴），新增调用方时不要从工作线程碰它们。
QStringList g_clipboard;   // 上次复制/剪切选中的绝对路径列表（按原样保存，不做归一化）
bool g_clipboardCut = false; // true = 剪切语义（粘贴成功后要删源）；false = 复制

/**
 * 递归删除（不进回收站，不可恢复）
 *
 * @param path 目标路径，文件/目录/链接均可。
 * @return 是否完全删除成功；目录下任意子项删不掉就整体返回 false（已删的部分不会回滚）。
 *
 * 目录分支要求目录先被清空才能 rmdir，所以必须自底向上删。
 * 符号链接按普通文件删除（QFile::remove 只删链接本身），避免顺藤摸瓜删掉链接目标。
 */
bool removeRecursively(const QString &path)
{
    QFileInfo fi(path); // 只查一次类型：递归中每层都要判「目录还是文件」
    if (fi.isDir() && !fi.isSymLink()) {
        QDir d(path);
        // 必须显式带上 Hidden/System：默认过滤器会漏掉隐藏文件，
        // 导致 rmdir 因「目录非空」失败，看起来像"删不掉"。
        const QFileInfoList entries = d.entryInfoList(QDir::AllEntries | QDir::Hidden
                                                      | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo &e : entries) {
            if (!removeRecursively(e.absoluteFilePath()))
                return false;
        }
        // 两段式兜底：先用当前 QDir 句柄删，失败再用一个新建的 QDir 重试一次。
        // 路径是绝对路径时两次调用等价，第二次纯粹是「再试一次」，不改语义。
        return d.rmdir(path) || QDir().rmdir(path);
    }
    return QFile::remove(path);
}

/**
 * 递归复制，保留时间戳；目标存在时自动改名
 *
 * @param src   源路径（文件或目录）。
 * @param dstIn 期望的目标路径（含文件名）。
 * @return 是否全部复制成功。
 *
 * 目标名不是「直接用 dstIn」，而是每次都过一遍 uniqueTargetPath：批内重名、
 * 以及复制到已有同名文件时都不会覆盖用户数据。副作用是递归子项也各自避让，
 * 因此目标目录内的结构与源可能不是严格一致（只有在冲突时才会不同）。
 */
bool copyRecursively(const QString &src, const QString &dstIn)
{
    const QString dst = ShellActions::uniqueTargetPath(QFileInfo(dstIn).absolutePath(),
                                                       QFileInfo(dstIn).fileName());
    QFileInfo si(src);
    if (si.isDir() && !si.isSymLink()) {
        QDir().mkpath(dst); // 目录先建出来，再逐个复制子项
        QDir sd(src);       // 用 QDir 取子项：带 Hidden/System 过滤，隐藏文件不会被漏拷
        const QFileInfoList entries = sd.entryInfoList(QDir::AllEntries | QDir::Hidden
                                                       | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo &e : entries) {
            if (!copyRecursively(e.absoluteFilePath(), dst + QLatin1Char('/') + e.fileName()))
                return false;
        }
        return true;
    }
    if (!QFile::copy(src, dst))
        return false;
    QFile f(dst); // 保留修改时间
    // 注意：这里只是构造了 QFile，并没有调用任何设置时间戳的接口。
    // 修改时间是否保留完全取决于 QFile::copy 的平台行为，不要据此依赖时间戳一致。
    return true;
}

/**
 * 移动单个条目到目录 dstDir（自动避让同名）
 *
 * @param src    源路径。
 * @param dstDir 目标目录（必须已存在，调用方负责 mkpath）。
 * @return 是否成功。
 *
 * 先尝试 QFile::rename（同卷下是原子的、瞬间完成）；失败通常意味着跨卷，
 * 退化成「复制 + 删源」。后者的非原子性意味着中途失败会同时留下源和半份目标，
 * 此时返回 false，但源文件仍在（数据不会丢，只是白复制了一次）。
 */
bool moveOne(const QString &src, const QString &dstDir)
{
    // 先算避让后的目标名。注意 QFile::rename 不覆盖已有文件：若这期间目标被别人占用，
    // rename 会失败并落到下面的「复制 + 删源」分支，那里会再避让一次名字
    const QString target = ShellActions::uniqueTargetPath(dstDir, QFileInfo(src).fileName());
    if (QFile::rename(src, target))
        return true;
    // 跨卷：先复制再删除源
    if (!copyRecursively(src, target))
        return false;
    return removeRecursively(src);
}
} // namespace

namespace ShellActions
{

// 只换分隔符，不做绝对化、不 cleanPath：「相对路径仍是相对路径」这一点对 explorer /select 很关键。
QString nativePath(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

// 空路径提前拦掉：调用方（右键菜单/拖拽）在无选中项时也会走到这里，
// 及早返回 false 比让 QUrl 去构造一个没有意义的空 URL 更明确。
bool open(const QString &path)
{
    if (path.isEmpty())
        return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// 用 explorer.exe /select 定位：这是唯一能在资源管理器里「选中」文件的官方途径。
// startDetached 是因为 explorer 会把命令转交给已运行的实例后立刻退出，
// 用 start 等待没有意义，还可能卡住调用线程。
void revealInExplorer(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString native = nativePath(path);
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,") + QDir::toNativeSeparators(native)});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

// 打开文件夹：不做存在性校验，路径不存在时由系统弹自己的错误提示（这里没有 UI 上下文可用）
bool openFolder(const QString &dir)
{
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// 回收站走 SHFileOperationW 而不是 Qt：Qt 没有回收站 API，直接 QFile::remove 是不可恢复的硬删。
bool moveToRecycleBin(const QStringList &paths, QStringList *failed)
{
    if (paths.isEmpty())
        return true;
#ifdef Q_OS_WIN
    // SHFileOperationW 需要双 null 结尾的路径列表
    // 即 "a\0b\0\0"：每项自己结尾，整个列表再补一个空项表示结束。
    // 少写最后一个 \0 会让 API 越过缓冲区继续读，属于典型的野指针崩溃。
    QString buf; // 待删除路径的「双 null 结尾」序列；utf16() 复用这块内存，不能中途再改 buf
    for (const QString &p : paths) {
        buf += QDir::toNativeSeparators(p);
        buf += QLatin1Char('\0');
    }
    buf += QLatin1Char('\0');

    SHFILEOPSTRUCTW op = {}; // 必须清零：fAnyOperationsAborted 等字段是出参，靠初值区分「没发生」
    op.wFunc = FO_DELETE;
    op.pFrom = reinterpret_cast<LPCWSTR>(buf.utf16());
    // FOF_ALLOWUNDO = 进回收站（不是硬删）；NOCONFIRMATION/NOERRORUI/SILENT = 不弹系统框，
    // 由本程序自己决定提示方式，避免删除时连弹十几个系统对话框。
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int rc = SHFileOperationW(&op);
    // rc == 0 只说明「没报错」，还要再看 fAnyOperationsAborted：用户/系统取消时
    // 返回值也可能是 0，但实际一项都没删。
    if (rc == 0 && !op.fAnyOperationsAborted)
        return true;
    if (failed)
        *failed = paths; // 该 API 不返回逐项结果，只能整体标记失败
    return false;
#else
    // 非 Windows 没有回收站语义，降级为永久删除（头文件已声明这一平台差异）
    Q_UNUSED(failed)
    return deletePermanently(paths, failed);
#endif
}

// 逐项硬删，单项失败继续处理后面的：批量删除时不应该因为一个文件被占用就全放弃。
bool deletePermanently(const QStringList &paths, QStringList *failed)
{
    bool ok = true; // 只要有一项删不掉就整体置 false，但不会中断循环
    for (const QString &p : paths) {
        if (!removeRecursively(p)) {
            ok = false;
            if (failed)
                failed->append(p);
        }
    }
    return ok;
}

// 批量复制。空目录也会被建出来（mkpath），即使一个文件都没复制成功。
QStringList copyInto(const QStringList &paths, const QString &destDir)
{
    QStringList failed; // 收集失败项；成功项不会出现在这里
    // 不检查 mkpath 的返回值：目标目录建不出来时，下面每一项复制都会失败并计入 failed，
    // 调用方得到的结果与「提前报错」一致，没必要在这里另开一条失败路径。
    QDir().mkpath(destDir);
    for (const QString &p : paths) {
        // 先判存在：否则 copyRecursively 会给出「像是不存在」的模糊失败，
        // 而这里能明确归因为源已消失（拖拽后源被删/移动是常见情况）。
        if (!QFileInfo::exists(p)) {
            failed << p;
            continue;
        }
        if (!copyRecursively(p, destDir + QLatin1Char('/') + QFileInfo(p).fileName()))
            failed << p;
    }
    return failed;
}

// 批量移动。两个提前 continue 的分支是「无效但会被误判成失败」的情况，
// 所以既不算成功也不进 failed，静默跳过。
QStringList moveInto(const QStringList &paths, const QString &destDir)
{
    QStringList failed; // 收集失败项；成功项不会出现在这里
    QDir().mkpath(destDir);
    for (const QString &p : paths) {
        if (!QFileInfo::exists(p)) {
            failed << p;
            continue;
        }
        // 目标就是源所在目录时无需操作
        if (QFileInfo(p).absolutePath() == QDir(destDir).absolutePath())
            continue;
        // 不允许把目录移动到自身内部
        // （isSubPath 相等也算 true，所以 "移动到父目录" 这种退化情况在这里一并被挡掉；
        //   不拦的话递归复制会无限展开，直到磁盘满）
        if (QFileInfo(p).isDir() && isSubPath(destDir, p))
            continue;
        if (!moveOne(p, destDir))
            failed << p;
    }
    return failed;
}

// 同目录改名。与资源管理器不同，这里没有「是否替换」的交互机会，所以冲突一律判失败，
// 把决定权留给调用方（弹提示 / 换名字重试）。
bool rename(const QString &path, const QString &newName, QString *newPath)
{
    const QFileInfo fi(path); // 只在原目录内改名，所以目录部分直接沿用，不解析也不跟随符号链接
    QString name = newName.trimmed();
    // 没改名字也算失败：让调用方能区分「真的改名了」和「输入框没动就回车」，
    // 避免刷新列表/弹提示。
    if (name.isEmpty() || name == fi.fileName())
        return false;
    // 过滤 Windows 文件名非法字符
    static const QString illegal = QStringLiteral("\\/:*?\"<>|");
    for (const QChar &c : illegal)
        name.remove(c);
    name = name.trimmed(); // 去掉非法字符后可能只剩下首尾空白
    if (name.isEmpty())
        return false;

    const QString target = fi.absolutePath() + QLatin1Char('/') + name; // 用 absolutePath 而不是 path()：后者可能带相对段
    // 不做覆盖：Windows 资源管理器同名时会问用户，这里没有 UI 上下文，直接判失败更安全。
    if (QFileInfo::exists(target))
        return false;
    if (!QFile::rename(fi.absoluteFilePath(), target))
        return false;
    if (newPath)
        *newPath = target;
    return true;
}

// 新建文件夹。名字冲突由内部消化（自动加序号），因此「同名」不会成为失败原因，
// 失败只可能是 mkpath 真的失败（无权限 / 路径非法 / 磁盘满）。
QString createFolder(const QString &parentDir, const QString &baseName, QString *created)
{
    QString base = baseName.trimmed(); // 用 trimmed 后的名字：纯空格名字在 Windows 上会被系统裁掉，导致路径不稳定
    if (base.isEmpty())
        base = QStringLiteral("新建文件夹");
    static const QString illegal = QStringLiteral("\\/:*?\"<>|");
    for (const QChar &c : illegal)
        base.remove(c);

    QString target = parentDir + QLatin1Char('/') + base;
    // 只在需要时才走避让（uniqueTargetPath 会做多次 exists 查询）
    if (QFileInfo::exists(target))
        target = uniqueTargetPath(parentDir, base);
    if (!QDir().mkpath(target))
        return QString();
    if (created)
        *created = target;
    return target;
}

// 求不冲突的落点。注意只做「是否存在」判断，不检查文件是否被占用、是否有写权限，
// 因此返回值立刻被别人抢走的可能性仍然存在（并发场景要靠调用方自己再加锁）。
QString uniqueTargetPath(const QString &destDir, const QString &fileName)
{
    QString candidate = destDir + QLatin1Char('/') + fileName; // 先试原名：绝大多数调用都会在这里直接返回
    if (!QFileInfo::exists(candidate))
        return candidate;

    QFileInfo fi(fileName); // 这里只传文件名，所以 isDir()/exists() 都按「当前工作目录」解析，语义有限（见头文件边界说明）
    // completeBaseName（而不是 baseName）才能正确处理 "a.tar.gz" → "a.tar" + "gz"；
    // 无扩展名时 completeBaseName 可能为空，回退到整个文件名。
    const QString base = fi.completeBaseName().isEmpty() ? fileName : fi.completeBaseName();
    const QString suffix = fi.suffix();
    // 从 2 开始（"xxx (2)"），1 留给原始名字；9999 是纯防御上限，避免异常情况下死循环。
    for (int i = 2; i < 9999; ++i) {
        candidate = destDir + QLatin1Char('/') + base + QStringLiteral(" (%1)").arg(i);
        // 目录没有扩展名，不能把后缀拼回去；fi.isDir() 对尚不存在的路径恒为 false，
        // 所以这里实际只在「目标已存在且是目录」时才有区分意义。
        if (!suffix.isEmpty() && !fi.isDir())
            candidate += QLatin1Char('.') + suffix;
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return destDir + QLatin1Char('/') + base + QStringLiteral(" (copy)");
}

// 体积格式化。单位固定英文缩写、不加千分位，输出宽度不随系统区域设置变化。
QString humanSize(qint64 bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"}; // 1024 进制；最大到 TB，不做 PB 档
    double v = double(bytes); // 用 double 累除：整数除会把 1.5 KB 截成 1 KB
    int u = 0;                // units 下标，同时充当「已换算档数」
    // u < 4：最多换算到 TB，超出（PB 级）就停在 TB 档显示大数字，避免越界访问 units。
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    if (u == 0)
        return QStringLiteral("%1 B").arg(bytes); // 字节档是整数，不显示小数
    // 小于 10 保留 1 位小数（1.5 MB），否则取整（23 MB）：宽度更稳定，列表不会跳
    return QStringLiteral("%1 %2").arg(QString::number(v, 'f', v < 10 ? 1 : 0), units[u]);
}

// 类型名只按后缀给粗分类，不看文件内容：改名后类型名会立刻跟着变，符合用户在列表里的直觉。
QString typeName(const QString &path, bool isDir)
{
    if (isDir)
        return QStringLiteral("文件夹");
    // 用 suffix() 而不是完整文件名：只是给列表做粗分类，"tar.gz" 归到 GZ 文件可接受
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return QStringLiteral("文件");
    return suffix.toUpper() + QStringLiteral(" 文件");
}

// 子路径判定是「前缀 + 分隔符」的字符串比较，不碰磁盘：
// 因此对符号链接、大小写敏感挂载点、以及跨盘符的 ".." 都只按字面处理。
bool isSubPath(const QString &child, const QString &parent)
{
    // 先归一化：absolutePath 补全相对路径，cleanPath 消掉 ".." 和多余分隔符，
    // 否则 "C:/a/../a/b" 与 "C:/a" 会因为字符串不同而被误判为不相关。
    const QString c = QDir::cleanPath(QDir(child).absolutePath());
    const QString p = QDir::cleanPath(QDir(parent).absolutePath());
#ifdef Q_OS_WIN
    // Windows 路径大小写不敏感；其他平台必须区分，否则 /A 和 /a 会被当成同一目录
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    if (c.compare(p, cs) == 0)
        return true; // 相等也算「在其内」，调用方（moveInto）正好需要这个语义
    // 必须带分隔符比较：否则 "C:/abc" 会被判成 "C:/ab" 的子路径
    return c.startsWith(p.endsWith('/') ? p : p + QLatin1Char('/'), cs);
}

// 只改内存里的两份状态，不碰系统剪贴板：跨进程粘贴拿不到，但也不会污染用户的系统剪贴板。
void setClipboardPaths(const QStringList &paths, bool cut)
{
    g_clipboard = paths;
    g_clipboardCut = cut;
}

// 按值返回，调用方改返回值不会影响内部状态。
QStringList clipboardPaths()
{
    return g_clipboard;
}

// 默认 false（复制）是安全侧：误判成「剪切」会在粘贴后删掉用户文件，误判成「复制」只是多一份。
bool clipboardIsCut()
{
    return g_clipboardCut;
}

} // namespace ShellActions
