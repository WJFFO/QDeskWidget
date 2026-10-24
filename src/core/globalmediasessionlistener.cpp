// -----------------------------------------------------------------------------
// globalmediasessionlistener —— GSMTC 全局媒体会话的监听与控制实现。
// 职责：RequestAsync 拿管理器 → 订阅管理器/会话事件 → 轮询 + 协程读元数据 → 发 Qt 信号。
// 约定：本类所有成员只在 Qt GUI 线程（STA）读写；winrt 异步续体一律先 co_await
//       ResumeOnGuiThread{} 切回来，再判存活标志，最后才碰 this。
// 坑1：fire_and_forget 协程不绑定对象生命周期，进程退出/对象析构后仍可能恢复，
//      因此每个回调都携带 m_alive 的拷贝，先 alive->load() 再访问任何成员。
// 坑2：SMTC 的 Position 只在播放器上报时变化，不能直接当"当前进度"用，
//      必须经 smoothPosition() 用本地单调时钟外推（详见该函数注释）。
// -----------------------------------------------------------------------------
#include "globalmediasessionlistener.h"

#include <QDebug>
#include <QHash>
#include <QCoreApplication>
#include <QThread>
#include <vector>
#include <chrono>
#include <coroutine>

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;

namespace {

// ---------------------------------------------------------------------------
// 把协程恢复点拉回 Qt GUI 线程。
//
// C++/WinRT 的 co_await 不会像 C# 那样自动回到原上下文：IAsyncOperation 完成后，
// 续体会在 Windows 线程池线程上继续执行。若在那里碰 QObject / QTimer / emit，
// 就是典型的跨线程非法访问（表现为栈/堆被破坏、RPC_E_WRONG_THREAD 等）。
// 因此每个 winrt co_await 之后都必须 co_await 一次本 awaiter。
// ---------------------------------------------------------------------------
struct ResumeOnGuiThread
{
    /** 已经在 GUI 线程（或 QCoreApplication 已不存在）时无需挂起，直接续跑 */
    bool await_ready() const noexcept
    {
        const QCoreApplication *app = QCoreApplication::instance();
        return app == nullptr || QThread::currentThread() == app->thread();
    }

    /**
     * 不在 GUI 线程时，把一个"恢复协程"的任务排队投递到主线程。
     * @param handle 待恢复的协程句柄，由 lambda 捕获后在 GUI 线程调用 resume()。
     */
    void await_suspend(std::coroutine_handle<> handle) const noexcept
    {
        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
        {
            // 没有事件循环可投递，直接在当前线程继续（进程正在退出）
            handle.resume();
            return;
        }
        // 以 app 为上下文投递：app 析构后该事件随之失效，不会回调到已释放的协程
        QMetaObject::invokeMethod(app, [handle] { handle.resume(); }, Qt::QueuedConnection);
    }

    /** 恢复后无返回值，仅用于让 co_await 表达式有合法结果类型 */
    void await_resume() const noexcept {}
};

} // namespace

namespace {

/**
 * 从 AUMID（如 NetEase.CloudMusic_1.0.0.0_x64__...）解析出可读的应用名
 *
 * @param appId 会话的 SourceAppUserModelId；空串时返回「未知播放器」。
 * @return 优先返回 knownNames 里的中文友好名（如「网易云音乐」）；
 *         否则返回剥掉厂商前缀后的 AppName（如 "CloudMusic"）；
 *         完全解析不出时返回去掉版本段的原始串。
 *
 * 纯字符串启发式，不查注册表：打包应用的 AUMID 格式并不完全统一，
 * 这里只求「够用且不崩」，解析失败也不影响会话本身的可用性。
 */
QString appNameFromAppId(const QString &appId)
{
    if (appId.isEmpty())
        return QStringLiteral("未知播放器");

    // AUMID 形如 "Company.AppName_..._hash"，取第一个下划线之前的部分
    const int underscore = appId.indexOf(QLatin1Char('_'));
    QString name = underscore >= 0 ? appId.left(underscore) : appId;

    // 去掉常见厂商前缀（如 "NetEase."、"Tencent."、"SpotifyAB." 等），保留 AppName
    static const char *const prefixes[] = {
        "NetEase.", "Tencent.", "TencentTechnology.", "SpotifyAB.", "Microsoft.",
        "AppleInc.", "Google.", "Yandex.", "VLC."
    };
    for (const char *prefix : prefixes)
    {
        const QString p = QString::fromLatin1(prefix);
        if (name.startsWith(p))
            name = name.mid(p.size());
    }

    // 已知应用的中文友好名
    static const QHash<QString, QString> knownNames = {
        { QStringLiteral("CloudMusic"), QStringLiteral("网易云音乐") },
        { QStringLiteral("QQMusic"), QStringLiteral("QQ音乐") },
        { QStringLiteral("KugouMusic"), QStringLiteral("酷狗音乐") },
        { QStringLiteral("KuwoMusic"), QStringLiteral("酷我音乐") },
        { QStringLiteral("MiguMusic"), QStringLiteral("咪咕音乐") },
        { QStringLiteral("Spotify"), QStringLiteral("Spotify") },
        { QStringLiteral("ZuneMusic"), QStringLiteral("Groove 音乐") },
        { QStringLiteral("MSEdge"), QStringLiteral("Edge 浏览器") },
        { QStringLiteral("chrome"), QStringLiteral("Chrome 浏览器") },
        { QStringLiteral("firefox"), QStringLiteral("Firefox 浏览器") },
        { QStringLiteral("VLC"), QStringLiteral("VLC") },
    };
    if (knownNames.contains(name))
        return knownNames.value(name);

    return name;
}

} // namespace

