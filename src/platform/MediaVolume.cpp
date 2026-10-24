// -----------------------------------------------------------------------------
// MediaVolume —— 按播放器调音量的 Core Audio 实现（MediaVolume.h 的实现文件）。
// 职责：枚举默认渲染设备上的所有音频会话，按进程 exe / 会话标识匹配目标播放器，
//       再通过 ISimpleAudioVolume 读写它自己的音量标量。
// 约定：每次调用都新起一次 ComScope，用完即还；所有失败路径都 return false，
//       由公开函数翻译成 -1 / false，调用方不需要感知 HRESULT。
// 坑：  AUMID 形式的 appId（Company.AppName_hash）没有进程路径可对，只能退化到
//       「会话标识包含应用名」的模糊匹配，因此同类播放器多开时可能命中错的那个。
// -----------------------------------------------------------------------------
#include "platform/MediaVolume.h"

#ifdef Q_OS_WIN
#  include <windows.h>

#  include <audiopolicy.h>
#  include <endpointvolume.h>
#  include <mmdeviceapi.h>

#  include <algorithm> // std::size()：求栈上路径缓冲区的元素个数，避免写死 MAX_PATH*2
#  include <functional> // std::function：把「拿到会话后做什么」抽成回调，供两处遍历复用
#endif

namespace
{

#ifdef Q_OS_WIN
// 内部链接：ComPtr/ComScope 与各匹配辅助函数都只服务本 TU，
// 不放进 MediaVolume 命名空间，避免污染公开头文件的符号面。

/**
 * 极简 COM 智能指针（只需要 Release）
 *
 * 只用于本文件内部持有 COM 接口，故意不实现 AddRef/拷贝：
 * 所有获取路径都是「新建 → 用 → 随作用域销毁」，不存在共享所有权。
 * 模板参数 T 必须是带 Release() 的 COM 接口（IMMDevice 等）。
 *
 * 非线程安全：接口指针既不封送也不加锁，只能在自己取到它的那个线程里用完即弃。
 * 单线程内使用是安全的——COM 接口的线程模型由 CoInitializeEx 的公寓保证，与本类无关。
 */
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    /** 供 -> 取接口方法；不在任何路径上做判空，调用前须确认 ok() */
    T *operator->() const { return m_p; }
    /** 裸指针（可判空）；用于传给只需要入参接口的 API，所有权仍在本对象 */
    T *get() const { return m_p; }
    /** 取 &m_p 供 CoCreateInstance/Activate 等出参使用；会先释放旧值，避免覆盖泄漏 */
    T **put()
    {
        reset();
        return &m_p;
    }
    /** 释放并置空；可重复调用，析构与本对象所有失败路径都靠它兜住引用计数 */
    void reset()
    {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }
    /** 是否持有接口；每个 CoCreateInstance/Activate 之后都靠它判空，别只看 HRESULT */
    bool ok() const { return m_p != nullptr; }

private:
    /** 裸接口指针，生命周期完全由本对象管理 */
    T *m_p = nullptr;
};

/**
 * 每次调用都自己初始化 COM，避免和 Qt / WinRT 的初始化状态互相干扰
 *
 * 只负责「这一小段调用期间 COM 可用」。若线程已被别的模块以别的模式初始化过
 * （RPC_E_CHANGED_MODE），不视为失败：接口仍可同步使用，只是不归我们释放。
 */
