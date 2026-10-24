#pragma once

#include "widgets/WidgetPanel.h"

#include <QElapsedTimer>
#include <QPixmap>
#include <QStringList>
#include <QVector>

class QGridLayout;
class QHBoxLayout;
class QLabel;
class CoverView;
class MarqueeLabel;
class LyricsView;
class QMenu;
class QSlider;
class QTimer;
class QToolButton;
class LyricFetcher;
class GlobalMediaSessionListener;

// MusicWidget：本文件负责「系统媒体会话 → 界面」的装配与尺寸换档，本身不含播放器。
// 数据只来自 GSMTC 回调（onMediaChanged），界面永远是被动的；没有 GSMTC 的构建（非 MSVC）
// 会退化成一条「不可用」提示，但仍要保证布局可用。
// 尺寸相关的所有取值都是离散档位（见 Metrics），不要引入 h*0.xx 这类连续公式：
// 连续公式会让每 1px 缩放都算作换档，触发重建封面 + 重新折行歌词，表现就是拖边缘时抖动。
// 重排入口只有 applyMetrics()；relayout() 只摆位置，不负责尺寸与可见性之外的逻辑。

/**
 * 音乐组件：接管**系统全局媒体会话**（GSMTC），
 * 显示任意播放器（网易云 / QQ音乐 / 酷狗 / 浏览器…）的歌名、歌手、专辑、封面、进度与歌词，
 * 并可直接播放/暂停、上一首/下一首、拖动进度。
 *
 * 默认版式：
 *      [旋转封面]   歌词（当前句高亮，长句自动换行，可显示上下文多行）
 *       歌名
 *       歌手 · 专辑
 *      03:18 ─────────────●──── 03:42
 *            ⟳    ⏮    ⏸    ⏭    🔊        ← 整行居中
 *
 * 歌名/歌手放在封面下方，超长时横向滚动（跑马灯）；窗口太小时退化成「歌名在封面右侧、不显示歌词」。
 *
 * 尺寸自适应：字号、封面直径、是否显示时间/上下曲/循环/音量、歌词行数都跟着内容区大小换档，
 * 不会把控件硬缩小导致文字被裁。
 * 「优先显示哪个播放器」只在右键菜单里选（界面上不再占用一行按钮）。
 */
class MusicWidget : public WidgetPanel
{
    Q_OBJECT

public:
    /**
     * 构造即完成全部装配（控件、信号、GSMTC 监听、300ms 心跳、首帧档位），没有单独的 init。
     * @param parent 父控件（桌面盒容器），所有权交给 Qt 父子机制
     * 构造内部的顺序有依赖：LyricsView 必须最先建、歌词抓取器要早于 buildUi 的信号连接、
     * m_applyTimer 要在任何 resize 之前就绪。调整顺序前先看构造函数里的注释。
     */
    explicit MusicWidget(QWidget *parent = nullptr);

    /**
     * 析构时主动断开 GSMTC 监听器到本控件的所有连接。
     * 监听器回调是异步投递的，不先断开可能在半析构状态下收到信号（进而访问已释放成员）。
     */
    ~MusicWidget() override;

    /** @return 面板标题，恒为「音乐」，不随当前播放器变化 */
    QString panelTitle() const override;
    /**
     * 标题栏右侧角标文字。
     * @return 优先当前播放器展示名；没有名字时退回状态词（「跟随系统」/「不可用」）
     */
    QString badgeText() const override;
    /** 外层定时/手动刷新：重新枚举会话、回到用户锁定的会话、再同步音量；不会重新拉歌词 */
    void refresh() override;
    /**
     * 持久化用户选择（播放器、循环模式、音量），不存「当前在放什么歌」。
     * @param o 由调用方持有并负责落盘的 JSON 对象，本函数只往里写键，不清理已有键
     */
    void saveState(QJsonObject &o) const override;
    /**
     * 恢复持久化状态；所有枚举/数值都会夹到合法区间，手改过的配置不会污染内部状态。
     * @param o 可能缺键的 JSON；缺 appId 视为「跟随系统」，此时不会触发重新落盘
     */
    void restoreState(const QJsonObject &o) override;
    /**
     * 往外部右键菜单里追加本组件的项（优先播放器 / 循环模式 / 重扫 / 重连）。
     * @param menu 调用方创建且会自行 show 的菜单；本函数只 addMenu/addAction，
     *             另外会给自建的子菜单就地套主题样式（QMenu 是顶层窗口，不继承父样式）
     * 菜单项的勾选态直接读内部状态，切换动作仍复用同一批成员函数以保证副作用一致。
     */
    void buildMenu(QMenu *menu) override;