// 构造：只做 Qt 侧准备（元类型注册 + 定时器配置），不碰 WinRT，因此不会抛异常。
// @param parent 见头文件；定时器用成员对象、随本对象一起销毁，无需手动 stop 才安全。
GlobalMediaSessionListener::GlobalMediaSessionListener(QObject *parent)
    : QObject(parent)
{
    // availableSessions 携带 QList<SessionEntry>，注册后即使跨线程排队也能投递
    qRegisterMetaType<QList<GlobalMediaSessionListener::SessionEntry>>(
        "QList<GlobalMediaSessionListener::SessionEntry>");

    m_pollTimer.setInterval(500); // 500ms：事件丢了的兜底节奏，再密就是白耗 CPU，再疏进度条会顿
    connect(&m_pollTimer, &QTimer::timeout, this, &GlobalMediaSessionListener::pollCurrentSession);

    // 看门狗单次触发：RequestAsync 正常返回时由 RequestAndInit 主动 stop，
    // 没停就说明 broker 卡住了，交给 onInitTimeout 重试/报错。
    m_initTimer.setSingleShot(true);
    // 首轮给 5s：正常 broker 冷启动是百毫秒级，超过 5s 基本可判「卡住」，
    // 重试那轮再放宽到 8s（视窗刚唤醒 / broker 重启时首轮特别慢）。
    m_initTimer.setInterval(5000);
    connect(&m_initTimer, &QTimer::timeout, this, &GlobalMediaSessionListener::onInitTimeout);
}

// 析构顺序很关键：先断掉一切可能回调进来的入口，再释放 WinRT 对象。
// 反过来做的话，Release 过程中触发的回调会打到已进入析构的 this 上。
GlobalMediaSessionListener::~GlobalMediaSessionListener()
{
    // 1. 先置存活标志为 false：所有在飞的协程恢复后、所有迟到的 COM 回调进入后都会立即放弃
    m_alive->store(false);

    // 2. 停定时器，阻止析构期间再派发新的轮询
    m_pollTimer.stop();

    // 3. 退订管理器级事件
    try
    {
        if (m_manager)
        {
            if (m_sessionsChangedToken)
                m_manager.SessionsChanged(m_sessionsChangedToken);
            if (m_currentSessionChangedToken)
                m_manager.CurrentSessionChanged(m_currentSessionChangedToken);
        }
    }
    catch (...) {}
    m_sessionsChangedToken = {};
    m_currentSessionChangedToken = {};

    // 4. 退订会话级事件。
    //    必须退订"当初订阅的那个会话对象"，而不是 m_manager.GetCurrentSession()——
    //    当前会话可能已经切走了，用错对象退订会让旧回调一直挂在别人身上，
    //    对象销毁后回调仍会触发 [this] 捕获 → 悬垂访问。
    try
    {
        if (m_subscribedSession)
        {
            if (m_curPlaybackToken)
                m_subscribedSession.PlaybackInfoChanged(m_curPlaybackToken);
            if (m_curMediaToken)
                m_subscribedSession.MediaPropertiesChanged(m_curMediaToken);
        }
    }
    catch (...) {}
    m_curPlaybackToken = {};
    m_curMediaToken = {};
    m_subscribedSession = nullptr;

    // 5. 最后释放管理器（此时已无任何回调入口）
    m_manager = nullptr;
}

