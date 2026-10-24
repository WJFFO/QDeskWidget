/**
 * @file App.cpp
 * App 的实现：托盘装配、窗口生命周期（建 / 重建 / 显隐）、组合关系维护、设置对话框与落盘。
 *
 * 关键约定 / 坑：
 *  - BoxManager 的 boxAdded / boxRemoved 是直连同步信号，所以 create() 返回时窗口一定已建好、
 *    remove() 返回时窗口已被 take + deleteLater（不要在这两个调用后立刻再用旧窗口指针）；
 *  - 组合关系改动后统一 rebuildWindows()，旧窗口指针全部作废；
 *  - 落盘分两条路：高频路径走 450ms 防抖 scheduleSave()，结构性改动（组合、退出）直接 save()。
 */
#include "App.h"

#include "core/AppSettings.h"
#include "core/BoxManager.h"
#include "core/Paths.h"
#include "core/ShellActions.h"
#include "platform/Platform.h"
#include "ui/FileBoxWindow.h"
#include "ui/NewBoxDialog.h"
#include "ui/SettingsDialog.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QSharedPointer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QApplication>
#include <QMenu>
#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QSystemTrayIcon>
#include <QTimer>

namespace
{
/**
 * 给弹出菜单套上主题样式：底色、圆角、项内边距、选中色、禁用色、分隔线都取自 AppSettings。
 * @param m 由调用方 new 出来的 QMenu（本函数不接管其生命周期）
 * 注：QSS 不继承，所以每个弹出的菜单都要单独套一次（新建整理盒对话框的菜单走 UiStyle）。
 */
void styleAppMenu(QMenu *m)
{
    const AppSettings &st = AppSettings::get();
    // 菜单字号固定 13px，与全局 UI 字体保持一致（不跟随 DPI 缩放单独配）
    m->setFont(st.uiFont(13));
    m->setStyleSheet(QStringLiteral(
                         "QMenu{background:%1;border:1px solid %2;border-radius:9px;padding:6px;}"
                         "QMenu::item{padding:6px 22px 6px 10px;border-radius:6px;color:%3;}"
                         "QMenu::item:selected{background:%4;}"
                         "QMenu::item:disabled{color:%5;}"
                         "QMenu::separator{height:1px;background:%2;margin:5px 8px;}")
                         .arg(st.menuBg().name(QColor::HexArgb),   // %1 底色带 alpha，才能做出玻璃感
                              st.borderColor().name(QColor::HexArgb), // %2 描边 & 分隔线
                              st.textColor().name(),               // %3 常态文字
                              st.accent().name(),                  // %4 悬停/选中底色
                              st.subTextColor().name()));          // %5 禁用项文字
}

/**
 * 程序图标：统一使用 resources/logo.png（经 qt_add_resources 编译进 qrc，前缀 /app）。
 * 托盘、窗口、任务栏、弹窗全部取这一个来源，不再各画各的。
 * @return 图标；QIcon 会按目标尺寸自动缩放 256×256 原图（托盘 16px / 任务栏 32px 都够用）。
 *         资源缺失（理论上只在手工改动 CMake 资源段后出现）时回退到旧的程序化绘制。
 */
QIcon makeAppIcon()
{
    const QString iconPath = QStringLiteral(":/app/logo.png");
    if (QFile::exists(iconPath))
        return QIcon(iconPath);

    // ---- 兜底：资源缺失时的程序化占位图标（正常构建不会走到） ----
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // 竖向渐变：上浅下深，模拟玻璃受光
    QLinearGradient g(0, 0, 0, 64);
    g.setColorAt(0.0, QColor(0x62, 0xCB, 0xFF));
    g.setColorAt(1.0, QColor(0x2C, 0x6F, 0xD6));
    QPainterPath base;
    base.addRoundedRect(QRectF(6, 8, 52, 48), 13, 13);
    p.fillPath(base, g);

    // 文件夹图形
    QPainterPath folder;
    folder.addRoundedRect(QRectF(15, 24, 34, 22), 5, 5);
    p.fillPath(folder, QColor(255, 255, 255, 240));
    QPainterPath tab;
    // 标签页比主体更窄、更靠上，透明度略低一档以区分前后层次
    tab.addRoundedRect(QRectF(15, 19, 16, 9), 4, 4);
    p.fillPath(tab, QColor(255, 255, 255, 190));

    p.end();
    return QIcon(pm);
}
} // namespace

