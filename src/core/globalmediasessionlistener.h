// -----------------------------------------------------------------------------
// globalmediasessionlistener —— 全局媒体会话（GSMTC）监听与控制的头文件。
// 职责：把 Windows 的 GlobalSystemMediaTransportControlsSessionManager 包成 Qt 信号槽，
//       向 UI 提供「谁在放什么、放到哪了」以及播放/暂停/切歌/seek 能力。
// 约定：整条链路跑在 Qt GUI 线程（STA）；所有 winrt 异步续体必须切回 GUI 线程再碰 this。
// 坑1：COM 异步回复可能在对象析构后才到达。存活标志必须用 shared_ptr<atomic<bool>> 拷贝捕获，
//      直接读成员 atomic 本身就是 UAF，后续写成员会写坏堆块尾部（HEAP CORRUPTION）。
// 坑2：RequestAsync 可能因系统 broker 卡死永不返回 → 5s 看门狗 + 1 次重试，仍失败发 unavailable。
// -----------------------------------------------------------------------------
#ifndef GLOBALMEDIASESSIONLISTENER_H
#define GLOBALMEDIASESSIONLISTENER_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QMetaType>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <atomic>
#include <memory>

#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

/**
 * 监听并控制系统全局媒体会话（Win11 专有）。
 *
 * 能力：
 *   - 读：标题/歌手/专辑/播放状态/进度(时间线)/封面缩略图
 *   - 控：播放/暂停/切换/上一首/下一首/停止/seek
 *
 * 限制：
 *   - 仅 Windows 11（build 22000+）；
 *   - 必须在 STA 线程使用（Qt GUI 主线程）；
 *   - 歌词无法通过 SMTC 全局会话获取（Windows 未提供歌词字段）；
 *   - 控制方法需播放器支持（TryXxxAsync 返回 false 表示被拒绝）。
 */