// 执行一次控制指令。这里刻意不捕获 this：协程只依赖返回值，对象没了也不影响它跑完。
// @param op   播放器返回的布尔异步操作；false 表示播放器拒绝执行。
// @param what 指令名，仅用于日志定位是哪条控制失败。
winrt::fire_and_forget GlobalMediaSessionListener::runAsync(IAsyncOperation<bool> op,
                                                            const char *what)
{
    try
    {
        bool ok = co_await op; // ok 为播放器对 TryXxxAsync 的应答：false = 不支持/拒绝执行，不是异常
        if (!ok)
            qWarning() << QString("SMTC 控制 %1 被播放器拒绝").arg(QString::fromLatin1(what));
    }
    catch (const hresult_error &e)
    {
        // 按 8 位十六进制打印 HRESULT，和 WinDbg / 系统错误码文档对得上
        qWarning() << QString("SMTC 控制 %1 失败: 0x%2")
                          .arg(QString::fromLatin1(what))
                          .arg(static_cast<quint32>(e.code()), 8, 16, QLatin1Char('0'));
    }
    catch (...) {}
    co_return;
}

// 启动入口：同步部分只「发起」请求，真实结果通过信号回来。
// 每次调用都重置重试计数，因此用户可以手动重试（例如重启播放器后再点一次）。
bool GlobalMediaSessionListener::start()
{
    m_lastError.clear();
    m_initRetries = 0;
    m_initTimer.start();
    try
    {
        RequestAndInit();
        return true;
    }
    catch (...)
    {
        // 只有「连协程都没能启动」才会走到这里（例如 STA 未初始化导致 WinRT 直接抛）
        m_lastError = QStringLiteral("启动全局会话监听异常");
        return false;
    }
}

// 申请管理器并完成订阅。这是整个类唯一的初始化路径，也是唯一可能长时间挂起的地方：
// RequestAsync 背后是系统 broker（跨进程），broker 卡死时该操作可能永不完成，
// 所以外部还有 m_initTimer 看门狗兜底。
winrt::fire_and_forget GlobalMediaSessionListener::RequestAndInit()
{
    // 协程帧里持有存活标志的拷贝：对象析构后恢复也不会读写已释放的 this
    auto alive = m_alive;
    try
    {
        // manager 是协程帧里的局部变量：RequestAsync 的返回值先落在这里，等切回 GUI 线程
        // 并判活之后才赋给 m_manager，避免在错误的线程上碰成员
        auto manager = co_await GlobalSystemMediaTransportControlsSessionManager::RequestAsync();

        // 关键：RequestAsync 的续体默认在线程池线程上继续，
        // 而下面要做的事（赋值管理器、订阅事件、启动 QTimer、emit 信号）
        // 全都在 Qt GUI 线程的主场，必须先切回 GUI 线程。
        co_await ResumeOnGuiThread{};

        if (!alive->load())
            co_return;

        // 过了这一行才进入「可以碰 this」的安全区：上面的 manager 是协程帧局部量，
        // 在此之前任何一步都不得读写成员（含日志用的成员）。
        m_manager = manager;

        // 拿到管理器就立刻停看门狗：后面即使订阅步骤抛异常，也不需要再触发重试
        if (m_manager)
            m_initTimer.stop();

        if (!m_manager)
        {
            // RequestAsync 理论上不会返回空管理器，但接口是 WinRT 对象，防御性判空
            m_lastError = QStringLiteral("RequestAsync 返回空管理器");
            co_return;
        }

        // 事件回调由 COM 在任意线程投递，这里统一 post 回 GUI 线程再处理，
        // 避免在 COM 回调栈里直接操作 QObject
        m_sessionsChangedToken = m_manager.SessionsChanged(
            [this, alive](GlobalSystemMediaTransportControlsSessionManager const &,
                          IInspectable const &)
            {
                if (!alive->load())
                    return;
                postToGui([this, alive] {
                    if (!alive->load())
                        return;
                    emit sessionsChanged();
                    refreshAvailableSessions();
                });
            });

        m_currentSessionChangedToken = m_manager.CurrentSessionChanged(
            [this, alive](GlobalSystemMediaTransportControlsSessionManager const &,
                          CurrentSessionChangedEventArgs const &)
            {
                if (!alive->load())
                    return;
                postToGui([this, alive] {
                    if (!alive->load())
                        return;
                    // 系统焦点换了播放器 → 事件订阅也得跟着换，否则新播放器的
                    // 播放/元数据变化不会再有回调（只剩 500ms 轮询兜底）
                    emit sessionsChanged();
                    refreshAvailableSessions();
                    // 订阅可能因会话刚被关闭而抛；吞掉即可，轮询这条兜底路径仍在
                    try { subscribeCurrentSession(currentSession()); } catch (...) {}
                });
            });

        // 首帧订阅：此时可能还没有任何会话（没开播放器），nullptr 时 subscribe 内部会直接返回
        try { subscribeCurrentSession(currentSession()); } catch (...) {}

        // 轮询必须在订阅之后启动：事件负责"及时"，轮询负责"兜底"
        m_pollTimer.start();
        // 首帧主动推一次列表与信号，UI 不必等下一次事件或 500ms 轮询才拿到初始状态
        emit sessionsChanged();
        refreshAvailableSessions();
    }
    catch (const hresult_error &e)
    {
        // 停看门狗：失败已经定性，不必再等超时；原因先记下来，
        // 是否对外 emit unavailable 由函数末尾按「有没有管理器」统一决定
        m_initTimer.stop();
        m_lastError = QStringLiteral("HRESULT 0x%1：%2")
                          .arg(static_cast<quint32>(e.code()), 8, 16, QLatin1Char('0'))
                          .arg(QString::fromWCharArray(e.message().c_str()));
    }
    catch (const std::exception &e)
    {
        m_initTimer.stop();
        m_lastError = QStringLiteral("异常：%1").arg(QString::fromUtf8(e.what()));
    }

    // 只有「彻底没有管理器」才对外报不可用；已拿到管理器再出错不影响基本功能
    if (!m_lastError.isEmpty() && !m_manager)
        emit unavailable(m_lastError);
}