    /** 低于此尺寸会落到「档 0」版式（见 metrics()）：只保证封面+标题+进度+控制能挤下 */
    QSize minimumSizeHint() const override { return QSize(168, 76); }
    /** 再大就只剩空白了，给个上限 */
    QSize maximumSizeHint() const override { return QSize(880, 680); }
    /** 折叠标题栏时边距可以放松一档（标题栏占了 42px，隐藏后纵向空间算法不同） */
    void setHeaderHidden(bool hidden) override;
    /**
     * 主题切换：把缓存在控件里的配色全部重设一遍。
     * QSS（panelCss）、MarqueeLabel 的文字色、歌词配色、染色图标都是在设置那一刻固定下来的，
     * 不会自己跟着 AppSettings 变——不刷新的话深色主题下文字还是浅色主题的颜色。
     * 只管颜色：几何由窗口负责，这里不碰尺寸。
     */
    void applyTheme() override;

protected:
    /**
     * 尺寸变化入口：只在档位真的变了时才排队一次重排（0ms 单次定时器合并连续 resize）。
     * @param e resize 事件，转发给基类后本函数不再使用其内容
     * 同档位内的纯尺寸变化不重排，交给布局自己拉伸；否则拖边缘会持续重建封面/重折歌词。
     */
    void resizeEvent(QResizeEvent *e) override;
    /** 只拦音量按钮的滚轮（滚轮微调音量）；其余事件全部交回基类 */
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    /**
     * 循环模式。取值只有 0..2，会被持久化到 JSON，同时被当作右键菜单里的下标使用，
     * 所以顺序不能随便调；新增取值要同步 restoreState 的夹取上界与 saveState 的注释。
     */
    enum RepeatMode { RepeatOff = 0, RepeatAll = 1, RepeatOne = 2 };

    /**
     * 一档版式的全部尺寸参数。
     * 所有像素值都是离散档位枚举出来的结果，禁止改成按宽高比例的连续公式：
     * 连续公式会让每 1px 缩放都产生新档位，触发重建圆形封面 + 全文重新折行，表现为缩放抖动。
     */
    struct Metrics
    {
        int marginX = 14;      // 网格左右边距（px）
        int marginY = 10;      // 网格上下边距（px）
        int gapX = 12;         // 网格列间距（px）
        int gapY = 4;          // 网格行间距（px）
        int cover = 56;        // 封面直径（px），同时决定 CoverView 固定尺寸
        int titlePx = 15;      // 歌名字号（px）
        int artistPx = 11;     // 歌手/专辑字号（px）
        int lyricPx = 12;      // 歌词正文字号（px），上下文行由 LyricsView 自行减 2
        int timePx = 10;       // 进度两端时间字号（px）
        int btn = 30;          // 普通按钮盒子边长（px）
        int playBtn = 36;      // 播放键盒子边长（px），比其它按钮大一圈
        int iconPx = 15;       // 普通按钮的图标框边长（px）；重着色生成位图时统一用 iconPx+4，播放键图标框也 +4
        bool underCover = false;   // 歌名/歌手放到封面下方，右侧留给歌词
        bool lyricOverInfo = false; // 太矮放不下「歌名 + 歌词」时，歌名区直接换成当前歌词
        /** 紧凑版式下歌词区固定显示几行（整行数，避免半行被裁）；1..4 */
        int lyricRows = 2;
        bool showArtist = true;    // 是否显示歌手/专辑（宽度不足时省掉）
        bool showProgress = true;  // 是否显示进度条整行；极矮时也要保留
        bool showTimes = true;     // 是否显示两端时间（纯宽度问题，不影响高度）
        bool showPrevNext = true;  // 是否显示上一首/下一首
        bool showRepeatVolume = true; // 是否显示循环与音量（最次要，先砍）
    };

    /**
     * 按当前控件尺寸算出档位参数。
     * @return 完整的一档 Metrics；只读 width()/height() 与 m_headerHidden，
     *         不改任何界面状态，因此可以每帧调用（metricsChanged 就是这么用的）
     */
    Metrics metrics() const;
    /** 一次性搭出所有子控件、样式与信号连接；只在构造函数里调用 */
    void buildUi();
    /**
     * 档位的唯一应用入口：写入 m_m → 按档位设字号/尺寸/可见性 → relayout()。
     * 幂等，会被防抖后的 resize、换主题、setHeaderHidden 反复调用；
     * 内部顺序不能改动（relayout 依赖已经更新过的 m_m）。
     */
    void applyMetrics();
    /**
     * 当前尺寸算出来的档位是否与已应用的（m_m）不同。
     * @return true = 需要重排；只比较「会影响布局」的字段子集，成本低
     */
    bool metricsChanged() const;
    /**
     * 把控件重新摆进网格，并统一收口所有子控件的可见性。
     * 会先清空网格再重摆，但只删 layout item 不删 widget，所以控件可以反复复用。
     */
    void relayout();
    /** 把 m_titleText / m_artistText / m_albumText 刷到标签上（歌手与专辑用「 · 」拼接） */
    void updateTexts();
    /** 重新把 effectivePlaceholder() 的结果喂给歌词视图 */
    void updateLyricHint();
    QString effectivePlaceholder() const; // 矮版式下没歌词时退回「歌名 · 歌手」

