/**
 * @file main.cpp
 * 进程入口：COM/STA 初始化 → QApplication → 解析开发自检参数 → 启控制器 → 进事件循环。
 *
 * 关键约定 / 坑：
 *  - 自检参数全部是「一次性诊断开关」，正常用户不会传；其中 --vol-probe / --media-probe
 *    会在创建 App 之前就 return，等于跑一个独立的小工具而不是启动桌面盒子；
 *  - 两个自检出口故意用 std::_Exit(0)：GSMTC 的媒体会话线程会把正常退出拖住几十秒；
 *  - QApplication::setQuitOnLastWindowClosed(false) 是常驻托盘的前提，别改回默认值。
 */
#include "App.h"
#include "core/AppSettings.h"
#include "core/Paths.h"
#include "platform/MediaVolume.h"
#include "widgets/WidgetRegistry.h"

#ifdef QDW_HAS_GSMTC
#  include "core/globalmediasessionlistener.h"
#endif

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

#ifdef _DEBUG
#  include <crtdbg.h>
#endif

#ifdef QDW_HAS_GSMTC
// C++/WinRT：系统全局媒体会话要求在 STA 线程上初始化 COM
#  include <winrt/base.h>
#endif

int main(int argc, char *argv[])
{
#ifdef QDW_HAS_GSMTC
    // 必须在 QApplication 之前（或至少在任何 WinRT 调用之前）把当前线程设为 STA，
    // 且整个进程生命周期只能初始化一次；RPC_E_CHANGED_MODE 说明已被别人初始化过
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (...) {
        // 已经初始化过（Qt 可能先做了）就忽略
    }
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QDeskWidget"));
    QApplication::setApplicationVersion(QStringLiteral("0.3.0"));
    QApplication::setQuitOnLastWindowClosed(false); // 常驻托盘
    // 全局窗口图标：设置对话框、新建盒对话框等所有顶层窗口/弹窗都继承这一个来源
    // （与 App::buildTray 的托盘图标共用 resources/logo.png，经 qrc 编译进二进制）
    app.setWindowIcon(QIcon(QStringLiteral(":/app/logo.png")));

    // 数据目录必须先建出来：后续 BoxManager::load、插件扫描、日志都可能往里写
    Paths::ensureDir(Paths::dataDir());
    // 全局默认字体取 13px；各控件仍会按用途再调 uiFont(n)，这里只是兜底
    QApplication::setFont(AppSettings::get().uiFont(13));

    // 开发自检：
    //   --grab <目录>                渲染所有盒子后退出
    //   --material-seq 2,1,0,1 --out <目录>
    //                                依次切换材质（0 亚克力 / 1 云母 / 2 纯色）并截图，
    //                                同时记录每步的 DWM 背板类型，用于验证切换一致性
    //   --settings                   打开设置对话框（自检用，配 --quit-after 截图）
    //   --new-widget <kind> [--new-widget-plugin <id>]
    //                                走「新建组件」路径建一个组件盒子（验证最小尺寸落地）
    //   --theme-flip <毫秒>          到点切换一次主题并刷新所有窗口，用于验证组件面板的重新上色
    //
    // 参数解析约定：
    //   --grab <目录>            输出 PNG 的目录，不存在会自动创建；5.2s 后截图并 _Exit(0)。
    //                            也支持 --grab=<目录> 的等号写法（唯一支持等号的参数）
    //   --material-seq <序列>    逗号分隔的材质序列，元素 0 亚克力 / 1 云母 / 2 纯色；
    //                            空段被跳过，非数字段会被 toInt() 解析成 0（即亚克力）
    //   --out <目录>             材质序列的输出目录，缺省 build/matseq
    //   --vol-probe <appId>      验证「按播放器调音量」的 WASAPI 通路：打印该播放器当前音量、
    //                            试设 30 再读回、然后恢复原值（读不到则恢复成 100），随即退出。
    //                            不启动 App，属于纯命令行工具
    //   --media-probe            直接查询系统全局媒体会话（GSMTC），打印会话列表与当前曲目信息，
    //                            用于判断「检测不到歌曲」是应用问题还是系统压根没有会话。
    //                            不带值（写成 --media-probe=1 也认），16s 后强制 _Exit(0)
    //   --crt-debug              仅 Debug 构建：把 CRT 堆破坏报告改为输出到调试器并中断，
    //                            而不是弹模态框（批处理里弹框会永久卡住）
    //   --quit-after <毫秒>      到点走正常 quit()（完整析构），用于发现「退出时堆破坏」这类问题
    //   --remove-all <毫秒>      到点静默删除所有盒子（不走确认框），用于批量删除的稳定性测试
    //   --delete-ui <毫秒>       到点走「删除…」对话框并自动点确认，复现真实用户路径
    //   --new-widget <kind>      到点（固定 150ms）新建一个该 kind 的组件盒子；
    //                            kind 取 BoxConfig::Kind（天气 2 / 音乐 3 / 插件 4）
    //   --new-widget-plugin <id> 配合 --new-widget，kind == KindPlugin 时指定插件 id
    //   --theme-flip <毫秒>      到点把主题切到另一档、落盘并刷新所有窗口（配 --grab 比对用）
    QString grabDir;            // --grab 的输出目录；空表示不截图
    QString seqDir;             // --material-seq 的输出目录（--out）
    QString volProbe;           // --vol-probe 的目标播放器 appId；空表示不跑该自检
    QString mediaProbe;         // --media-probe 的开关标志（非空即启用，内容本身无意义）
    int quitAfterMs = 0;        // --quit-after，单位毫秒；0 表示不自动退出
    int removeAllMs = 0;        // --remove-all，单位毫秒；0 表示不执行
    int deleteUiMs = 0;         // --delete-ui，单位毫秒；0 表示不执行
    bool openSettings = false;
    int themeFlipMs = 0;        // --theme-flip，单位毫秒；0 表示不切
    int newWidgetKind = -1;     // --new-widget 的 kind；-1 是「不新建」的哨兵（合法 kind 均 ≥ 0）
    QString newWidgetPlugin;    // --new-widget-plugin，仅插件组件需要
    QList<int> materials;       // --material-seq 解析后的材质序列，顺序即执行顺序
    const QStringList args = QApplication::arguments();
    // 手工解析而非 QCommandLineParser：需要容忍未知参数与两种写法，且不想为自检引入依赖。
    // 所有带值参数都先检查 i+1 < size，缺值就静默跳过（不报错、不影响启动）
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (a == QLatin1String("--grab") && i + 1 < args.size()) {
            grabDir = args.at(++i);
        } else if (a.startsWith(QLatin1String("--grab="))) {
            // section 取第一个 '=' 之后的全部内容，路径里再有 '=' 也不会被截断
            grabDir = a.section(QLatin1Char('='), 1);
        } else if (a == QLatin1String("--out") && i + 1 < args.size()) {
            seqDir = args.at(++i);
        } else if (a == QLatin1String("--vol-probe") && i + 1 < args.size()) {
            volProbe = args.at(++i);
        } else if (a.startsWith(QLatin1String("--media-probe"))) {
            // 只判前缀：--media-probe / --media-probe=1 都算启用，值一概忽略
            mediaProbe = QStringLiteral("1");
        } else if (a == QLatin1String("--quit-after") && i + 1 < args.size()) {
            quitAfterMs = args.at(++i).toInt();
        } else if (a == QLatin1String("--remove-all") && i + 1 < args.size()) {
            removeAllMs = args.at(++i).toInt();
        } else if (a == QLatin1String("--delete-ui") && i + 1 < args.size()) {
            deleteUiMs = args.at(++i).toInt();
        } else if (a == QLatin1String("--theme-flip") && i + 1 < args.size()) {
            themeFlipMs = args.at(++i).toInt();
        } else if (a == QLatin1String("--settings")) {
            openSettings = true;
        } else if (a == QLatin1String("--new-widget") && i + 1 < args.size()) {
            newWidgetKind = args.at(++i).toInt();
        } else if (a == QLatin1String("--new-widget-plugin") && i + 1 < args.size()) {
            newWidgetPlugin = args.at(++i);
        } else if (a == QLatin1String("--material-seq") && i + 1 < args.size()) {
            // SkipEmptyParts 容忍 "2,1,0," 这种尾逗号；trimmed 容忍 "2, 1, 0" 带空格
            for (const QString &part : args.at(++i).split(QLatin1Char(','), Qt::SkipEmptyParts))
                materials << part.trimmed().toInt();
        }
    }

    // 开发自检：验证「按播放器调音量」的 WASAPI 通路
    // 注意此分支在 App 之前：不建托盘、不读 boxes.json，纯命令行探针
    if (!volProbe.isEmpty()) {
        const int before = MediaVolume::volumeFor(volProbe);
        const int sysBefore = MediaVolume::systemVolume();
        const bool ok = MediaVolume::setVolume(volProbe, 30);
        const int after = MediaVolume::volumeFor(volProbe);
        // 恢复原值：before 为 -1 说明没踩到这个播放器的会话，就用 100 兜底，
        // 免得把用户音量留在一个奇怪的数值上
        MediaVolume::setVolume(volProbe, before >= 0 ? before : 100);
        std::printf("volume(%s): before=%d set30=%d after=%d system=%d\n",
                    qPrintable(volProbe), before, ok ? 1 : 0, after, sysBefore);
        return 0;
    }