// 看门狗：RequestAsync 5 秒没回来就重试一次，两次都不行才判定 broker 卡死。
// 注意这里不 reset m_lastError —— 若上一轮已写入具体原因，优先把原因透给用户。
void GlobalMediaSessionListener::onInitTimeout()
{
    if (m_manager)
        return;

    // RequestAsync 偶尔只是慢，先重试一次；两次都失败才认为是 broker 卡住。
    // 后置自增：本次用旧值判断（0 → 进入重试），退出时把计数推进到 1；下次再进就是 1 → 走失败分支
    if (m_initRetries++ < 1)
    {
        RequestAndInit();
        m_initTimer.start(8000); // 第二次等更久：系统刚唤醒/broker 重启时首轮特别慢
        return;
    }

    if (m_lastError.isEmpty())
        m_lastError = QStringLiteral("系统媒体会话无响应，重启播放器或电脑后可恢复");
    emit unavailable(m_lastError);
}

// 把工作投递到本对象所属线程（GUI 线程）执行；对象已销毁则丢弃
void GlobalMediaSessionListener::postToGui(std::function<void()> fn)
{
    if (!m_alive->load() || !fn)
        return;
    // 以 this 作 context：对象析构后 Qt 会连同尚未执行的排队调用一起丢弃，
    // 不会把闭包派发到已释放的对象上；这里先判 alive 只是让丢弃尽早发生、少排一个事件。
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

// 订阅当前会话的播放状态/媒体属性/时间线变化
void GlobalMediaSessionListener::subscribeCurrentSession(
    GlobalSystemMediaTransportControlsSession session)
{
    if (!session)
        return;

    // 先退订"上次订阅的那个会话"（可能和这次的 session 不是同一个）
    if (m_subscribedSession && (m_curPlaybackToken || m_curMediaToken))
    {
        try
        {
            if (m_curPlaybackToken)
                m_subscribedSession.PlaybackInfoChanged(m_curPlaybackToken);
            if (m_curMediaToken)
                m_subscribedSession.MediaPropertiesChanged(m_curMediaToken);
        }
        catch (...) {}
    }
    m_curPlaybackToken = {};
    m_curMediaToken = {};
    m_subscribedSession = nullptr;

    // 回调里不能直接干活（COM 线程），只投递一次 poll；这里采用 [this] 裸捕获而不带 alive，
    // 是因为 postToGui 内部会判 m_alive —— 但投递后对象仍可能销毁，pollCurrentSession
    // 自身入口还会再判一次，双保险。
    m_curPlaybackToken = session.PlaybackInfoChanged(
        [this](GlobalSystemMediaTransportControlsSession const &,
               PlaybackInfoChangedEventArgs const &)
        {
            postToGui([this] { pollCurrentSession(); });
        });

    m_curMediaToken = session.MediaPropertiesChanged(
        [this](GlobalSystemMediaTransportControlsSession const &,
               MediaPropertiesChangedEventArgs const &)
        {
            postToGui([this] { pollCurrentSession(); });
        });

    m_subscribedSession = session;
}

// 轮询当前会话，做进度外推后发出 currentMediaChanged
void GlobalMediaSessionListener::pollCurrentSession()
{
    if (!m_alive->load() || !m_manager)
        return;

    try
    {
        auto session = currentSession();
        if (!session)
            return; // 没有在播的会话：保持上一次状态不发信号，避免 UI 被清空闪一下

        // 回调里捕获 alive 的拷贝（而不是 this 的成员），对象已析构则整段丢弃
        visitSessionAsync(session, [this, alive = m_alive](const QString &appId, const QString &title,
                                          const QString &artist, const QString &album,
                                          bool isPlaying, qint64 pos, qint64 posAge,
                                          qint64 dur,
                                          bool seekable, const QByteArray &coverData)
        {
            if (!alive->load())
                return;

            // 把播放器上报的原始位置换算成"当前应显示的位置"（暂停期间不推进）
            const qint64 shownPos = smoothPosition(appId, title, pos, posAge, isPlaying, dur);
            emit currentMediaChanged(appId, title, artist, album, isPlaying, shownPos, dur, seekable, coverData);
        });
    }
    catch (...) {}
}

// 把 SMTC 上报的原始播放位置换算成界面要显示的位置。
//
// 两个坑都要避开：
// 1) Position 是"播放器上报那一刻"的位置，不是此刻的位置。播放器可能很久才报一次
//    （浏览器甚至整首歌只在开始/暂停/seek 时报），首次采样时不补这段延迟，
//    界面就会固定落后一截 —— 播放器 1:05、界面 1:03，一直差这两秒。
// 2) LastUpdatedTime 期间如果发生过暂停，暂停时长会被算成播放时长，
//    于是"暂停在 00:33 → 点播放瞬间跳到 00:35 → 又跌回 00:33"。
// 做法：只在"建立新锚点"（首帧、换曲、换播放器）时用 LastUpdatedTime 补延迟，
// 之后一律用本地单调时钟推进，暂停期间冻结，恢复播放从冻结位置继续。
//
// @param appId/title  共同构成"当前曲目"的 key，任一变化都会重建锚点。
// @param rawPos       原始位置 ms，< 0 表示无时间线（原样返回）。
// @param rawAgeMs     上报延迟 ms，仅在新锚点时用于补偿，超出 [1, 600000) 视为异常忽略。
// @param isPlaying    本次观测的播放状态。
// @param durationMs   总时长 ms，> 0 时用于夹上界，<= 0 表示未知。
// @return 显示用位置 ms（>= 0），或用 -1 表示无时间线。
qint64 GlobalMediaSessionListener::smoothPosition(const QString &appId, const QString &title,
                                                  qint64 rawPos, qint64 rawAgeMs,
                                                  bool isPlaying, qint64 durationMs)
{
    if (rawPos < 0)          // 播放器不提供时间线：交给上层按"无进度"处理
        return rawPos;

    if (!m_playClock.isValid())
        m_playClock.start();

    const qint64 now = m_playClock.elapsed(); // 本轮唯一的时间基准：后面建锚/重锚都用它，避免同轮内取两次造成偏差
    // '\x1' 作分隔符：标题里不可能出现控制字符，避免 "AB"+"C" 与 "A"+"BC" 撞 key
    const QString key = appId + QChar(0x1) + title;

    // 播放器这次是否真的报了个新位置（Position 不变说明只是陈旧的旧值）
    const bool rawChanged = (rawPos != m_lastRawPos);
    m_lastRawPos = rawPos;

    const bool newTrack = (key != m_anchorKey || m_anchorPos < 0);

    // 只有建立新锚点时才补"上报延迟"：此时不存在"暂停过"的歧义，
    // 而连播途中本地时钟已经在推进，再补一次就会重复计时。
    // 上限 600000ms（10 分钟）用于挡掉 LastUpdatedTime 异常（时区/长时间休眠）导致的离谱补偿。
    qint64 base = rawPos; // 本轮采信的基准位置：默认原样用播放器上报值，只有新锚点才补上报延迟
    if (isPlaying && newTrack && rawAgeMs > 0 && rawAgeMs < 600000)
        base = rawPos + rawAgeMs;

    qint64 shown = base; // 最终显示位置；下面会按「暂停/恢复/在播」三种情形改写，末尾再夹紧

    if (newTrack)   // 首帧 / 换曲 / 换播放器 → 重新奠基
    {
        m_anchorKey = key;
        m_anchorPos = base;
        m_anchorAt = now;
        m_anchorPlaying = isPlaying;
    }
    else
    {
        // 从锚点推算到现在应有的位置：上次观测时在播放才推进，否则原地不动
        const qint64 predicted = m_anchorPos + (m_anchorPlaying ? now - m_anchorAt : 0);

        // 阈值 1500ms 是"可信跳转"的门槛：小于它的偏差属于采样抖动/上报粒度，
        // 采信播放器反而会让进度条来回抖，所以一律以推算值为准。
        if (!isPlaying)
        {
            // 暂停：冻在当前显示位置。只有播放器报了个差异明显的新位置
            // （期间被拖动/跳转过）才采信它，避免陈旧值把进度拉回去。
            shown = (rawChanged && qAbs(rawPos - predicted) > 1500) ? rawPos : predicted;
            m_anchorPlaying = false;
        }
        else if (!m_anchorPlaying)
        {
            // 暂停 → 播放：从冻结位置接着走，绝不把暂停的那段时间加进去
            shown = (rawChanged && qAbs(rawPos - predicted) > 1500) ? rawPos : m_anchorPos;
            m_anchorPlaying = true;
        }
        else
        {
            // 一直在播：按本地时钟推进（播放器可能根本不刷新 Position）；
            // 若这次上报的位置与推算值相差过大，说明发生了 seek/跳转，以播放器为准。
            shown = (rawChanged && qAbs(rawPos - predicted) > 1500) ? rawPos : predicted;
        }

        // 每轮都用当前结果重锚：既避免 now - anchorAt 无界增长，也让"临时采信播放器值"
        // 之后能立刻从新位置继续外推。
        m_anchorPos = shown;
        m_anchorAt = now;
    }

    // 夹紧：部分播放器会把 Position 报过结尾（或 EndTime 小于 Position）
    if (durationMs > 0 && shown > durationMs)
        shown = durationMs;
    if (shown < 0)
        shown = 0;

    return shown;
}

// 解析出目标会话：优先用户选中的，否则系统"当前会话"
GlobalSystemMediaTransportControlsSession GlobalMediaSessionListener::currentSession()
{
    if (!m_manager)
        return nullptr;

    // 未选择时，跟随系统当前会话
    if (m_selectedAppId.isEmpty())
        return m_manager.GetCurrentSession();

    // 已选择时，在所有会话中按 appId 匹配
    try
    {
        auto sessions = m_manager.GetSessions(); // 只读视图，顺序由系统决定，不能拿索引当优先级用
        for (auto const &session : sessions)
        {
            // AUMID 是 UTF-16，逐字转 QString 后做全等比较（不做模糊匹配，避免选错播放器）
            QString appId = QString::fromWCharArray(session.SourceAppUserModelId().c_str());
            if (appId == m_selectedAppId)
                return session;
        }
    }
    catch (...) {}

    // 找不到选中的会话（可能已退出）→ 回退到系统当前会话
    return m_manager.GetCurrentSession();
}

// 枚举当前所有可用播放器，通过 availableSessions 信号返回
void GlobalMediaSessionListener::refreshAvailableSessions()
{
    if (!m_manager)
        return;

    QList<SessionEntry> result; // 先攒完再发：中途抛异常时发出去的是完整列表，不会是半截
    try
    {
        auto sessions = m_manager.GetSessions();
        for (auto const &session : sessions)
        {
            SessionEntry entry;
            entry.appId = QString::fromWCharArray(session.SourceAppUserModelId().c_str());
            entry.appName = appNameFromAppId(entry.appId);
            result.append(entry);
        }
    }
    catch (...) {}

    emit availableSessions(result);
}

// 选择要控制的播放器
void GlobalMediaSessionListener::selectSession(const QString &appId)
{
    m_selectedAppId = appId;

    // 切换目标后立即重新订阅事件 + 刷新一次，UI 能马上反映新目标
    // （不这样做的话，要等到下一次 sessionsChanged 或 500ms 轮询才有反应）
    try
    {
        subscribeCurrentSession(currentSession());
    }
    catch (...) {}
    pollCurrentSession();
}

// 读单个会话的完整信息。字段分三段 try 各自兜底：任一段失败不应连累其他段
// （例如播放器不支持缩略图时，标题/歌手仍然要能显示）。
winrt::fire_and_forget GlobalMediaSessionListener::visitSessionAsync(
    GlobalSystemMediaTransportControlsSession session, SessionVisitor visitor)
{
    auto alive = m_alive;
    // appId 在第一次 co_await 之前读完：SourceAppUserModelId 是同步属性，此时还在调用者线程上，
    // 且后续协程在哪个线程恢复都不影响这个已经拷贝好的 QString
    QString appId = QString::fromWCharArray(session.SourceAppUserModelId().c_str());
    // 先全部给默认值：任何一段读取失败时，回调拿到的就是这些「未知」占位
    QString title, artist, album;
    bool isPlaying = false;
    bool seekable = false;
    qint64 positionMs = -1, durationMs = -1, positionAgeMs = -1;
    QByteArray coverData;

    // 第一段：播放状态 + 是否允许 seek（同步接口，不挂起）
    try
    {
        auto info = session.GetPlaybackInfo();
        isPlaying = (info.PlaybackStatus() ==
                     GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
        seekable = info.Controls().IsPlaybackPositionEnabled();
    }
    catch (...) {}

    // 第二段：时间线（同步接口，不挂起）
    try
    {
        auto tl = session.GetTimelineProperties(); // 时间线快照；字段是 TimeSpan（100ns tick），不是毫秒
        auto pos = tl.Position();                  // 播放器上报的绝对播放位置（相对曲目开头）
        auto start = tl.StartTime();               // 曲目起点，多数播放器为 0，流媒体切片可能非 0
        auto end = tl.EndTime();                   // 曲目终点；时长必须用 end - start，不能直接用 end

        // 这里只取播放器上报的原始位置，不做任何推算。
        // 顺便记下"这个位置是多久以前上报的"：播放器的 Position 是上报那一刻的位置，
        // 不是此刻的位置，首次采样时若不补这段延迟，界面就会固定落后一截。
        positionMs = std::chrono::duration_cast<std::chrono::milliseconds>(pos).count();
        durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        positionAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            winrt::clock::now() - tl.LastUpdatedTime()).count();
        if (durationMs < 0)
            durationMs = -1; // 用 -1 而不是 0：0 会被上层当成"时长 0"，-1 才是"未知"
        if (positionMs < 0)
            positionMs = 0;
    }
    catch (...) {}

    // 第三段：元数据 + 封面（异步，co_await 之后续体已在别的线程）
    try
    {
        auto props = co_await session.TryGetMediaPropertiesAsync();
        title = QString::fromWCharArray(props.Title().c_str());
        artist = QString::fromWCharArray(props.Artist().c_str());
        album = QString::fromWCharArray(props.AlbumTitle().c_str());

        auto thumbRef = props.Thumbnail();
        if (thumbRef)
        {
            auto stream = co_await thumbRef.OpenReadAsync();
            if (stream)
            {
                // 封面读取（最稳妥写法）：
                // 用 LoadAsync 的实际返回字节数（GetResults）分配缓冲区并读取，
                // 不信任 Size()/UnconsumedBufferLength，从根源上杜绝堆越界写。
                // 结果以 QByteArray（纯字节）保存，跨线程绝对安全，GUI 线程再解码。
                const uint64_t size = stream.Size();
                // 上限 64MB：封面正常只有几百 KB，超过这个量级说明数据异常（或是占位大图），
                // 截到 64MB 既能读完也不会让内存炸掉。
                const uint64_t capped = (size > 0 && size <= 64 * 1024 * 1024)
                                            ? size : 64 * 1024 * 1024;
                const uint32_t len = static_cast<uint32_t>(capped);

                DataReader reader(stream);
                const uint32_t loaded = co_await reader.LoadAsync(len);

                if (loaded > 0)
                {
                    // 按 loaded（实际读到的字节数）分配，绝不按 Size() 分配：
                    // 两者不一致时按 Size 分配+按 loaded 读会浪费，反过来则越界。
                    std::vector<uint8_t> buf(loaded);
                    reader.ReadBytes(buf);
                    coverData = QByteArray(reinterpret_cast<const char *>(buf.data()),
                                           static_cast<int>(buf.size()));
                }
            }
        }
    }
    catch (...) {}

    // 关键的最后一跳：上面三个 winrt co_await 让续体落在了线程池线程上，
    // 而下面要回调 visitor（内部会 emit 信号、更新 QWidget），必须回到 GUI 线程。
    co_await ResumeOnGuiThread{};

    // 协程可能在对象析构后才恢复（fire_and_forget 无生命周期绑定），
    // 此处必须检查存活标志，避免悬垂访问 this/emit
    if (!alive->load())
        co_return;

    if (visitor)
        visitor(appId, title, artist, album, isPlaying, positionMs, positionAgeMs,
                durationMs, seekable, coverData);

    co_return;
}

// ---- 控制方法 ----
// 所有控制入口都是同一套模板：无管理器就返回 → 取当前会话 → 发起 TryXxxAsync 交给 runAsync 收尾。
// 同步 try/catch 只兜住"取会话/发起调用"这一步的 COM 异常，异步结果由 runAsync 处理。

void GlobalMediaSessionListener::play()
{
    if (!m_manager) return; // 没管理器 = 初始化没成功，前面已按 unavailable 报过，这里静默丢弃即可
    try
    {
        auto s = currentSession();
        if (s) runAsync(s.TryPlayAsync(), "play"); // 播放器可能拒绝（如无音源），结果由 runAsync 记日志
    }
    catch (...) {}
}

// 暂停。作用对象是 currentSession()：用户选中的播放器已退出时会落到系统焦点会话上，
// 因此「点了暂停却暂停了别的播放器」是有可能的，UI 侧应以 currentMediaChanged 回报的 appId 为准。
void GlobalMediaSessionListener::pause()
{
    if (!m_manager) return;
    try
    {
        auto s = currentSession();
        if (s) runAsync(s.TryPauseAsync(), "pause");
    }
    catch (...) {}
}

// 播放/暂停二合一。比先查状态再调 play/pause 更可靠：查状态与执行之间状态可能已变，
// 而 TryTogglePlayPauseAsync 由播放器原子地完成翻转。
void GlobalMediaSessionListener::toggle()
{
    if (!m_manager) return;
    try
    {
        auto s = currentSession();
        if (s) runAsync(s.TryTogglePlayPauseAsync(), "toggle");
    }
    catch (...) {}
}

// 下一首。只发出请求；切歌后的标题/封面/时长全部靠事件或 500ms 轮询回来，这里不做本地预判。
void GlobalMediaSessionListener::next()
{
    if (!m_manager) return;
    try
    {
        auto s = currentSession();
        if (s) runAsync(s.TrySkipNextAsync(), "next");
    }
    catch (...) {}
}

// 上一首。具体是「上一曲」还是「回到本曲开头」由播放器自己决定，本程序不做补偿或改写。
void GlobalMediaSessionListener::previous()
{
    if (!m_manager) return;
    try
    {
        auto s = currentSession();
        if (s) runAsync(s.TrySkipPreviousAsync(), "previous");
    }
    catch (...) {}
}

// 停止。不少播放器并不实现该动作（TryStopAsync 返回 false），因此失败只记日志、不向用户报错。
void GlobalMediaSessionListener::stop()
{
    if (!m_manager) return;
    try
    {
        auto s = currentSession();
        if (s) runAsync(s.TryStopAsync(), "stop");
    }
    catch (...) {}
}

// 跳转。注意 SMTC 只认"绝对位置"，不支持相对快进/快退，调用方需自行换算。
void GlobalMediaSessionListener::seek(qint64 positionMs)
{
    if (!m_manager) return;
    try
    {
        auto s = currentSession();
        if (s)
        {
            // TryChangePlaybackPositionAsync 参数单位是 100 纳秒 tick（TimeSpan）
            int64_t ticks = positionMs * 10000LL;
            runAsync(s.TryChangePlaybackPositionAsync(ticks), "seek");
        }
    }
    catch (...) {}
}