// 构造：只搭数据侧与信号连接，不碰磁盘、不建托盘（都推迟到 start()，便于单实例提前退出）
App::App(QObject *parent)
    : QObject(parent)
{
    m_boxes = new BoxManager(this);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    // 450ms：把拖动、连续改设置产生的一串状态回写合并成一次全量写盘。
    // 单次定时器每次 start() 都会重新计时，防抖就是靠这个「重启」实现的
    m_saveTimer->setInterval(450);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { m_boxes->save(); });

    // boxAdded 是直连信号：create/createWidget 返回前窗口就已经建好并 show 了
    connect(m_boxes, &BoxManager::boxAdded, this, [this](const BoxConfig &cfg)
    {
        createWindow(cfg);
    });
    // 先 take 再销毁：take 会从哈希里摘掉，避免 deleteLater 之前还有代码按 id 拿到半死的指针
    connect(m_boxes, &BoxManager::boxRemoved, this, [this](const QString &id)
    {
        if (FileBoxWindow *w = m_windows.take(id)) {
            w->hide();
            w->deleteLater();
        }
    });
}

// 启动顺序有依赖：抢单实例 → 读配置 → 建窗口 → 建托盘。
// 托盘放最后，是因为它引用了 AppSettings 与当前盒子集合，此时数据才算齐。
bool App::start()
{
    // ---- 单实例：已有进程时通知它显示窗口，然后本进程退出 ----
    const QString key = QStringLiteral("QDeskWidget-single-instance");
    {
        QLocalSocket probe;
        probe.connectToServer(key);
        // 180ms 只等本地命名管道的握手；真有一实例在跑会立刻连上，等太久只会拖慢冷启动
        if (probe.waitForConnected(180)) {
            probe.write("show");
            probe.flush();
            probe.waitForBytesWritten(180);
            return false;
        }
    }
    // 上一个进程若被强杀，管道文件会残留、listen 会失败，所以先无条件清一次；
    // 上面已确认没有活着的实例，此处删除不会误伤
    QLocalServer::removeServer(key);
    m_server = new QLocalServer(this);
    if (m_server->listen(key)) {
        connect(m_server, &QLocalServer::newConnection, this, [this]
        {
            while (QLocalSocket *sock = m_server->nextPendingConnection()) {
                sock->deleteLater();
                // 不校验对端发来的内容（我们只发 "show"），收到任何连接都把窗口叫出来
                showAllWindows();
            }
        });
    }

    m_boxes->load();

    // 不自动创建空盒子：没有任何盒子时只留托盘，用户可以从托盘菜单新建。
    // （以前这里会建一个空的「桌面整理」，对已经用过一阵的人来说反而是噪音）
    rebuildWindows();

    buildTray();

    // 兜底落盘：正常路径的 450ms 防抖可能还没到点就被退出打断
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { m_boxes->save(); });
    return true;
}