class ComScope
{
public:
    ComScope()
    {
        // 用 STA 而不是 MTA：与 main.cpp 里 winrt::init_apartment(single_threaded) 保持
        // 同一公寓模型，同一线程再次 CoInitializeEx 只会拿到 S_FALSE，不会把线程改到别的公寓
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // S_OK 才需要配对 Uninitialize；RPC_E_CHANGED_MODE 说明别的线程模式下已经初始化过，直接用
        m_owned = SUCCEEDED(hr);
        m_usable = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    ~ComScope()
    {
        if (m_owned)
            CoUninitialize();
    }
    /** 当前线程能否安全调用 Core Audio 接口（false 时调用方应直接放弃本次操作） */
    bool usable() const { return m_usable; }

private:
    bool m_owned = false;  // 是否由本对象完成初始化（决定要不要配对 CoUninitialize）
    bool m_usable = false; // 是否可用；S_FALSE 也表示可用，只有硬失败才 false
};

/**
 * 取进程可执行文件的完整路径；拿不到返回空
 *
 * @param pid 目标进程 ID，0（系统空闲进程/会话无进程）直接返回空串。
 * @return 完整 NT 路径；进程已退出、权限不足（受保护进程）或调用失败时为空。
 *         用 PROCESS_QUERY_LIMITED_INFORMATION 而非 QUERY_INFORMATION，是为了
 *         在权限受限时也能拿到路径，减少「明明在放歌却匹配不到」。
 */
QString processImagePath(DWORD pid)
{
    if (pid == 0)
        return QString();

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return QString(); // 受保护进程 / 已退出：拿不到路径就只能靠 sessionId 做模糊匹配

    wchar_t buf[MAX_PATH * 2] = {}; // 长路径可能超过 MAX_PATH，故开到两倍
    DWORD size = DWORD(std::size(buf)); // 入参为缓冲区字符容量，返回后被改写为实际写入的字符数（不含结尾 \0）
    QString out;
    if (QueryFullProcessImageNameW(h, 0, buf, &size))
        out = QString::fromWCharArray(buf, int(size));
    CloseHandle(h);
    return out;
}

/**
 * 文件名去掉目录与 .exe，统一小写，用来做匹配
 *
 * @param pathOrId 完整路径（C:\...\cloudmusic.exe）或裸文件/AUMID 串，均可。
 * @return 小写化的「主干名」；AUMID 不会被切开（其中没有路径分隔符，只会被小写化）。
 *         输入为空/纯空白时返回空串，调用方据此判无效。
 */
QString exeStem(const QString &pathOrId)
{
    QString s = pathOrId;
    // 同时找 / 和 \：调用方可能给 Qt 风格路径，也可能给原生路径，取靠后的那个分隔符
    const int slash = qMax(s.lastIndexOf(QLatin1Char('/')), s.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0)
        s = s.mid(slash + 1);
    if (s.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
        s.chop(4); // 去掉 ".exe" 四个字符（大小写不限，chop 与前面的不敏感判断配套）
    return s.trimmed().toLower();
}

/**
 * appId 可能是 "cloudmusic.exe"（Win32 程序），也可能是
 * "Company.AppName_hash"（打包应用），所以要分两种方式比对。
 *
 * @param appId     调用方传入的目标标识。
 * @param exePath   会话对应进程的可执行文件全路径；拿不到时为空串（跳过 exe 比对）。
 * @param sessionId 会话标识（通常是 AUMID 形态的字符串）；可能为空。
 * @return 是否认为该会话属于目标播放器。
 *
 * 判定顺序：exe 主干名全等优先；否则从 AUMID 里剥出「应用名」做包含匹配。
 * 包含匹配要求应用名至少 3 个字符，短名（如 "qq"）会误命中大量无关进程，宁可放弃。
 */
bool matches(const QString &appId, const QString &exePath, const QString &sessionId)
{
    const QString want = exeStem(appId); // 目标标识的主干名（已小写、已去目录与 .exe）
    if (want.isEmpty())
        return false;

    const QString exe = exeStem(exePath); // 会话进程的主干名；空串表示拿不到进程路径
    if (!exe.isEmpty() && exe == want)
        return true; // Win32 程序：文件名对得上就是它，这条最可靠，优先短路

    // AUMID：取最后一段（应用名）做包含匹配
    // 先切掉 "_1.0.0.0_x64__hash" 这段版本/架构/哈希，再从 "Company.AppName" 里取 AppName
    QString tail = want.section(QLatin1Char('_'), 0, 0);
    tail = tail.section(QLatin1Char('.'), -1);
    // 这两刀对非 AUMID 串（"cloudmusic"）等于原样返回，所以不会误伤 exe 名；
    // 只对带 _ 和 . 的打包应用标识起作用。
    if (tail.size() >= 3) { // 见函数注释：短名（"qq"）会误命中大量无关进程
        if (!exe.isEmpty() && exe.contains(tail))
            return true; // 打包应用的会话进程名常带应用名（如 ...CloudMusic.exe）
        if (sessionId.toLower().contains(tail))
            return true; // 会话标识大小写不确定，统一小写后再比（tail 已经过 exeStem 小写化）
    }
    return false;
}

/** 会话回调：返回 true 表示「已处理完毕，停止遍历」，false 表示「继续找下一个会话」 */
using SessionFn = std::function<bool(ISimpleAudioVolume *)>;

/**
 * 在所有音频会话里找 appId 对应的那个，找到就调用 fn
 *
 * @param appId 目标播放器标识，匹配规则见 matches()。
 * @param fn    命中会话后调用的回调，入参保证非空（调用期间由 ComPtr 持有）；
 *              返回 true 立即结束遍历，返回 false 则继续匹配后续同名会话
 *              （某些播放器会为每个音频流建独立会话）。
 * @return 是否至少有一次 fn 返回了 true。
 *
 * 失败行为：COM 不可用、没有默认渲染设备、拿不到会话枚举，均返回 false，
 * 不区分具体原因（对调用方而言都是「调不了这个播放器」）。
 */
bool withAppSession(const QString &appId, const SessionFn &fn)
{
    ComScope com;
    if (!com.usable())
        return false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void **>(enumerator.put()))))
        return false;

    ComPtr<IMMDevice> device;
    // eRender = 输出设备，eConsole = 游戏/系统默认角色；只处理「当前正在用的扬声器」
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put())))
        return false;

    // 必须激活 IAudioSessionManager2：一代 IAudioSessionManager 拿不到会话枚举
    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(manager.put()))))
        return false;

    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(sessions.put())))
        return false;

    int count = 0; // 当前端点上的会话数（含系统声音等非目标会话）；拿不到就没法遍历
    if (FAILED(sessions->GetCount(&count)))
        return false;

    // 遍历的是一份「会话快照」：期间播放器可能新建或销毁会话，
    // 因此每次 GetSession 都可能失败，只能逐个跳过，不能因为一个坏会话就放弃整轮匹配。
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        // 单个会话取不到就跳过：注销中的会话会让 GetSession 失败，不该连累整轮匹配
        if (FAILED(sessions->GetSession(i, control.put())) || !control.ok())
            continue;

        // 只有 Control2 才提供进程 ID / 会话标识，Control1 无法定位到具体播放器
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                          reinterpret_cast<void **>(control2.put())))
            || !control2.ok())
            continue;

        DWORD pid = 0;
        control2->GetProcessId(&pid); // 失败时保持 0，processImagePath(0) 会返回空串

        // GetSessionIdentifier 返回 CoTaskMemAlloc 出来的字符串，必须 CoTaskMemFree，
        // 不能用 delete/free（不同堆，会崩）。
        LPWSTR rawId = nullptr;
        QString sessionId;
        if (SUCCEEDED(control2->GetSessionIdentifier(&rawId)) && rawId) {
            sessionId = QString::fromWCharArray(rawId);
            CoTaskMemFree(rawId);
        }

        if (!matches(appId, processImagePath(pid), sessionId))
            continue;

        // 会话存在不代表可调音量：QueryInterface 失败说明该会话没有音量控制能力
        ComPtr<ISimpleAudioVolume> volume;
        if (FAILED(control2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                           reinterpret_cast<void **>(volume.put())))
            || !volume.ok())
            continue;

        if (fn(volume.get()))
            return true;
    }
    return false;
}

