// -----------------------------------------------------------------------------
// Autostart —— 开机自启实现（HKCU\...\Run 注册表值）。
// 约定见 Autostart.h；这里只补充实现层面的坑：
// 坑1：QSettings 在「键没有写权限」时可能只是静默失败，所以写完必须 sync() 再用
//      status() / 回读校验，不能只凭 setValue() 没崩就认为成功。
// 坑2：命令行必须整串加引号，含空格的安装路径不加引号会被系统拆成两段，开机启动必失败。
// 坑3：QSettings 不是可拷贝的（有 QObject 基类），不要写「返回 QSettings」的辅助函数。
// -----------------------------------------------------------------------------
#include "platform/Autostart.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QSettings>

namespace
{
/**
 * 启动项所在的注册表路径：当前用户的 Run 键，登录时由系统逐条执行。
 * 只碰 HKCU —— HKLM 需要管理员权限，计划任务则要在任务计划程序里留一条用户看不懂的项。
 */
QString runKeyPath()
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}

/**
 * 启动项名。任务管理器「启动」标签里显示的就是它，
 * 改名等于新增一条：旧条目会变成指向同一 exe 的孤儿，所以这个字符串当常量对待。
 */
QString valueName()
{
    return QStringLiteral("QDeskWidget");
}
} // namespace

namespace Autostart
{

bool isSupported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool isEnabled()
{
#ifdef Q_OS_WIN
    QSettings reg(runKeyPath(), QSettings::NativeFormat);
    // 用 contains 而不是「value 转成字符串后判空」：用户可能把值清成空串，
    // 那也算启动项还在（只是无效），同样应该显示为「已开启」，再由 setEnabled(true) 重写值
    return reg.contains(valueName());
#else
    return false;
#endif
}

QString commandLine()
{
    // 反斜杠：注册表里写正斜杠系统也能跑，但任务管理器 / 第三方启动项工具显示出来很怪
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    // 整串加引号：便携版常被放在 "D:\My Tools\..." 这类含空格的目录里，
    // 不加引号系统会按空格切分，把目录当程序名，开机时静默启动失败
    return QStringLiteral("\"%1\"").arg(exe);
}

bool setEnabled(bool on)
{
#ifdef Q_OS_WIN
    QSettings reg(runKeyPath(), QSettings::NativeFormat);
    if (on)
        reg.setValue(valueName(), commandLine());
    else
        reg.remove(valueName()); // 本来就不存在时是空操作，也算成功

    reg.sync(); // 不 sync 的话写入还在 QSettings 的缓冲里，进程退出前不保证落盘
    if (reg.status() != QSettings::NoError) {
        qWarning("autostart: 写注册表失败（status=%d），开机自启未改变", int(reg.status()));
        return false;
    }

    // 回读校验：权限不足时 QSettings 可能既不报错也没写进去，
    // 只有读回来的状态与目标一致才算成功，否则界面会显示一个假的勾
    return isEnabled() == on;
#else
    Q_UNUSED(on)
    return false;
#endif
}

void syncIfEnabled()
{
#ifdef Q_OS_WIN
    QSettings reg(runKeyPath(), QSettings::NativeFormat);
    if (!reg.contains(valueName()))
        return; // 没开启：纯读一次就返回，不产生任何写入

    const QString want = commandLine();
    // 忽略大小写：Windows 路径本身不区分大小写，只有「根本不是同一个 exe」才需要重写
    if (reg.value(valueName()).toString().compare(want, Qt::CaseInsensitive) == 0)
        return;

    reg.setValue(valueName(), want);
    reg.sync();
    if (reg.status() == QSettings::NoError)
        qInfo("autostart: 启动项路径已校正为 %s", qUtf8Printable(want));
#endif
}

} // namespace Autostart