class GlobalMediaSessionListener : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造监听器
     * @param parent Qt 父对象，通常传主窗口；本对象会随父对象一起销毁。
     *
     * 构造只做 Qt 侧准备（注册元类型、给轮询/看门狗定时器配参数并接上槽），
     * 不会碰 WinRT —— 真正申请会话管理器在 start() 里做，这样构造永远不会抛异常。
     */
    explicit GlobalMediaSessionListener(QObject *parent = nullptr);
    /**
     * @brief 析构
     *
     * 顺序是安全前提：先置 m_alive=false（掐断所有在飞回调）→ 停轮询 → 退订管理器级事件
     * → 用 m_subscribedSession（而非当前会话）退订会话级事件 → 最后释放管理器。
     * 任何一步提前都会让 Release 过程中的回调打到半销毁的 this 上。
     */
    ~GlobalMediaSessionListener() override;

    /**
     * @brief 启动监听
     * @return 是否成功「发起」了初始化请求。注意 true 只代表协程已启动，
     *         真正的成功/失败要等 currentMediaChanged 或 unavailable 信号。
     *
     * 可重复调用：会清空上次错误、重置重试计数、重启 5s 看门狗并重新 RequestAsync。
     * 副作用：成功后会启动 500ms 轮询定时器、订阅管理器级事件。
     */
    bool start();
    /** @return 最近一次失败原因（可直接展示）；从未失败过则为空串。 */
    QString lastError() const { return m_lastError; }

    // 一个可用的媒体会话（播放器）
    struct SessionEntry
    {
        QString appId;     // SourceAppUserModelId，唯一标识（如 NetEase.CloudMusic_...）
        QString appName;   // 从 appId 解析出的可读名称；属启发式结果（见 appNameFromAppId），
                           // 可能与播放器自报名称不一致，只用于下拉框展示，不要拿它做匹配
    };

    // ---- 读取（同步部分：appId + 播放状态 + 时间线）----
    /**
     * 单次会话信息读取完成后的回调签名。
     *
     * @param appId         会话的 AUMID，可回传给 selectSession() 锁定该播放器。
     * @param title         歌曲标题；读不到时为空串。
     * @param artist        歌手；读不到时为空串。
     * @param album         专辑名；读不到时为空串。
     * @param isPlaying     当前是否 Playing（其余状态含 Paused/Stopped/Closed 一律为 false）。
     * @param positionMs    播放器上报的「原始」位置（毫秒），未做任何外推；
     *                      播放器不提供时间线时为 -1。
     * @param positionAgeMs 距该位置上报时刻已过去多久（毫秒），用于首次采样时补延迟；
     *                      与 positionMs 同为 -1 表示无时间线。
     * @param durationMs    曲目总时长（毫秒，EndTime - StartTime）；未知或非法时为 -1。
     * @param seekable      播放器是否允许 seek（IsPlaybackPositionEnabled）。
     * @param coverData     封面原始字节（PNG/JPEG，未解码，上限 64MB）；无封面时为空。
     *
     * 回调保证在 GUI 线程触发；对象已析构时不会被调用。
     */
    using SessionVisitor = std::function<void(const QString &appId,
                                              const QString &title,
                                              const QString &artist,
                                              const QString &album,
                                              bool isPlaying,
                                              qint64 positionMs,
                                              qint64 positionAgeMs,   // 距播放器上报该位置过了多久
                                              qint64 durationMs,
                                              bool seekable,
                                              const QByteArray &coverData)>;

    /**
     * 枚举当前所有可用的媒体会话（播放器）列表，通过 availableSessions 信号返回
     *
     * 无管理器时静默返回（不发信号）；每次调用都会发一次 availableSessions，
     * 即使列表为空，所以调用方可以把它当成「刷新」按钮的入口。
     */
    void refreshAvailableSessions();

    /**
     * 选择要控制的播放器（按 appId 精确匹配）。传空串则回退到系统"当前会话"。
     *
     * @param appId 目标会话的 AUMID；空串表示跟随系统焦点会话。
     *              传了但匹配不到（播放器已退出）时，内部自动回退到系统当前会话。
     *
     * 副作用：立即重新订阅事件并同步触发一次 pollCurrentSession()，让 UI 马上反映新目标。
     */
    void selectSession(const QString &appId);

    // ---- 控制（作用于当前会话）----
    // 以下控制方法全部是「发出即忘」：不阻塞、不抛异常，失败只打 qWarning。
    // 当前会话不存在或管理器未就绪时静默无操作。播放器不支持的指令会被 TryXxxAsync
    // 返回 false（例如有些播放器不允许 seek），调用方不要假设一定成功。
    /**
     * 请求播放（底层 TryPlayAsync）。
     * 已在播放时是否算成功、是否有副作用由播放器决定（多数播放器直接返回 true 不做动作）。
     */
    void play();
    /**
     * 请求暂停（底层 TryPauseAsync）。
     * 作用对象是 currentSession()：用户选中的播放器已退出时会落到系统焦点会话上，
     * 因此「点了暂停却暂停了别的播放器」是有可能的，UI 应以 currentMediaChanged 回报的 appId 为准。
     */
    void pause();
    /**
     * 播放/暂停二合一（底层 TryTogglePlayPauseAsync）。
     * 比「先查状态再调 play/pause」可靠：查状态与执行之间状态可能已变，这里由播放器原子翻转。
     */
    void toggle();
    /**
     * 下一首（底层 TrySkipNextAsync）。
     * 只发出请求；新曲的标题/封面/时长靠事件或 500ms 轮询回填，本地不做预判。
     */
    void next();
    /**
     * 上一首（底层 TrySkipPreviousAsync）。
     * 具体是「真的上一曲」还是「回到本曲开头」由播放器自己决定，本程序不做补偿。
     */
    void previous();
    /**
     * 停止（底层 TryStopAsync）。
     * 相当多播放器并未实现该动作，返回 false 属常态，因此失败只记 qWarning、不向用户报错。
     */
    void stop();
    /**
     * @brief 跳转到指定位置
     * @param positionMs 目标位置（毫秒）。本函数不校验范围：负数会原样换算成负的 tick
     *                   传给播放器，超长的值也不会提前夹到 duration；夹紧/忽略由播放器决定。
     * 实际调用 TryChangePlaybackPositionAsync，内部按 100ns tick 换算。
     */
    void seek(qint64 positionMs); // 跳转到指定位置（毫秒）