/**
 * 取默认输出设备的主音量接口并回调。
 *
 * @param fn 入参为设备主音量接口（非空）；返回值原样成为本函数的返回值。
 * @return fn 的返回值；拿不到设备/接口时返回 false。
 *
 * 与 withAppSession 的区别：这里操作的是端点（endpoint）级主音量，即系统总音量。
 */
bool withEndpointVolume(const std::function<bool(IAudioEndpointVolume *)> &fn)
{
    ComScope com;
    if (!com.usable())
        return false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void **>(enumerator.put()))))
        return false;

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put())))
        return false;

    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(volume.put()))))
        return false;

    return fn(volume.get());
}

#endif // Q_OS_WIN

} // namespace

namespace MediaVolume
{

// 读单个播放器音量：标量 0.0-1.0 换算成百分比并夹到 0-100（播放器可能短暂回报越界值）。
int volumeFor(const QString &appId)
{
#ifdef Q_OS_WIN
    int result = -1; // -1 是「没找到会话」的哨兵，回调成功才会被改写
    // 不看 withAppSession 的返回值：它只表示「回调有没有返回 true」，
    // 而这里回调返回 true 必然已改写 result，所以 result 仍是 -1 就等价于失败，信息无损。
    withAppSession(appId, [&result](ISimpleAudioVolume *v)
    {
        float scalar = 0.0f; // 音量标量，合法范围 0.0f-1.0f（非线性感知曲线，但接口按线性标量存）
        if (FAILED(v->GetMasterVolume(&scalar)))
            return false;
        result = qBound(0, qRound(scalar * 100.0f), 100);
        return true;
    });
    return result;
#else
    Q_UNUSED(appId)
    return -1;
#endif
}

// 写单个播放器音量：先夹紧到 0-100，再换算回 0.0-1.0 标量。
bool setVolume(const QString &appId, int volume)
{
#ifdef Q_OS_WIN
    const int v = qBound(0, volume, 100); // 先夹紧成整数百分比，再换算标量，避免负值被强转成负数标量
    return withAppSession(appId, [v](ISimpleAudioVolume *vol)
    {
        // 第二参数是 eventContext，用于关联 IAudioSessionEvents 回调；这里不监听，传 nullptr
        // 两个易错点：① 只写音量、不动静音位，设备/会话处于静音时写进去也听不到声音；
        //             ② 命中第一个可写会话就返回，多会话播放器只改到其中一条流。
        return SUCCEEDED(vol->SetMasterVolume(float(v) / 100.0f, nullptr));
    });
#else
    Q_UNUSED(appId)
    Q_UNUSED(volume)
    return false;
#endif
}

// 读系统主音量：走端点级的 MasterVolumeLevelScalar，与播放器会话无关。
int systemVolume()
{
#ifdef Q_OS_WIN
    int result = -1; // 同上：-1 表示「读不到主音量」，成功才被回调改写
    withEndpointVolume([&result](IAudioEndpointVolume *v)
    {
        float scalar = 0.0f;
        if (FAILED(v->GetMasterVolumeLevelScalar(&scalar)))
            return false;
        result = qBound(0, qRound(scalar * 100.0f), 100);
        return true;
    });
    return result;
#else
    return -1;
#endif
}

// 写系统主音量。注意这是「标量」而非 dB：设备被静音时标量仍可写，但不会自动解除静音。
bool setSystemVolume(int volume)
{
#ifdef Q_OS_WIN
    const int v = qBound(0, volume, 100); // 越界值静默夹紧，和头文件承诺一致
    return withEndpointVolume([v](IAudioEndpointVolume *vol)
    {
        return SUCCEEDED(vol->SetMasterVolumeLevelScalar(float(v) / 100.0f, nullptr));
    });
#else
    Q_UNUSED(volume)
    return false;
#endif
}

} // namespace MediaVolume