    // ---- 系统媒体会话 ----
    /** 创建 GSMTC 监听器、接好信号并启动；没有 GSMTC 的构建直接把界面设成「不可用」 */
    void initSystem();
    /**
     * 系统媒体会话的当前状态回调（一首歌的任何字段变化都会走到这里）。
     * @param appId    会话所属播放器标识（exe 名或 AUMID），可能为空
     * @param title    歌名；与 artist 同时为空视为「未在播放」
     * @param artist   歌手
     * @param album    专辑，可能为空
     * @param playing  是否正在播放
     * @param position 当前进度（毫秒）
     * @param duration 总时长（毫秒），0 表示未知/直播流
     * @param seekable 播放器是否支持 seek；不支持时进度条仍可拖动但不会真的跳转
     * @param cover    封面图的原始编码字节（PNG/JPEG），空表示本会话没有封面
     */
    void onMediaChanged(const QString &appId, const QString &title, const QString &artist,
                        const QString &album, bool playing, qint64 position, qint64 duration,
                        bool seekable, const QByteArray &cover);
    /**
     * 切换「优先显示哪个播放器」。
     * @param appId 目标会话标识；传空表示跟随系统当前会话（默认）
     * 副作用：写入全局设置并立即落盘、重新向监听器订阅、刷新音量。
     */
    void chooseSource(const QString &appId);

    // ---- 歌词 ----
    /** 清掉旧歌词并按当前歌名/歌手发起一次网络歌词请求（未在播放时只更新提示语） */
    void requestLyrics();
    /**
     * 歌词抓取完成回调。
     * @param title  请求时的歌名，用于丢弃「迟到的上一首」结果
     * @param artist 请求时的歌手（当前未使用）
     * @param text   歌词原文（LRC 或纯文本）；空表示查不到
     */
    void onLyricReady(const QString &title, const QString &artist, const QString &text);

    // ---- 显示 ----
    /**
     * 更新歌名/歌手/专辑；歌名+歌手变化时会清空并重新拉歌词。
     * @param title  歌名
     * @param artist 歌手
     * @param album  专辑，空则界面上只显示歌手
     */
    void setNowPlaying(const QString &title, const QString &artist, const QString &album);
    /** 设置封面（空图 = 让 CoverView 画音符占位），同时缓存原始图供换尺寸重采样 */
    void setCover(const QPixmap &pixmap);
    /**
     * 更新进度显示。
     * @param position 当前进度（毫秒）
     * @param duration 总时长（毫秒），<=0 时进度条禁用
     * 正在拖动滑条时（m_seeking）不改滑条本身，避免和用户的手抢。
     */
    void setProgress(qint64 position, qint64 duration);
    /** 播放状态变化：联动封面旋转与播放键图标（旋转本身不受 m_playing 去重影响） */
    void setPlaying(bool playing);
    /** 立即刷新两端时间文本（本地补时后调用，保证与滑条同步） */
    void updateTimeLabels();
    /** 300ms 心跳：用本地时钟补进度、推送歌词位置、执行循环策略 */
    void onTick();

    // ---- 循环 / 音量 ----
    /** 顺序 → 列表 → 单曲 循环切换，并清掉「本轮已处理」标志 */
    void cycleRepeat();
    /** 按 m_repeat 刷新循环按钮的图标与提示文案 */
    void updateRepeatButton();
    /** 到达曲末时按循环模式替播放器补一个动作（单曲回头 / 列表拉下一首） */
    void applyRepeatPolicy();
    /** 从播放器会话（找不到则系统）读当前音量回填滑条与提示 */
    void syncVolume();
    /**
     * 写音量。
     * @param value    目标音量，内部夹到 [0, 100]；<0 由调用方负责（本函数不接受未知值）
     * @param fromUser true = 用户操作，需要 emit panelChanged 触发持久化；程序回填时传 false 免得回环
     */
    void applyVolume(int value, bool fromUser);
    /** 按当前音量与「是否退化成系统音量」刷新音量图标和提示 */
    void updateVolumeButton();
    /** 按 m_playing 重画播放/上一首/下一首图标（统用主题色重着色） */
    void updateTransportIcons();

    LyricFetcher *m_lyricFetcher = nullptr;              // 歌词网络抓取；随本控件析构
    GlobalMediaSessionListener *m_listener = nullptr;    // GSMTC 监听；无 GSMTC 构建时恒为 nullptr

