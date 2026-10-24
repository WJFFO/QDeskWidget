// -----------------------------------------------------------------------------
// SingleInstance —— 命名互斥体实现（非 Windows 退回 QLockFile）。
//
// 坑1：CreateMutexW 返回 ERROR_ALREADY_EXISTS 时拿到的是**别人的**互斥体句柄，
//      必须 CloseHandle()，否则本进程会一直替对方「举着」这把锁，
//      对方退出后锁也不会释放（表现为重启程序永远起不来）。
// 坑2：用 "Local\" 前缀（会话级）而不是 "Global\"：Global 需要 SeCreateGlobalPrivilege，
//      普通账户不一定有；同一会话内只跑一个已经足够，快速用户切换下各会话互不干扰。
// 坑3：句柄故意不关也不存进 QObject，进程退出由系统回收，省掉「谁来 release」的时序问题。
// -----------------------------------------------------------------------------
#include "platform/SingleInstance.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#  include <windows.h>
#else
#  include <QLockFile>

#  include <memory>
#endif

namespace
{
#ifdef Q_OS_WIN
/** 成功抢到的互斥体句柄；非空即代表「本进程已被认定为唯一实例」 */
HANDLE g_mutex = nullptr;
#else
/** 非 Windows 的降级实现：用锁文件代替互斥体，同样只在本进程内保存一份 */
std::unique_ptr<QLockFile> g_lock;
#endif
} // namespace

namespace SingleInstance
{

bool acquire(const QString &key)
{
#ifdef Q_OS_WIN
    if (g_mutex)
        return true; // 重复调用视为成功，不重复建句柄

    const QString full = QStringLiteral("Local\\") + key;
    // 第二个参数 FALSE = 不要求立即拥有：我们只要「名字唯一」这个身份，
    // 不打算 WaitForSingleObject，所以不需要初始所有权
    HANDLE h = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(full.utf16()));
    if (!h)
        return false; // 极端情况（句柄耗尽 / 名字非法）：按已有实例处理，宁可不启动也不开两个

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h); // 别人的锁：只是借了个句柄，必须还回去（见文件头坑1）
        return false;
    }

    g_mutex = h; // 保持到进程结束，由系统回收
    return true;
#else
    if (g_lock)
        return true;
    // 锁文件放临时目录而不是数据目录：数据目录可能只读，读不到就会误判成「已有实例」
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(key + QStringLiteral(".lock"));
    auto lock = std::make_unique<QLockFile>(path);
    // 0 = 不按时间判过期，只看持有者进程是否还活着：本程序的实例会常驻托盘跑几天，
    // 用默认的 30s 反而会把正常持有的锁当陈旧锁抢走
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0))
        return false;
    g_lock = std::move(lock);
    return true;
#endif
}

} // namespace SingleInstance
