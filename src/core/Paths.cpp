#include "core/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QStandardPaths>

// 路径拼接全部用 '/'：Windows 的 QDir/QFile 都接受，避免混用反斜杠后出现
// 「C:\a\/b」这种既不好看也难做字符串比较的路径。
// 注意这些函数只拼字符串，不创建目录——除 ensureDir() 外都没有副作用。

namespace
{

// 递归拷贝目录（含子目录与文件），失败静默跳过，不中断主流程。
void copyDirRecursively(const QString &src, const QString &dst)
{
    QDir srcDir(src);
    if (!srcDir.exists())
        return;
    QDir().mkpath(dst);
    QDirIterator it(src, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo &fi = it.fileInfo();
        const QString target = dst + QLatin1Char('/')
                               + srcDir.relativeFilePath(fi.absoluteFilePath());
        if (fi.isDir()) {
            QDir().mkpath(target);
        } else {
            QDir().mkpath(QFileInfo(target).absolutePath());
            QFile::copy(fi.absoluteFilePath(), target);
        }
    }
}

// 一次性迁移：旧数据在 %APPDATA%/QDeskWidget，新数据在 exe 同级的 QDeskWidget/。
// 注意：新目录里很可能已经先有 plugins/ 子目录（构建 / 安装时就放进去了），
// 所以不能简单用「目录是否存在」判断，否则会被 plugins/ 误判为「已初始化」而跳过迁移。
// 这里以「关键数据文件 settings.json / boxes.json 是否落地」作为初始化标志：
//   - 新位置已有其一 → 认为已迁移过，跳过；
//   - 新位置都没有、旧位置至少有其一 → 整体拷过来；
//   - 两端都没有 → 全新安装，无需迁移。
// 旧目录保留不删，迁移完成后就是死数据，用户可自行清理。
void migrateFromAppData(const QString &newBase)
{
    const bool newInitialized = QFile::exists(newBase + QStringLiteral("/settings.json"))
                                || QFile::exists(newBase + QStringLiteral("/boxes.json"));
    if (newInitialized)
        return;
    const QString oldBase = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (oldBase.isEmpty())
        return;
    const bool oldHasData = QFile::exists(oldBase + QStringLiteral("/settings.json"))
                            || QFile::exists(oldBase + QStringLiteral("/boxes.json"));
    if (!oldHasData)
        return;
    copyDirRecursively(oldBase, newBase);
}

} // namespace

namespace Paths
{

// 结果缓存到函数内静态变量：applicationDirPath 每次调用也要问一次系统，
// 而路径在进程生命周期内不会变。lambda 保证只在首次调用时求值。
// 数据目录放在 exe 同级目录下的 QDeskWidget/，使程序成为「数据随 exe 走」的便携形态：
// 整个程序文件夹拷到别的机器也能带着配置/盒子/缓存一起跑。
QString dataDir()
{
    static const QString dir = []
    {
        QString base = QCoreApplication::applicationDirPath()
                       + QStringLiteral("/QDeskWidget");
        migrateFromAppData(base);
        return base;
    }();
    return dir;
}

// 下面几个都是「根目录 + 固定名」的纯拼接，不再校验存在性。
QString settingsFile()
{
    return dataDir() + QStringLiteral("/settings.json");
}

QString boxesFile()
{
    return dataDir() + QStringLiteral("/boxes.json");
}

QString storageRoot()
{
    return dataDir() + QStringLiteral("/storage");
}

// 只用 '/' 连接，因此 boxId 里若混入分隔符会直接改变路径层级；本程序的 id
// 由 BoxManager::makeId() 生成的十六进制串保证不会出现这种情况。
QString boxStorageDir(const QString &boxId)
{
    return storageRoot() + QLatin1Char('/') + boxId;
}

// mkpath 会递归建出中间目录，已存在时也返回 true；空串直接判失败，
// 交给调用方决定怎么处理「没有路径」这种情况。
bool ensureDir(const QString &dir)
{
    if (dir.isEmpty())
        return false;
    QDir d;
    return d.mkpath(dir);
}

} // namespace Paths