signals:
    // 会话列表/当前会话变化。两种时机都会发：播放器启动/退出（SessionsChanged）、
    // 系统焦点会话切换（CurrentSessionChanged）。信号本身不带数据，要列表请调 refreshAvailableSessions()。
    void sessionsChanged();
    // 可用播放器列表（供 UI 下拉框使用）
    // sessions 是「本次枚举的完整快照」：空列表代表当前系统里一个会话都没有，
    // 而不是「没变化」——填充下拉框时应当整体替换，不要与旧列表合并。
    void availableSessions(const QList<SessionEntry> &sessions);
    // 系统媒体会话连不上（初始化超时/失败）。reason 可直接展示给用户
    // 只在「始终没拿到管理器」时发出；已成功初始化后再出错不会发（见 RequestAndInit 末尾）。
    // reason 与 lastError() 是同一个字符串，错过信号的一方可以事后补取。
    void unavailable(const QString &reason);
    /**
     * 当前会话媒体信息变化（携带完整信息；coverData 为封面原始字节，在 GUI 线程解码）
     *
     * positionMs 已经过进度平滑（暂停期间冻结、播放中按本地时钟推进），
     * 可直接用于画进度条，不要再自己按时间外推。
     * 有会话在播时至少每 500ms 发一次（轮询兜底），播放器事件触发时额外再发。
     */
    void currentMediaChanged(const QString &appId,
                             const QString &title,
                             const QString &artist,
                             const QString &album,
                             bool isPlaying,
                             qint64 positionMs,
                             qint64 durationMs,
                             bool seekable,
                             const QByteArray &coverData);