// 建单个宿主窗口：先收集成员配置，再建窗口并接信号。
// 注意 cfg 是「宿主」的配置，成员配置要从 m_boxes 里现取，不能沿用调用方的副本。
void App::createWindow(const BoxConfig &cfg)
{
    // 已有同 id 窗口：说明只是配置变了（如外部整体刷新），不重建窗口，避免丢焦点 / 丢动画状态
    if (m_windows.contains(cfg.id)) {
        m_windows.value(cfg.id)->setConfig(cfg);
        return;
    }

    // 0 号固定是宿主自己，后面按 groupMembers 顺序追加；成员配置此时可能已被删，
    // 找不到就跳过（rebuildWindows 已先做过一次清理，这里是二次兜底）
    QVector<BoxConfig> members{ cfg };
    for (const QString &id : cfg.groupMembers) {
        if (const BoxConfig *m = m_boxes->find(id))
            members << *m;
    }

    auto *w = new FileBoxWindow(cfg);
    m_windows.insert(cfg.id, w);

    // 回调而非快照：每次打开菜单现取，才能反映期间新建 / 删除的盒子
    w->setBoxListProvider([this] { return allBoxes(); });
    if (members.size() > 1)
        w->setGroupMembers(members, cfg.currentMember);

    // 窗口把「当前显示成员」的即时状态回写进存储。id 是成员 id，可能不是宿主 id，
    // 所以这里必须按 id 查而不是直接覆盖 cfg
    connect(w, &FileBoxWindow::stateChanged, this, [this, w](const QString &id)
    {
        if (BoxConfig *stored = m_boxes->find(id))
            *stored = w->config();
        scheduleSave();
    });
    connect(w, &FileBoxWindow::removeRequested, this, [this](const QString &id)
    {
        // 删掉宿主窗口时，把成员也一并移除，避免变成孤儿
        // （先取副本再逐个删：remove 会改动 m_boxes 内部列表，不能一边遍历一边删）
        if (const BoxConfig *host = m_boxes->find(id)) {
            const QStringList members = host->groupMembers;
            for (const QString &m : members)
                m_boxes->remove(m);
        }
        m_boxes->remove(id); // boxRemoved 会销毁窗口
        scheduleSave();
    });
    connect(w, &FileBoxWindow::newBoxRequested, this, &App::newBox);
    connect(w, &FileBoxWindow::settingsRequested, this, &App::openSettings);
    connect(w, &FileBoxWindow::currentMemberChanged, this,
            [this](const QString &hostId, const QString &memberId)
    {
        // 只记「上次显示的是谁」，不重建窗口：切成员只是同一窗口换内容
        if (BoxConfig *host = m_boxes->find(hostId)) {
            host->currentMember = memberId;
            scheduleSave();
        }
    });
    connect(w, &FileBoxWindow::groupChangeRequested, this,
            // 信号只带 memberId，组长身份在建立连接时就绑死成 cfg.id（窗口的 hostId 永不改变）
            [this, hostId = cfg.id](const QString &memberId, bool add)
    {
        groupChange(hostId, memberId, add);
    });

    w->show();
}

// 返回副本而不是引用：窗口拿着它在菜单弹出期间做判断，这段时间里盒子集合可能被改
QVector<BoxConfig> App::allBoxes() const
{
    return m_boxes->boxes();
}

// 全量重建：先销毁所有窗口，再清脏数据，最后只给「非成员」建窗口。
// 三步顺序不能换——清理必须在建窗口之前，否则新窗口会拿到过期成员表。
void App::rebuildWindows()
{
    // 先备份指针并清空哈希：deleteLater 是延迟销毁，期间若有人按 id 查会拿到正在死的窗口
    const QList<FileBoxWindow *> olds = m_windows.values();
    m_windows.clear();
    for (FileBoxWindow *w : olds) {
        w->hide();
        w->deleteLater();
    }

    // 清掉指向已删除盒子的组合成员，避免出现「幽灵成员」
    for (const BoxConfig &c : m_boxes->boxes()) {
        if (c.groupMembers.isEmpty())
            continue;
        if (BoxConfig *host = m_boxes->find(c.id)) {
            QStringList kept;
            for (const QString &id : host->groupMembers) {
                if (m_boxes->find(id))
                    kept << id;
            }
            host->groupMembers = kept;
            // 当前显示成员被删时回到「显示自己」，否则窗口会尝试切到一个不存在的成员
            if (!host->currentMember.isEmpty() && !m_boxes->find(host->currentMember))
                host->currentMember.clear();
        }
    }

    // 收集「所有作为成员出现过的 id」；注意这是全表扫描，成员的成员不会再往下追——
    // 组合只有一层，成员自己的 groupMembers 在 groupChange 里已被清空
    QSet<QString> members;
    for (const BoxConfig &c : m_boxes->boxes())
        for (const QString &id : c.groupMembers)
            members.insert(id);

    for (const BoxConfig &c : m_boxes->boxes()) {
        if (members.contains(c.id))
            continue; // 组合成员不单独建窗口
        createWindow(c);
    }
}