    QString m_appId;        // 空 = 跟随系统当前会话
    QString m_appName;      // m_appId 的展示名，显示在角标上
    QString m_currentAppId; // 当前实际在放歌的播放器
    QStringList m_sessionIds;   // 最近一次扫描到的会话标识，与 m_sessionNames 同下标
    QStringList m_sessionNames; // 上面对应的展示名（friendlyApp 优先，其次播放器自报名）

    QString m_titleText;   // 歌名；「未在播放」也是走这里
    QString m_artistText;  // 歌手
    QString m_albumText;   // 专辑，空则歌手行只显示歌手
    QString m_trackKey;    // title|artist 拼串，用来判断是否换歌（避免重复拉歌词/清封面）
    QPixmap m_coverPix;    // 原始封面缓存；当前只写不读（重裁切由 CoverView 自己的 m_src 负责）
    QByteArray m_coverData; // 上一次真正应用过的封面原始字节；用于跳过「同一张图重复送」的解码与重裁

    bool m_playing = false;      // 是否正在播放（来自会话回调）
    bool m_seekable = true;      // 播放器是否支持 seek；false 时拖动只改本地显示
    bool m_seeking = false;      // 用户正按着进度条；期间忽略外部进度覆盖
    bool m_systemReady = false;  // GSMTC 是否成功启动（决定角标显示「跟随系统」还是「不可用」）
    bool m_headerHidden = false; // 标题栏是否折叠，影响纵向边距档位
    bool m_repeatHandled = false;   // 本轮播放结束已经处理过，避免反复触发
    qint64 m_pos = 0;            // 上次已知/推算出的播放进度（毫秒）
    qint64 m_dur = 0;            // 总时长（毫秒），0 = 未知
    QElapsedTimer m_posClock;    // 距上次 m_posBase 落地已过多久，用于两次上报之间补时间
    qint64 m_posBase = 0;        // 最近一次权威进度（毫秒）；实际进度 = m_posBase + m_posClock
    /**
     * 两端时间文本上次真正刷新时对应的「秒数」与总时长，用来抑制高频 setText。
     * 心跳是 10Hz，但显示的只是 mm:ss，秒数没变就不该重设文本（每秒 10 次 setText 会带着
     * 进度行一起重排，白抖）。-1 表示还没刷过，保证首次一定刷新。
     */
    qint64 m_shownSec = -1;
    qint64 m_shownDur = -1;

    int m_repeat = RepeatOff;    // 循环模式，取值见 RepeatMode
    int m_volume = -1;              // -1 表示还没取到
    bool m_volumeIsSystem = false;  // 找不到播放器自己的会话时，退化成调系统音量

    QVector<QPair<qint64, QString>> m_lyricLines; // 解析后的 LRC（毫秒 + 该句文本，按时间升序）
    QString m_lyricPlain;                         // 无时间轴时退回的纯文本歌词

    Metrics m_m;   // 已应用的档位；metricsChanged() 拿它和新算的档位比

    QGridLayout *m_grid = nullptr;   // 唯一布局容器，relayout 时整表清空重摆
    QWidget *m_progressRow = nullptr; // 时间 + 滑条那一行
    QWidget *m_info = nullptr;        // 歌名 + 歌手两行的容器
    QWidget *m_controls = nullptr;    // 控制按钮行
    CoverView *m_cover = nullptr;     // 圆形旋转封面
    MarqueeLabel *m_title = nullptr;  // 歌名（超长滚动）
    MarqueeLabel *m_artist = nullptr; // 歌手 · 专辑（超长滚动）
    LyricsView *m_lyrics = nullptr;   // 歌词；构造期就建好，relayout 只负责摆位置
    QLabel *m_elapsed = nullptr;      // 已播时间，宽度按「88:88」等宽固定
    QLabel *m_total = nullptr;        // 总时长，同上
    QSlider *m_progress = nullptr;    // 进度滑条，范围 [0, 总时长毫秒]
    QToolButton *m_repeatBtn = nullptr; // 循环模式按钮
    QToolButton *m_playBtn = nullptr;   // 播放/暂停（图标随 m_playing 变）
    QToolButton *m_prevBtn = nullptr;   // 上一首
    QToolButton *m_nextBtn = nullptr;   // 下一首
    QToolButton *m_volumeBtn = nullptr; // 音量；点击弹面板，滚轮微调
    QString m_lyricHint;              // 歌词区无内容时的提示语（加载中/暂无歌词/未在播放）
    QMenu *m_volumeMenu = nullptr;    // 音量弹出面板（内含滑条 + 百分比）
    QSlider *m_volumeSlider = nullptr; // 音量滑条，范围 [0, 100]
    QLabel *m_volumeValue = nullptr;  // 百分比文本，未知时显示「—」
    QTimer *m_tick = nullptr;         // 300ms 心跳
    QTimer *m_applyTimer = nullptr;   // 合并连续 resize，一帧只重排一次
};