private:
    // 刷新当前会话（供 QTimer 轮询）
    // 三个入口都会走到这里：500ms 定时器、会话的 PlaybackInfoChanged/MediaPropertiesChanged
    // （经 postToGui 排队）、以及 selectSession()。因此入口处要再判一次存活与 m_manager，
    // 不能假设调用时状态一定有效。
    void pollCurrentSession();
    // RequestAsync 超时：系统 broker 卡住时不能让界面一直停在"未在播放"
    // 只在 m_manager 仍为空时才有意义；顺序是「先重试一次，再报 unavailable」，
    // 计数由 m_initRetries 控制，详见 .cpp。
    void onInitTimeout();
    /**
     * 由 SMTC 原始位置 + 本地播放时钟，算出用于显示的播放位置
     *
     * @param appId       会话标识，与 title 一起构成锚点 key，换播放器时重置锚点。
     * @param title       标题，同上；标题变即视为换曲。
     * @param rawPos      播放器上报的原始位置（ms）；< 0 表示无时间线，原样返回。
     * @param rawAgeMs    距上传时刻的延迟（ms）；仅在建立新锚点时用于补偿，<=0 或
     *                    >= 600000（10 分钟，明显异常）时忽略。
     * @param isPlaying   本次观测的播放状态，决定锚点时间是否继续累加。
     * @param durationMs  曲目总时长（ms），>0 时用于把结果显示上界夹到结尾；<=0 表示未知、不夹。
     * @return 应显示的位置（ms），保证 >= 0；无时间线时仍是 -1。
     *
     * 有状态：会读写 m_anchor* / m_lastRawPos / m_playClock，只在 GUI 线程调用。
     */
    qint64 smoothPosition(const QString &appId, const QString &title,
                          qint64 rawPos, qint64 rawAgeMs,
                          bool isPlaying, qint64 durationMs);
    /**
     * 把工作投递到本对象所属线程（GUI 线程）执行
     * @param fn 待执行闭包；为空、或对象已标记析构时不投递（直接丢弃）。
     * 注意：投递是排队语义，闭包真正执行时对象可能已析构，所以闭包内仍需自行判活。
     */
    void postToGui(std::function<void()> fn);
    /**
     * 解析出目标会话（优先用户选中的，否则系统当前会话）
     * @return 目标会话；管理器未就绪或没有任何会话时返回 nullptr。
     *         选中的 appId 找不到时（播放器退出）回退到系统当前会话。
     */
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession currentSession();
    /**
     * 执行一次播放器控制指令并吞掉所有异常。
     * @param op   播放器返回的布尔异步操作（TryPlayAsync 等）。
     * @param what 指令名（"play"/"pause"/"seek"...），仅用于失败日志，不参与逻辑。
     * 协程内部不碰 this，因此无需判活。
     */
    winrt::fire_and_forget runAsync(winrt::Windows::Foundation::IAsyncOperation<bool> op,
                                    const char *what);
    /**
     * 申请会话管理器并完成订阅（协程，fire_and_forget）。
     * 内部先捕获存活标志拷贝，co_await 之后一律先切回 GUI 线程再判活、再碰成员。
     * 失败时把原因写进 m_lastError，并（在完全没有管理器时）emit unavailable。
     */
    winrt::fire_and_forget RequestAndInit();
    /**
     * 异步读取单个会话完整信息并回调
     * @param session 目标会话，调用前须非空；协程内各字段读取失败时保持默认值（空串 / -1）。
     * @param visitor 结果回调，保证在 GUI 线程调用；对象已析构或 visitor 为空则不调用。
     */
    winrt::fire_and_forget visitSessionAsync(
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session,
        SessionVisitor visitor);
    /**
     * 订阅当前会话的事件
     * @param session 新的目标会话；为空则只做退订、不做订阅。
     *
     * 会先退订 m_subscribedSession（当初实际订阅的那个，可能已不是当前会话），
     * 再订阅新会话，最后把 m_subscribedSession 换成新对象——退订必须用这个字段。
     */
    void subscribeCurrentSession(
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session);

    /** GSMTC 会话管理器；nullptr = 尚未初始化成功，所有读写/控制入口都会短路 */
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager m_manager{ nullptr };
    /** 管理器级事件令牌：会话增删（播放器启动/退出） */
    winrt::event_token m_sessionsChangedToken;
    /** 管理器级事件令牌：系统焦点会话切换 */
    winrt::event_token m_currentSessionChangedToken;
    /** 当前会话级事件令牌：播放/暂停状态变化 */
    winrt::event_token m_curPlaybackToken;
    /** 当前会话级事件令牌：曲目元数据变化 */
    winrt::event_token m_curMediaToken;

    // 当前实际订阅的会话：退订时必须用这个对象，而不是"当前会话"（可能已切走）
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession m_subscribedSession{ nullptr };

    QTimer m_pollTimer; // 500ms 轮询当前会话，弥补事件上报不及时
    QTimer m_initTimer; // 初始化看门狗（单次触发）：RequestAsync 迟迟不返回时给出明确提示。
                        // 首轮 5s，重试那轮放宽到 8s（见 onInitTimeout）。
    /** 已重试次数：0 = 首次；1 = 已重试过一次。到 2 就不再重试，直接报不可用 */
    int m_initRetries = 0;

    // ---- 播放进度平滑用的本地锚点 ----
    // SMTC 的 Position 只在播放器主动上报时才变（浏览器可能整首歌只报一次），
    // 而 LastUpdatedTime 反映的是"上报时刻"，暂停过就会把暂停时长算成播放时长。
    // 因此改为：锚点 + 本地单调时钟，只在"上次观测和本次观测都在播放"时推进。
    QElapsedTimer m_playClock;   // 单调时钟（ms）：只取差值，不受系统时间被改/休眠抖动影响
    qint64 m_anchorPos = -1;     // 锚点位置（ms）；-1 表示尚未建立锚点（等价于「首帧」）
    qint64 m_anchorAt = 0;       // 锚点建立时的 m_playClock 读数（ms），与 m_anchorPos 配对做外推
    bool   m_anchorPlaying = false; // 建锚时是否在播放；false 表示锚点时间不推进（暂停冻结）
    qint64 m_lastRawPos = -1;    // 上一次 SMTC 原始位置，用于判断播放器是否真的更新了
    QString m_anchorKey;         // appId + 标题，换曲/换播放器时重置锚点

    QString m_selectedAppId; // 用户选中的播放器标识（空 = 跟随系统当前会话）

    // 存活标志：析构置 false。用 shared_ptr 而不是成员原子量 ——
    // COM 回调/协程可能在对象析构后才恢复，那时访问成员本身就已经是悬垂访问；
    // 捕获这个 shared 标志的拷贝，才能在对象消失后安全地判断"别再碰它了"。
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    /** 最近一次失败原因（人类可读，中文），供 lastError() 与 unavailable 信号使用 */
    QString m_lastError;
};

// 供 qRegisterMetaType / 跨线程排队连接使用
Q_DECLARE_METATYPE(GlobalMediaSessionListener::SessionEntry)

#endif // GLOBALMEDIASESSIONLISTENER_H