// 组合关系变更：先维护数据一致性（唯一归属 + 单一层级），再全量重建窗口。
// 这里不尝试就地修改窗口，是因为成员增删会改变窗口承载的内容集合，重建最简单也最不易漏。
void App::groupChange(const QString &hostId, const QString &memberId, bool add)
{
    BoxConfig *host = m_boxes->find(hostId);
    // 自己不能当自己的成员；组长不存在时直接放弃（可能刚被删）
    if (!host || hostId == memberId)
        return;

    if (add) {
        // 一个盒子只能属于一个组合：先把它从别的组合里摘出来
        for (const BoxConfig &c : m_boxes->boxes()) {
            if (c.id == hostId)
                continue;
            if (c.groupMembers.contains(memberId)) {
                if (BoxConfig *other = m_boxes->find(c.id)) {
                    other->groupMembers.removeAll(memberId);
                    if (other->currentMember == memberId)
                        other->currentMember.clear();
                }
            }
        }
        // 被拉进组合的盒子，自己不再当组长
        // （组合只支持一层：否则窗口归属与成员遍历都会出现歧义）
        if (BoxConfig *m = m_boxes->find(memberId)) {
            m->groupMembers.clear();
            m->currentMember.clear();
        }
        // 重复加入时保持幂等，不留重复 id
        if (!host->groupMembers.contains(memberId))
            host->groupMembers << memberId;
    } else {
        host->groupMembers.removeAll(memberId);
        // 移出的正好是当前显示成员时，把显示位置退回宿主自己
        if (host->currentMember == memberId)
            host->currentMember.clear();
    }

    // 结构性改动不进防抖队列：立刻落盘，避免崩溃时组合关系与 boxes.json 不一致
    m_boxes->save();
    rebuildWindows();
}

// 自检用：清空所有盒子。拷贝一份再遍历，因为 remove 会改动 m_boxes 内部列表。
void App::removeAllBoxes()
{
    const QVector<BoxConfig> all = m_boxes->boxes();
    for (const BoxConfig &c : all) {
        m_boxes->remove(c.id);
        // 立刻派发 DeferredDelete，模拟事件循环里窗口真的被销毁
        // （这是本函数存在的意义：批量删除时的堆破坏只有在真的销毁才暴露）
        QCoreApplication::processEvents();
    }
}

// 自检用：模拟用户点「删除…」并点确认。取 constBegin() 的窗口，不保证是「最后建的」那个，
// 只保证每次都是同一个（哈希顺序稳定）——自检只关心路径能否走通。
void App::deleteLastBoxViaUi()
{
    if (m_windows.isEmpty())
        return;

    FileBoxWindow *w = m_windows.constBegin().value();
    // 外层延时：给窗口留出首帧布局时间，避免在窗口尚未稳定时就去走删除流程
    QTimer::singleShot(300, w, [w]
    {
        // 对话框是模态的，deleteBox() 会阻塞；用定时器在模态循环里点「删除整理盒」
        // 内层 600ms 是留给模态框弹出的时间，必须比外层的 300ms 更晚
        QTimer::singleShot(600, w, []
        {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            // 按文本匹配而不是按角色：确认按钮的角色在不同 Qt 版本 / 语言包里不稳定
            for (QAbstractButton *b : box->buttons()) {
                if (b->text().contains(QStringLiteral("删除整理盒"))) {
                    b->click();
                    return;
                }
            }
        });
        w->deleteBoxForTest();
    });
}

// 自检用：把每个窗口当前画面存成 PNG，组合窗口额外遍历各成员。
// 截图前不主动 processEvents / 等待，依赖调用方在窗口稳定后再调（main 里是 5.2s 后）。
void App::saveSnapshots(const QString &dir)
{
    QDir().mkpath(dir);
    for (FileBoxWindow *w : m_windows) {
        const QString base = dir + QStringLiteral("/box_") + w->boxId();
        w->grab().save(base + QStringLiteral(".png"));

        // 组合窗口：逐个切换成员各存一张，顺带验证 Ctrl+Tab 的切换逻辑
        // 先存再切，所以第 1 张就是当前成员；循环结束时窗口回到起点（n 次 +1 步绕一圈）
        const int n = w->memberCount();
        if (n > 1) {
            for (int i = 0; i < n; ++i) {
                w->grab().save(QStringLiteral("%1_member%2.png").arg(base).arg(i + 1));
                w->switchMember(1);
            }
        }
    }
}

// 重启单次定时器即完成防抖：450ms 内的连续调用只会落盘一次
void App::scheduleSave()
{
    m_saveTimer->start();
}

// 托盘菜单入口：模态对话框，确认后交给 BoxManager。
// 不在这里创建窗口——create() 发 boxAdded，App 构造函数里接的槽会建窗口
void App::newBox()
{
    NewBoxDialog dlg;
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_boxes->create(dlg.boxTitle(), dlg.folder(), dlg.isManaged());
}

