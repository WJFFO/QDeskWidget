// -----------------------------------------------------------------------------
// ShellActions —— 文件系统 / Windows Shell 操作封装（头文件）。
// 职责：把「打开、定位、复制、移动、改名、删除、回收站、剪贴板」收敛成纯函数，
//       UI 层不直接写 QFile/SHFileOperation。
// 约定：破坏性操作一律优先走系统语义（回收站、系统图标/类型名）；批处理函数不中途
//       弹窗，失败项通过返回值或 failed 出参交回给调用方决定怎么提示。
// 坑：  同一目录内移动、把目录移进自身子目录都属于「无效但危险」的操作，
//       moveInto 会静默跳过；调用方不要假设每个入参都会被处理。
// -----------------------------------------------------------------------------
#pragma once

#include <QString>
#include <QStringList>

/**
 * 文件系统 / Windows Shell 操作封装。
 * 移动、复制、删除等破坏性操作都尽量走系统语义（回收站、确认）。
 */
namespace ShellActions
{

/**
 * 把 Qt 的 '/' 分隔路径转成本平台习惯的分隔符。
 * @param path 任意 Qt 风格路径（可为空）。
 * @return Windows 上为反斜杠形式；其他平台原样返回。仅用于显示/传给原生 API，
 *         不要拿返回值再去做 QFile 操作（Qt 两种都认，但混用后比较字符串会不等）。
 */
QString nativePath(const QString &path);

/**
 * 用默认程序打开
 * @param path 文件或目录路径；空串直接返回 false。
 * @return QDesktopServices::openUrl 的结果。注意它只表示「请求已交给系统」，
 *         关联程序打不开（如无默认程序）时仍可能返回 true。
 */
bool open(const QString &path);                      // 用默认程序打开

/**
 * 资源管理器中定位（选中该文件/文件夹）
 * @param path 目标路径；空串直接返回。
 * 无返回值：Windows 上用 explorer /select 异步拉起（startDetached，不等待、拿不到失败），
 * 其他平台退化为打开其父目录。
 */
void revealInExplorer(const QString &path);          // 资源管理器中定位

/**
 * 打开文件夹
 * @param dir 目录路径；不校验存在性，交给系统处理（Windows 上失败会返回 false）。
 * @return Windows：ShellExecuteEx 成功即 true，失败退回 explorer.exe；两条路都不行才 false
 *         （调用方应给出反馈——静默失败是「点了没反应」的来源）。
 *         其它平台：QDesktopServices::openUrl 的结果，只表示请求已提交。
 */
bool openFolder(const QString &dir);                 // 打开文件夹

/**
 * 移入回收站（Windows）/ 永久删除（其他平台降级）
 * @param paths  待删除项，可含文件与目录；空列表视为成功（返回 true）。
 * @param failed 出参，可为 nullptr。整体失败时被赋成整个 paths（系统 API 不告诉具体哪项失败）。
 * @return 全部成功为 true；被用户取消、被占用、权限不足均为 false。
 *         注意 Windows 上带 FOF_SILENT/FOF_NOCONFIRMATION，不会弹系统确认框。
 */
bool moveToRecycleBin(const QStringList &paths, QStringList *failed = nullptr);

/**
 * 永久删除（不进回收站，不可恢复）
 * @param paths  待删除项。
 * @param failed 出参，可为 nullptr；逐项追加失败的路径（与回收站版不同，这里能精确到项）。
 * @return 全部成功为 true；部分失败时为 false，成功的项已被删掉。
 */
bool deletePermanently(const QStringList &paths, QStringList *failed = nullptr);

/**
 * 复制 / 移动一批文件到目标目录；返回失败路径
 *
 * 目标目录不存在会自动创建；同名目标自动改名（"xxx (2).ext"），不会覆盖。
 * @param paths   源路径列表；不存在的项直接算失败。
 * @param destDir 目标目录（不存在则 mkpath）。
 * @return 失败的源路径列表；空列表表示全部成功。返回值只用于提示，处理已经发生。
 */
QStringList copyInto(const QStringList &paths, const QString &destDir);

/**
 * 移动一批文件到目标目录（同卷 rename，跨卷自动降级为「复制 + 删除源」）。
 * @param paths   源路径列表；不存在的项算失败。
 * @param destDir 目标目录。
 * @return 失败的源路径列表。
 *
 * 静默跳过两种情况（既不算成功也不算失败）：目标就是源当前所在目录、
 * 或把目录移动到它自己的子目录里（后者会无限递归）。
 */
QStringList moveInto(const QStringList &paths, const QString &destDir);

/**
 * 重命名（同目录内）
 * @param path    原路径。
 * @param newName 新文件名（不含目录）；会自动剔除 \/:*?"<>| 这些非法字符。
 * @param newPath 出参，可为 nullptr；成功时写入改名后的完整路径。
 * @return 成功为 true。新名为空、清掉非法字符后为空、与原文件名相同、
 *         目标已存在、rename 失败，均返回 false（不覆盖已有文件）。
 */
bool rename(const QString &path, const QString &newName, QString *newPath = nullptr);

/**
 * 在 parentDir 下新建文件夹
 * @param parentDir 父目录（不校验存在性；mkpath 会连带创建）。
 * @param baseName  期望名字；为空时用「新建文件夹」，非法字符会被剔除。
 * @param created   出参，可为 nullptr；成功时写入最终路径。
 * @return 创建出的完整路径；失败（mkpath 返回 false）时返回空串。
 *
 * 同名时自动改成「新建文件夹 (2)」这类可用名，不会失败。
 */
QString createFolder(const QString &parentDir, const QString &baseName, QString *created = nullptr);

/**
 * 在 destDir 下求一个不冲突的路径
 * @param destDir  目标目录（不校验存在性）。
 * @param fileName 期望文件名（可含扩展名）。
 * @return 若 destDir/fileName 不存在，原样返回；否则追加 " (2)"、" (3)"…… 直到不冲突。
 *
 * 边界：序号最多试到 9998，之后兜底返回 "base (copy)"（可能仍然冲突）；
 *       判断「是否目录」用的是磁盘现状，而目标通常还不存在，所以带点的目录名
 *       会被当成「有扩展名的文件」处理，改名时序号会插在扩展名之前。
 */
QString uniqueTargetPath(const QString &destDir, const QString &fileName);

/**
 * 人类可读的体积
 * @param bytes 字节数；按 1024 进制换算，最大到 TB。
 * @return 形如 "512 B"、"1.5 KB"、"23 MB"：B 档不带小数，其余档 <10 保留 1 位小数。
 *         负数未做特殊处理，会原样显示成 "-1 B"。
 */
QString humanSize(qint64 bytes);

/**
 * 文件类型的中文名（用于列表展示）
 * @param path  文件路径，用来取扩展名（目录时忽略）。
 * @param isDir 是否目录；true 时直接返回「文件夹」。
 * @return 「文件夹」/「文件」（无扩展名）/「PNG 文件」这类大写扩展名描述。
 */
QString typeName(const QString &path, bool isDir);

/**
 * child 是否位于 parent 之内
 *
 * 纯字符串判断（不访问磁盘、不解析符号链接），路径先 cleanPath + absolutePath 归一化。
 * @param child  子路径。
 * @param parent 父路径。
 * @return child 等于 parent 或位于其下为 true；Windows 下不区分大小写。
 *         注意：cleanPath 会消掉 ".."，但全程不解析符号链接，也不判断两者是否真实存在。
 */
bool isSubPath(const QString &child, const QString &parent); // child 是否位于 parent 之内

/**
 * 资源管理器拖拽/粘贴所用的内部剪贴板
 *
 * 只是进程内的两份静态数据（路径列表 + 是否剪切），不写系统剪贴板，
 * 因此从外部资源管理器 Ctrl+V 拿不到这些路径，反过来本程序也读不到系统剪贴板里的文件。
 * @param paths 选中的路径列表；按值拷贝保存。
 * @param cut   true = 剪切（粘贴后要删除源），false = 复制。
 *
 * 内部是无锁的进程内全局状态，只允许在 GUI 线程调用（与右键菜单/粘贴动作的线程一致）。
 */
void setClipboardPaths(const QStringList &paths, bool cut);

/** @return 上一次 setClipboardPaths 保存的路径列表；从未设置过则为空列表。 */
QStringList clipboardPaths();

/** @return 当前剪贴板是否为「剪切」语义；默认 false（复制）。 */
bool clipboardIsCut();

} // namespace ShellActions