#ifdef QDW_HAS_GSMTC
    // 开发自检：直接问一遍系统媒体会话，判断「检测不到歌曲」是应用问题还是系统没有会话
    // 三个 lambda 都只用 printf 输出（该分支不依赖日志框架），并逐条 fflush 保证管道下也能及时看到
    if (!mediaProbe.isEmpty()) {
        // listener 故意不 delete：下面靠 _Exit(0) 收尾，析构反而可能触发 COM 退订的时序问题
        auto *listener = new GlobalMediaSessionListener();
        QObject::connect(listener, &GlobalMediaSessionListener::availableSessions, listener,
            [](const QList<GlobalMediaSessionListener::SessionEntry> &sessions)
        {
            std::printf("sessions=%d\n", int(sessions.size()));
            for (const auto &s : sessions)
                std::printf("  session: %s | %s\n", qPrintable(s.appId), qPrintable(s.appName));
            std::fflush(stdout);
        });
        // currentMediaChanged 每秒可能来多次，这里的输出用于人工比对曲目信息是否与播放器一致
        QObject::connect(listener, &GlobalMediaSessionListener::currentMediaChanged, listener,
            [](const QString &appId, const QString &title, const QString &artist,
               const QString &album, bool playing, qint64 pos, qint64 dur, bool seekable,
               const QByteArray &cover)
        {
            std::printf("media: app=%s title=%s artist=%s playing=%d pos=%lld dur=%lld seek=%d cover=%d\n",
                        qPrintable(appId), qPrintable(title), qPrintable(artist),
                        playing ? 1 : 0, (long long)pos, (long long)dur,
                        seekable ? 1 : 0, int(cover.size()));
            std::fflush(stdout);
        });
        // 连不上系统会话时（Win10 / 会话 broker 超时）走到这里，reason 直接可读
        QObject::connect(listener, &GlobalMediaSessionListener::unavailable, listener,
            [](const QString &reason)
        {
            std::printf("unavailable: %s\n", qPrintable(reason));
            std::fflush(stdout);
        });
        std::printf("listener.start()=%d\n", listener->start() ? 1 : 0);
        std::fflush(stdout);
        // 16s 上限：够覆盖初始化超时 + 一次轮询，又不会让自检脚本挂死
        QTimer::singleShot(16000, &app, [listener]
        {
            std::printf("lastError=%s\n", qPrintable(listener->lastError()));
            std::fflush(stdout);
            std::_Exit(0);
        });
        return QApplication::exec();
    }