// 新建组件统一入口：设置面板的「添加」、自检参数都走这里，保证行为一致
void App::createWidgetBox(int kind, const QString &pluginId, const QString &title)
{
    // 新建的组件一律按「最小尺寸」落地。各组件的最小尺寸由面板的
    // minimumSizeHint() 决定（音乐 168×76 内容区、天气 …），这个值只有
    // 窗口自己算得出来，所以先建窗口，再让它收缩到下限。
    // 依赖 boxAdded 是直连同步信号：createWidget 返回时窗口已在 m_windows 中，
    // 否则这里 value() 会拿到 nullptr，组件就会以默认的大尺寸出生
    const BoxConfig c = m_boxes->createWidget(kind, pluginId, title);
    if (FileBoxWindow *w = m_windows.value(c.id, nullptr))
        w->shrinkToMinimum();
    scheduleSave();
}

// 自检用：切一次主题并刷新所有窗口。和设置面板里改主题走的是同一条刷新路径
// （SettingsDialog 发 appearanceChanged → applyAppearanceToAll），所以能真实复现「换主题后面板没重上色」。
void App::toggleThemeForTest()
{
    AppSettings &st = AppSettings::get();
    st.theme = st.isDark() ? AppSettings::Light : AppSettings::Dark;
    st.save();
    applyAppearanceToAll();
    qInfo("theme -> %s", st.isDark() ? "Dark" : "Light");
}

// 自检用：定时切换材质并记录 DWM 背板类型，验证「设置里的材质」与「实际生效的背板」一致。
// 全流程靠 QTimer 串起来，不阻塞事件循环（材质切换后需要若干帧才稳定，必须让事件循环跑起来）。
void App::runMaterialSequence(const QString &dir, const QList<int> &materials)
{
    QDir().mkpath(dir);
    // 用 QSharedPointer 管理日志文件：两个定时器 lambda 都要写它，谁先谁后不确定，
    // 靠引用计数保证最后一个使用者退出时才关闭。打开失败就整个序列放弃
    auto log = QSharedPointer<QFile>::create(QDir(dir).filePath(QStringLiteral("material_check.txt")));
    if (!log->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    log->write("材质切换一致性检查（backdrop 1=NONE 2=MAINWINDOW/Mica 3=Acrylic）\n");
    log->flush();

    int delay = 700; // 首步前留 700ms，等窗口出完首帧再开始切材质
    for (int i = 0; i < materials.size(); ++i) {
        const int mat = materials.at(i);
        // 写设置 + 推到所有窗口。这里绕过 SettingsDialog 直接改 AppSettings，
        // 因为自检不该依赖 UI 是否存在
        QTimer::singleShot(delay, this, [this, mat]
        {
            AppSettings::get().material = mat;
            applyAppearanceToAll();
        });
        // 650ms 后再采样：DWM 换背板是异步的，立刻读 backdropType 会读到旧值
        QTimer::singleShot(delay + 650, this, [this, dir, i, mat, log]
        {
            QStringList lines;
            for (FileBoxWindow *w : m_windows) {
                lines << QStringLiteral("step %1 material=%2 box=%3 backdrop=%4\n")
                             .arg(i)
                             .arg(mat)
                             .arg(w->boxId())
                             .arg(Platform::backdropType(w));
                w->grab().save(QStringLiteral("%1/m%2_%3_%4.png")
                                   .arg(dir)
                                   .arg(i)
                                   .arg(mat)
                                   .arg(w->boxId()));
            }
            log->write(lines.join(QString()).toUtf8());
            log->flush(); // 每步都 flush：自检进程后面是 _Exit/quit，缓冲可能来不及落盘
        });
        // 每步占 1500ms：切换 + 等待 + 截图 + 余量
        delay += 1500;
    }
    QTimer::singleShot(delay + 400, this, [] { QCoreApplication::quit(); });
}

// 设置窗口是懒创建的单例（同一时刻最多一个）。用非模态，改一项就能看到主界面实时变化。
void App::openSettings()
{
    if (!m_settings) {
        m_settings = new SettingsDialog(nullptr); // parent 故意为空：避免被主窗口层级影响
        // 不再强制置顶：让设置框表现为普通窗口，可被其它窗口盖住（用户要求）。
        // 备注：整理盒默认是「置底」Tool 窗口（alwaysOnTop=false → WindowStaysOnBottomHint），
        // 正常情况下不会盖住设置框；若用户手动把某个盒子设为置顶，才可能出现遮挡。
        // 两个信号都接到同一个槽：目前区分「外观 / 行为」只是语义，处理方式一致
        connect(m_settings, &SettingsDialog::appearanceChanged, this, &App::applyAppearanceToAll);
        connect(m_settings, &SettingsDialog::behaviorChanged, this, &App::applyAppearanceToAll);
        // 组件 / 插件的添加入口只在设置面板里：这里接住「添加」请求，走和自检参数同一条创建路径
        connect(m_settings, &SettingsDialog::widgetAddRequested, this,
                [this](int kind, const QString &pluginId, const QString &title)
        {
            createWidgetBox(kind, pluginId, title);
        });
        // 关闭即销毁并置空，下次重新构造才能重新 loadValues()（防止外部改了设置后 UI 仍显示旧值）
        connect(m_settings, &QDialog::finished, this, [this]
        {
            m_settings->deleteLater();
            m_settings = nullptr;
        });
    }
    // 已存在时只是前置：show + raise + activate 三步缺一不可（最小化的窗口靠 activateWindow 恢复）
    m_settings->show();
    m_settings->raise();
    m_settings->activateWindow();
}

// 外观总开关：换肤 / 改材质 / 改图标尺寸后由这里统一推给所有窗口。
// 只遍历 m_windows（宿主），组合成员由各自宿主窗口内部一起刷新
void App::applyAppearanceToAll()
{
    for (FileBoxWindow *w : m_windows)
        w->applyAppearance();
}

void App::showAllWindows()
{
    for (FileBoxWindow *w : m_windows)
        w->show();
}

void App::hideAllWindows()
{
    for (FileBoxWindow *w : m_windows)
        w->hide();
}

// 以「有没有任何一个窗口可见」作为整体状态，避免出现「一半显示一半隐藏」时反复横跳
void App::toggleAllWindows()
{
    bool anyVisible = false;
    for (FileBoxWindow *w : m_windows)
        if (w->isVisible()) {
            anyVisible = true;
            break;
        }
    if (anyVisible)
        hideAllWindows();
    else
        showAllWindows();
}

// 托盘是这个应用唯一的常驻入口：所有窗口关掉后只能靠它把界面叫回来。
// 菜单结构：新建 → 显示/隐藏 → 设置/数据目录 → 退出，与窗口右键菜单保持同一套术语。
void App::buildTray()
{
    m_tray = new QSystemTrayIcon(makeAppIcon(), this);
    // tooltip 兼作「程序还在跑」的提示，用户关完窗口后靠它确认进程没退
    m_tray->setToolTip(QStringLiteral("QDeskWidget · 桌面整理盒"));

    // 菜单不设 parent：交给 m_tray->setContextMenu 接管所有权，设了 parent 反而可能被提前析构
    auto *menu = new QMenu();
    menu->setFont(AppSettings::get().uiFont(13));

    menu->addAction(QStringLiteral("新建整理盒…"), this, &App::newBox);
    // 组件 / 插件的添加入口统一收在设置面板里（组件多了右键菜单会放不下），
    // 托盘这里只留「新建整理盒」。
    menu->addSeparator();
    menu->addAction(QStringLiteral("显示全部整理盒"), this, &App::showAllWindows);
    menu->addAction(QStringLiteral("隐藏全部整理盒"), this, &App::hideAllWindows);
    menu->addSeparator();
    menu->addAction(QStringLiteral("设置…"), this, &App::openSettings);
    menu->addAction(QStringLiteral("打开数据目录"), this, []
    {
        Paths::ensureDir(Paths::dataDir());
        ShellActions::openFolder(Paths::dataDir());
    });
    menu->addSeparator();
    // 退出走 qApp 的 quit：会触发 aboutToQuit → 最后一次 save()
    menu->addAction(QStringLiteral("退出"), qApp, &QApplication::quit);

    m_tray->setContextMenu(menu);
    // 单击切换显隐、双击开设置；其余原因（中键、右键）不处理——右键由 contextMenu 自动接管
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r)
    {
        if (r == QSystemTrayIcon::Trigger)
            toggleAllWindows();
        else if (r == QSystemTrayIcon::DoubleClick)
            openSettings();
    });
    m_tray->show();
}