#endif

    App controller;
    if (!controller.start())
        return 0; // 已有实例在运行

#ifdef _DEBUG
    // 开发自检：把 CRT 的堆破坏报告输出到调试器并中断，而不是弹一个模态框
    // 必须在任何分配可能出问题之前设置，所以放在 App 构造之后、事件循环之前
    if (args.contains(QStringLiteral("--crt-debug"))) {
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
    }
#endif

    // 三个「到点执行」的自检开关互相独立，可以叠加（例如先 --delete-ui 再 --quit-after）
    if (deleteUiMs > 0) {
        QTimer::singleShot(deleteUiMs, &controller, [&controller] { controller.deleteLastBoxViaUi(); });
    }
    if (removeAllMs > 0) {
        QTimer::singleShot(removeAllMs, &controller, [&controller] { controller.removeAllBoxes(); });
    }
    if (newWidgetKind >= 0) {
        // 开发自检：走和托盘菜单一样的「新建组件」路径，用来验证组件以最小尺寸落地
        // 固定 150ms：等 QApplication 的首轮布局/托盘初始化跑完，否则窗口建立时还没有屏幕几何
        QTimer::singleShot(150, &controller, [&controller, newWidgetKind, newWidgetPlugin]
        {
            // 标题走注册表解析，与用户从菜单新建时完全一致（含插件显示名）
            controller.createWidgetBox(newWidgetKind, newWidgetPlugin,
                                       WidgetRegistry::displayNameFor(newWidgetKind,
                                                                      newWidgetPlugin));
        });
    }
    if (openSettings) {
        // 开发自检：设置对话框没有命令行入口，靠这个开关打开它做外观/截图核对
        QTimer::singleShot(300, &controller, [&controller] { controller.openSettingsForTest(); });
    }
    if (themeFlipMs > 0) {
        // 开发自检：到点切一次主题，验证组件面板重新上色（配 --grab 比对前后两张图）
        QTimer::singleShot(themeFlipMs, &app, [&controller] { controller.toggleThemeForTest(); });
    }
    if (quitAfterMs > 0) {
        // 开发自检：到点正常退出（走完整析构），用于发现"退出时堆破坏"这类问题
        QTimer::singleShot(quitAfterMs, &app, [] { QCoreApplication::quit(); });
    }

    // 材质序列优先于普通截图：两者都要遍历所有窗口，同时开会互相踩状态
    if (!materials.isEmpty()) {
        controller.runMaterialSequence(seqDir.isEmpty() ? QStringLiteral("build/matseq") : seqDir,
                                       materials);
    } else if (!grabDir.isEmpty()) {
        QDir().mkpath(grabDir);
        // 延时 5.2s 再截图：让组件有时间完成首次数据填充（立刻截图常拿到空面板）。
        // 用 _Exit 而不是 quit：正常退出会被媒体会话线程拖住
        QTimer::singleShot(5200, &app, [&controller, grabDir]
        {
            controller.saveSnapshots(grabDir);
            // 开发自检进程直接退出：正常退出会被媒体会话线程拖住
            std::_Exit(0);
        });
    }

    return QApplication::exec();
}
