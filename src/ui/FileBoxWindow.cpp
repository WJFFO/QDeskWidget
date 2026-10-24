/**
 * @file FileBoxWindow.cpp
 * FileBoxWindow / BoxHeaderBar / ElideLabel 的实现：标题栏排版与内联重命名、拖动-吸附两段式移动、
 * 折叠/胶囊形态的几何收敛、组合成员切换、内容区三选一以及各类右键菜单。
 *
 * 阅读要点：
 *  - 尺寸相关的落点集中在 applyGeometryForState()；resizeEvent 只做「越界兜底」与形态维持；
 *  - 拖动期间 moveEvent 刻意不落盘，仅松手（dragFinished / MouseButtonRelease）时 persist()；
 *  - 无边框窗口的缩放边靠 nativeEvent 手工命中，锁定/胶囊时要显式回 HTCLIENT。
 */
#include "ui/FileBoxWindow.h"

#include "core/AppSettings.h"
#include "core/FileItemModel.h"
#include "core/Paths.h"
#include "core/ShellActions.h"
#include "mcolumnlayout.h"
#include "platform/Platform.h"
#include "ui/FileListView.h"
#include "ui/StackView.h"
#include "ui/UiStyle.h"
#include "widgets/WidgetPanel.h"
#include "widgets/WidgetRegistry.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QShortcut>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <QWheelEvent>
#include <QWindow>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace
{
constexpr int kRadius = 12;        // 展开态圆角半径；胶囊形态改用 height()/2 做成全圆
constexpr int kResizeBorder = 8;   // 缩放边热区的逻辑宽度（px），实际按 DPI 放大，见 resizeBorderPx()

/**
 * @brief 生成菜单勾选前缀，让所有条目左侧对齐（未选中用等宽空格占位）。
 * @param on 是否选中
 * @return 选中返回 "✓ "，否则返回三个空格；用空格而非去掉前缀是为了菜单不左右跳动。
 */
QString checkPrefix(bool on)
{
    return on ? QStringLiteral("✓ ") : QStringLiteral("   ");
}

/**
 * @brief 按盒子种类现画一个矢量图标，供组件标题栏使用。
 * @param kind BoxConfig::Kind 取值（天气 / 音乐 / 其它）
 * @param pluginId 插件 id；目前不区分插件，统一用默认图形（Q_UNUSED 保留接口）
 * @return 40x40 透明底 pixmap 包成的图标；颜色取自当前主题强调色。
 *
 * 不用彩色 emoji 是因为不同字体下 emoji 的尺寸与颜色不可控，标题栏会被撑歪。
 */
QIcon iconForWidget(int kind, const QString &pluginId)
{
    Q_UNUSED(pluginId)

    // 纯矢量绘制，避免彩色 emoji 在不同字体下大小/颜色不可控
    const QColor accent = AppSettings::get().accent();
    QPixmap pm(40, 40);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QPen pen(accent, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (kind) {
    case BoxConfig::KindWeather: {
        // 太阳：描边圆 + 8 条射线（45° 一条，覆盖整圈）
        p.setPen(QPen(QColor(0xF5, 0xB3, 0x4B), 2.6, Qt::SolidLine, Qt::RoundCap));
        p.drawEllipse(QPointF(20, 20), 6.0, 6.0);
        for (int i = 0; i < 8; ++i) {
            const double a = i * M_PI / 4.0;   // 每 45° 一条，M_PI/4 ≈ 0.785 rad
            p.drawLine(QPointF(20 + std::cos(a) * 10.0, 20 + std::sin(a) * 10.0),
                       QPointF(20 + std::cos(a) * 13.5, 20 + std::sin(a) * 13.5));
        }
        break;
    }

    case BoxConfig::KindMusic:
        // 双符头音符：两条竖线 + 一条顶横梁，符头用实心椭圆
        p.drawLine(QPointF(17, 28), QPointF(17, 13));
        p.drawLine(QPointF(28, 25), QPointF(28, 10));
        p.drawLine(QPointF(17, 13), QPointF(28, 10));
        p.setBrush(accent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(14, 28), 3.2, 2.6);
        p.drawEllipse(QPointF(25, 25), 3.2, 2.6);
        break;

    default:
        // 插件/未知类型：圆角方框 + 中心圆点，作为通用「组件」占位图形
        p.drawRoundedRect(QRectF(11.5, 11.5, 17, 17), 4, 4);
        p.setBrush(accent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(20, 20), 3.4, 3.4);
        break;
    }

    p.end();
    return QIcon(pm);
}

/**
 * @brief 给弹出菜单套上应用的圆角主题样式（字体、底色、描边、选中高亮、禁用色、分隔线）。
 * @param m 目标菜单；子菜单需要单独再调一次，QSS 不会自动继承到弹出子菜单。
 */
void styleMenu(QMenu *m)
{
    const AppSettings &st = AppSettings::get();
    m->setFont(st.uiFont(13));
    m->setStyleSheet(QStringLiteral(
                         "QMenu{background:%1;border:1px solid %2;border-radius:9px;padding:6px;}"
                         "QMenu::item{padding:6px 20px 6px 8px;border-radius:6px;color:%3;}"
                         "QMenu::item:selected{background:%4;}"
                         "QMenu::item:disabled{color:%5;}"
                         "QMenu::separator{height:1px;background:%2;margin:5px 8px;}")
                         .arg(st.menuBg().name(QColor::HexArgb),
                              st.borderColor().name(QColor::HexArgb),
                              st.textColor().name(),
                              st.accent().name(),
                              st.subTextColor().name()));
}
} // namespace

// ======================================================================
// ElideLabel
// ======================================================================

// 构造：只定尺寸策略与交互标志，真正的省略要等有宽度后由 resizeEvent 触发。
ElideLabel::ElideLabel(QWidget *parent)
    : QLabel(parent)
{
    // Preferred（而不是 Ignored）：让 sizeHint 参与布局分配，否则标题只能分到弹簧的一半
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setTextInteractionFlags(Qt::NoTextInteraction);
}

// 换标题：m_text 存全量文本，tooltip 也给全量，避免省略后看不全。
void ElideLabel::setFullText(const QString &text)
{
    m_text = text;
    setToolTip(text);
    updateElide();
}

// 只重算显示文本，不改 m_text —— 所以反复 resize 不会丢掉原始标题。
void ElideLabel::updateElide()
{
    const QFontMetrics fm(font());
    QLabel::setText(fm.elidedText(m_text, m_mode, qMax(0, width())));
}

// 宽度取完整文本的宽度（QFontMetrics::horizontalAdvance 不处理换行，标题本是单行）。
QSize ElideLabel::sizeHint() const
{
    // 用完整文本作为尺寸建议，避免「已省略 → 更短 → 更省略」的自我收缩
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(m_text), fm.height());
}

// 宽度下限给 0：窄标题栏里标题可以完全让位给右侧按钮，高度仍保持一行。
QSize ElideLabel::minimumSizeHint() const
{
    return QSize(0, QFontMetrics(font()).height());
}

// 尺寸一变就重算省略；放在 resize 而不是 paintEvent 里，避免每帧重排文字。
void ElideLabel::resizeEvent(QResizeEvent *e)
{
    QLabel::resizeEvent(e);
    updateElide();
}

// ======================================================================
// BoxHeaderBar
// ======================================================================

// 构造：一条 MRowLayout 里塞「图标 + 标题 + 动作组」，动作组自己又是一个 MRowLayout，
// 这样右侧四项能整组贴边，不用给每个按钮单独算弹簧。
BoxHeaderBar::BoxHeaderBar(QWidget *parent)
    : MRowLayout(parent)
{
    setFixedHeight(kHeight);        // 标题栏高度写死，内容区由根列布局拉伸
    SetSpacing(6);
    SetMargin(12, 0, 6, 0);         // 左 12 留出图标呼吸位，右 6 让「⋯」不贴边
    SetHoverEnabled(false);         // 整行不做 hover 底色，高亮只留给右侧的按钮

    m_icon = new QLabel(this);
    m_icon->setFixedSize(20, 20);
    m_icon->setAlignment(Qt::AlignCenter);
    m_icon->setAttribute(Qt::WA_TransparentForMouseEvents);  // 图标不参与命中测试，按下直接落到标题栏

    m_title = new ElideLabel(this);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_actions = new MRowLayout(this);
    m_actions->SetSpacing(2);
    m_actions->SetHoverEnabled(false);
    m_actions->setAttribute(Qt::WA_TranslucentBackground);   // 动作组只是排版容器，不能画自己的底色

    m_member = new QLabel(m_actions);
    m_member->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_member->hide();               // 默认单成员，没有 "1/1" 可显示

    m_count = new QLabel(m_actions);
    m_count->setAttribute(Qt::WA_TransparentForMouseEvents);

    // 统一造按钮：26x26 固定尺寸 + NoFocus（不进 Tab 链，也不画焦点框）
    auto makeButton = [this](const QString &glyph, const QString &tip)
    {
        auto *b = new QToolButton(m_actions);
        b->setText(glyph);
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setFixedSize(26, 26);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    m_collapse = makeButton(QStringLiteral("▾"), QStringLiteral("折叠 / 展开"));
    m_menu = makeButton(QStringLiteral("⋯"), QStringLiteral("更多操作"));

    m_actions->RowAddWidget(m_member, 0, Qt::AlignVCenter);
    m_actions->RowAddWidget(m_count, 0, Qt::AlignVCenter);
    m_actions->RowAddWidget(m_collapse, 0, Qt::AlignVCenter);
    m_actions->RowAddWidget(m_menu, 0, Qt::AlignVCenter);

    // 不用 RowPinToRight：它补的尾部弹簧会和标题按 1:1 抢空间，标题就总被挤到一半。
    // 这里直接靠标题的拉伸把右侧动作组顶到行尾。
    RowAddWidget(m_icon, 0, Qt::AlignVCenter);
    RowAddWidget(m_title, 1, Qt::AlignVCenter);   // 只有标题 stretch=1，多余宽度全归它
    RowAddWidget(m_actions, 0, Qt::AlignVCenter);
    RowClearSpacers();              // 确保行尾没有历史弹簧，标题才是唯一可伸展项

    connect(m_collapse, &QToolButton::clicked, this, &BoxHeaderBar::collapseRequested);
    connect(m_menu, &QToolButton::clicked, this, [this]
    {
        // 从按钮右下角弹出，菜单不会盖住「⋯」本身；坐标必须先 mapToGlobal
        emit menuRequested(m_menu->mapToGlobal(QPoint(m_menu->width(), m_menu->height())));
    });

    applyTheme();
}

// 换肤：字号随形态（胶囊更小）走，颜色全部取自 AppSettings 的主题令牌。
void BoxHeaderBar::applyTheme()
{
    const AppSettings &st = AppSettings::get();

    m_title->setFont(st.uiFont(m_capsule ? 12 : 14, QFont::DemiBold));  // 胶囊省高度，字号降 2px
    m_title->setStyleSheet(QStringLiteral("color:%1;").arg(st.textColor().name()));

    m_count->setFont(st.uiFont(m_capsule ? 10 : 11));
    m_count->setStyleSheet(QStringLiteral("color:%1;").arg(st.subTextColor().name()));

    m_member->setFont(st.uiFont(m_capsule ? 10 : 11, QFont::DemiBold));
    m_member->setStyleSheet(QStringLiteral("color:%1;").arg(st.accent().name()));  // 强调色，和计数区分开

    // 按钮只在 hover/pressed 上底色，常态透明，避免标题栏出现四个方块
    const QString btnCss = QStringLiteral(
                               "QToolButton{color:%1;background:transparent;border:none;border-radius:6px;}"
                               "QToolButton:hover{background:%2;}"
                               "QToolButton:pressed{background:%3;}")
                               .arg(st.subTextColor().name(),
                                    st.hoverColor().name(QColor::HexArgb),
                                    st.borderColor().name(QColor::HexArgb));
    m_collapse->setStyleSheet(btnCss);
    m_menu->setStyleSheet(btnCss);
}

// 形态切换：高度、边距、按钮可见性、图标尺寸一起改，改完必须重套一遍样式才能让字号跟上。
void BoxHeaderBar::setCapsuleMode(bool capsule)
{
    if (m_capsule == capsule)
        return;
    m_capsule = capsule;

    if (capsule) {
        setFixedHeight(kCapsuleHeight);
        SetMargin(11, 0, 9, 0);     // 胶囊左右对称留白，视觉上才像一颗「药丸」
        SetSpacing(7);
        m_collapse->hide();         // 胶囊本身已是最小形态，折叠/更多操作没有入口
        m_menu->hide();
        m_icon->setFixedSize(16, 16);
    } else {
        setFixedHeight(kHeight);
        SetMargin(12, 0, 6, 0);
        SetSpacing(6);
        m_collapse->show();
        m_menu->show();
        m_icon->setFixedSize(20, 20);
    }
    applyTheme();
    setFolderIcon(m_folderIcon);   // 图标缓存的是原图，尺寸变了要重新取 pixmap
    m_title->setFullText(m_title->fullText());  // 字号变了，省略结果也得按新字体重算
}

// 胶囊宽度估算：窗口侧 applyGeometryForState 会再夹到 [96, 340]，这里先给个宽松上限。
int BoxHeaderBar::capsuleWidthHint() const
{
    // 直接按字体量算：左右边距 + 图标 + 三处间距 + 标题 + 数量 + 余量
    // 各项是经验值：20(左右边距合计) + 16(图标) + 21(间距) + 26(余量)，与 SetMargin/SetSpacing 大致对应
    const QFontMetrics tf(m_title->font());
    const QFontMetrics cf(m_count->font());
    const int w = 20 + 16 + 21 + tf.horizontalAdvance(m_title->fullText())
                  + cf.horizontalAdvance(m_count->text()) + 26;
    return qBound(96, w, 360);  // 太窄会缺字，太宽胶囊就不像胶囊了
}

// 标题栏只存一份文本，省略与否由 ElideLabel 按当前宽度决定。
void BoxHeaderBar::setTitle(const QString &title)
{
    m_title->setFullText(title);
}

// 负数 = 该盒子没有「项数」概念（组件），直接隐藏；0 单独显示「空」比「0 项」更自然。
void BoxHeaderBar::setCount(int count)
{
    if (count < 0) { // 组件没有计数概念
        m_countWanted = false;
        m_count->hide();
        return;
    }
    m_countWanted = true;
    m_count->setText(count > 0 ? QStringLiteral("%1 项").arg(count) : QStringLiteral("空"));
    updateResponsive();
}

// 成员徽标："1/3" 这种形式由调用方拼好；这里只记住「想不想要显示」，可见性交给宽度判定。
void BoxHeaderBar::setMemberBadge(const QString &text)
{
    m_memberWanted = !text.isEmpty();
    m_member->setText(text);
    updateResponsive();
}

/** 标题栏很窄时先收起徽标，把宽度让给标题，避免标题只剩「…」 */
void BoxHeaderBar::updateResponsive()
{
    // 两个门槛是经验值：250 以下先砍计数，215 以下连成员徽标也砍，保证标题至少能露几个字。
    // 只在 resize/setXXX 时算，不做逐帧判断。
    const int w = width();
    m_count->setVisible(m_countWanted && w >= 250);
    m_member->setVisible(m_memberWanted && w >= 215);
}

// 滚轮切成员：只在成员徽标可见（即确实有成员可切）时拦截，否则滚轮继续交给基类。
void BoxHeaderBar::wheelEvent(QWheelEvent *e)
{
    // 只有在标题栏上滚动才切换成员，避免误触
    if (m_member->isVisible()) {
        const int delta = e->angleDelta().y() > 0 ? -1 : 1;  // 上滚=上一个，符合翻页方向
        emit memberStepRequested(delta);
        e->accept();
        return;
    }
    MRowLayout::wheelEvent(e);
}

// 组件徽标：和 setCount 共用 m_count 这个 QLabel（同一时刻只会有一个数据源生效）。
void BoxHeaderBar::setBadge(const QString &text)
{
    if (text.isEmpty()) {
        m_countWanted = false;
        m_count->hide();
        return;
    }
    m_countWanted = true;
    m_count->setText(text);
    updateResponsive();
}

// 只改箭头字形；窗口真正的折叠状态在 FileBoxWindow::m_cfg.collapsed 里。
void BoxHeaderBar::setCollapsed(bool collapsed)
{
    m_collapse->setText(collapsed ? QStringLiteral("▸") : QStringLiteral("▾"));
}

// 缓存原图再按当前形态取对应像素尺寸，避免在 16/20 之间反复切换时丢失矢量信息。
void BoxHeaderBar::setFolderIcon(const QIcon &icon)
{
    m_folderIcon = icon;
    const int px = m_capsule ? 16 : 20;
    m_icon->setPixmap(icon.pixmap(px, px));
}

// 内联重命名：编辑框只在第一次进入时创建，之后复用；几何位置贴着标题所在矩形稍作外扩。
void BoxHeaderBar::beginEditTitle()
{
    if (m_editing)
        return;
    const AppSettings &st = AppSettings::get();

    if (!m_edit) {
        m_edit = new QLineEdit(this);
        m_edit->setFrame(false);
        m_edit->installEventFilter(this);   // 只为拦 Esc，见 eventFilter
        connect(m_edit, &QLineEdit::editingFinished, this, [this] { finishEdit(true); });
        connect(m_edit, &QLineEdit::returnPressed, this, [this] { finishEdit(true); });
    }
    m_edit->setFont(st.uiFont(13, QFont::DemiBold));
    m_edit->setStyleSheet(
        QStringLiteral("QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:6px;padding:1px 6px;}")
            .arg(st.isDark() ? "#2A2C33" : "#FFFFFF",
                 st.textColor().name(),
                 st.accent().name()));

    m_editing = true;
    m_edit->setText(m_title->fullText());   // 编辑的是完整标题，不是省略后的文本
    m_edit->setGeometry(m_title->geometry().adjusted(-4, -5, 4, 5));  // 外扩几像素，文字不贴边
    m_edit->show();
    m_edit->raise();                        // 盖在标题与右侧按钮之上
    m_edit->setFocus(Qt::OtherFocusReason);
    m_edit->selectAll();                    // 直接输入即覆盖旧名
}

// 结束编辑：先摘 m_editing 再判空/判重，避免 titleEdited 的回调里再次进入。
void BoxHeaderBar::finishEdit(bool commit)
{
    if (!m_editing)
        return;
    m_editing = false;
    const QString text = m_edit->text().trimmed();
    m_edit->hide();
    // 空标题直接丢弃（窗口标题不允许为空），与原标题相同也不发信号，减少无意义的落盘
    if (commit && !text.isEmpty() && text != m_title->fullText())
        emit titleEdited(text);
}

// 尺寸变化时：先让基类重排，再做响应式收敛，最后让编辑框跟上标题的新位置。
void BoxHeaderBar::resizeEvent(QResizeEvent *e)
{
    MRowLayout::resizeEvent(e);
    updateResponsive();
    if (m_edit && m_editing)
        m_edit->setGeometry(m_title->geometry().adjusted(-4, -5, 4, 5));
}

// 左键：只记录起点并发 pressedAt，真正的位移判断留给 mouseMoveEvent。
// 右键：直接弹菜单（不必等 release，和系统标题栏行为一致）。
void BoxHeaderBar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_pressed = true;
        m_dragging = false;
        m_pressGlobal = e->globalPosition().toPoint();
        emit pressedAt(m_pressGlobal);
        e->accept();
        return;
    }
    if (e->button() == Qt::RightButton) {
        emit menuRequested(e->globalPosition().toPoint());
        e->accept();
        return;
    }
    MRowLayout::mousePressEvent(e);
}

// 4px 阈值用曼哈顿距离：手抖不算拖动，越过阈值后每帧都发 draggedTo（窗口侧 1:1 跟手）。
void BoxHeaderBar::mouseMoveEvent(QMouseEvent *e)
{
    if (m_pressed && (e->buttons() & Qt::LeftButton)) {
        const QPoint g = e->globalPosition().toPoint();
        if (!m_dragging && (g - m_pressGlobal).manhattanLength() >= 4)
            m_dragging = true;
        if (m_dragging)
            emit draggedTo(g);
        e->accept();
        return;
    }
    MRowLayout::mouseMoveEvent(e);
}

// 松手的二选一：拖动过 → dragFinished（窗口吸附+落盘），没拖动 → clicked（折叠态可展开）。
void BoxHeaderBar::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_pressed && e->button() == Qt::LeftButton) {
        m_pressed = false;
        if (m_dragging) {
            m_dragging = false;
            emit dragFinished();
        } else {
            emit clicked();
        }
        e->accept();
        return;
    }
    MRowLayout::mouseReleaseEvent(e);
}

// 双击进入重命名。先清 pressed/dragging：否则双击后的 release 会被当成一次「点击」，
// 在折叠态下会顺手把窗口展开。
void BoxHeaderBar::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_pressed = false;
        m_dragging = false;
        beginEditTitle();
        e->accept();
        return;
    }
    MRowLayout::mouseDoubleClickEvent(e);
}

// 标题栏空白处右键：和「⋯」按钮走同一个菜单入口。
void BoxHeaderBar::contextMenuEvent(QContextMenuEvent *e)
{
    emit menuRequested(e->globalPos());
    e->accept();
}

// 只拦编辑框的 Esc（放弃修改）；其余按键（回车提交）交给 QLineEdit 自身。
bool BoxHeaderBar::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_edit && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Escape) {
            finishEdit(false);
            return true;
        }
    }
    return MRowLayout::eventFilter(obj, e);
}

// ======================================================================
// FileBoxWindow
// ======================================================================

// 构造：先备好成员/归属，再搭 UI，最后灌几何并应用外观。
// 折叠态不在 setGeometry 前应用，否则会被展开尺寸覆盖。
FileBoxWindow::FileBoxWindow(const BoxConfig &cfg, QWidget *parent)
    : QWidget(parent)
    , m_cfg(cfg)
{
    m_hostId = cfg.id;   // 宿主 id 一经构造就固定，组合切成员时只换 m_cfg
    m_members = { cfg };
    m_cur = 0;

    setAttribute(Qt::WA_TranslucentBackground);   // 圆角外的四角要透出桌面
    setMinimumSize(50, BoxHeaderBar::kHeight);    // 至少留得下一条标题栏
    setWindowTitle(cfg.title);
    setMouseTracking(true);
    setAcceptDrops(true);

    buildUi();

    // 不在这里抬到 220x160：组件有自己的最小尺寸，窗口在 applyGeometryForState 里夹紧
    // 80/120 只是防空尺寸的兜底，真正的下限由面板决定
    m_expandedHeight = qMax(80, cfg.geometry.height());
    m_expandedWidth = qMax(120, cfg.geometry.width());
    setGeometry(cfg.geometry);

    // 关键时序：先把 collapsed 清掉走一遍「展开态」的完整外观初始化（面板/几何/材质），
    // 最后再 setCollapsed(true)，这样折叠分支需要的一切（标题文本、计数）都已就绪。
    const bool startCollapsed = cfg.collapsed;
    m_cfg.collapsed = false;
    applyAppearance();
    if (startCollapsed)
        setCollapsed(true);

    refresh();
}

// 析构：qApp 的过滤器不随窗口销毁自动摘除，必须手动移除，否则 qApp 会持有悬空指针。
FileBoxWindow::~FileBoxWindow()
{
    if (m_filtering)
        qApp->removeEventFilter(this);
}

// 搭 UI：顺序有依赖 —— 模型必须早于视图 attach，面板必须早于根布局添加；
// 三个内容控件（列表 / 堆叠卡 / 组件面板）都进同一个根列布局，靠可见性切换而不是增删布局项。
void FileBoxWindow::buildUi()
{
    const AppSettings &st = AppSettings::get();

    m_model = new FileItemModel(this);
    m_model->setSortMode(m_cfg.effectiveSortMode());   // 存的是 -1 时在这里解析成全局默认
    m_model->setShowHidden(st.showHidden);
    m_model->setManualOrder(m_cfg.manualOrder);
    m_model->setFolder(m_cfg.folder);

    m_view = new FileListView(this);
    m_view->attach(m_model, m_cfg.id);   // 第二个参数写进内部拖拽 MIME，用来判断是否同盒内拖动

    m_stackCard = new StackCard(this);
    m_stackCard->setModel(m_model);
    m_stackCard->hide();                 // 堆叠模式才显示，平时不占布局空间

    if (m_cfg.isWidget()) {
        createPanel();                   // 组件盒先建面板，下面根布局才能把它加进去
    }

    m_header = new BoxHeaderBar(this);
    m_header->setTitle(m_cfg.title);

    // 窗口本身要能拿到焦点，快捷键才生效（QWidget 默认 NoFocus）
    setFocusPolicy(Qt::StrongFocus);

    m_root = new MColumnLayout(this);
    m_root->ColumnFillHost(this);        // 根容器铺满整窗，边距/间距归零
    m_root->SetMargin(0);
    m_root->SetSpacing(0);
    m_root->ColumnAddWidget(m_header, 0);       // 标题栏固定高，stretch=0
    m_root->ColumnAddWidget(m_view, 1);
    m_root->ColumnAddWidget(m_stackCard, 1);    // 与 m_view 同为 stretch=1，但两者不会同时可见
    if (m_panel)
        m_root->ColumnAddWidget(m_panel, 1);

    // ---- 标题栏交互 ----
    // 按下只记起点：拖动位移以「按下时的窗口位置 + 鼠标位移」计算，不做逐帧累加，避免误差漂移。
    connect(m_header, &BoxHeaderBar::pressedAt, this, [this](const QPoint &g)
    {
        m_dragWindowStart = pos();
        m_dragPressGlobal = g;
        m_dragging = false;
    });
    connect(m_header, &BoxHeaderBar::draggedTo, this, [this](const QPoint &g)
    {
        m_dragging = true;
        // 拖动过程严格 1:1 跟手，**不做网格吸附**：
        // 每帧取整会让窗口按格子跳，慢拖时看起来就是一顿一顿的抖动。
        move(m_dragWindowStart + (g - m_dragPressGlobal));
    });
    connect(m_header, &BoxHeaderBar::dragFinished, this, [this]
    {
        m_dragging = false;
        move(snappedPos(pos()));   // 松手时一次性对齐网格
        persist();
    });
    // 折叠态下点一下标题栏即展开；展开态点击不做任何事（避免误触）。
    connect(m_header, &BoxHeaderBar::clicked, this, [this]
    {
        if (m_cfg.collapsed)
            setCollapsed(false);
    });
    connect(m_header, &BoxHeaderBar::collapseRequested, this, [this] { toggleCollapsed(); });
    // 菜单在栈上创建：exec 是模态的，返回后即析构，不需要管理生命周期。
    connect(m_header, &BoxHeaderBar::menuRequested, this, [this](const QPoint &gp)
    {
        QMenu menu;
        styleMenu(&menu);
        buildHeaderMenu(&menu);
        menu.exec(gp);
    });
    // 改名要同时改 QWidget 的窗口标题（任务栏/辅助工具可见）和标题栏显示文本。
    connect(m_header, &BoxHeaderBar::titleEdited, this, [this](const QString &title)
    {
        m_cfg.title = title;
        setWindowTitle(title);
        m_header->setTitle(title);
        persist();
    });

    // ---- 内容视图 ----
    connect(m_view, &FileListView::openRequested, this, &FileBoxWindow::onOpenRequested);
    connect(m_view, &FileListView::contextMenuOnItem, this, &FileBoxWindow::onItemContextMenu);
    connect(m_view, &FileListView::contextMenuOnEmpty, this, &FileBoxWindow::onEmptyContextMenu);
    // 视图内部完成的改动（粘贴/删除/拖入）没有其它落盘路径，这里统一收口。
    connect(m_view, &FileListView::contentsMutated, this, [this]
    {
        updateCount();
        persist();
    });
    connect(m_view, &FileListView::iconSizeWheeled, this, [this](int size)
    {
        m_cfg.iconSize = size;
        persist();
    });
    connect(m_stackCard, &StackCard::clicked, this, &FileBoxWindow::openStackPopup);
    connect(m_header, &BoxHeaderBar::memberStepRequested, this, &FileBoxWindow::switchMember);

    // 快捷键走 QShortcut 而不是 keyPressEvent：焦点常落在搜索框 / 文件列表里，
    // 只靠 keyPressEvent 会收不到。
    // WindowShortcut 的生效范围是「本窗口及其子控件」，正好覆盖盒子内部任意焦点位置。
    auto addStep = [this](const QKeySequence &seq, int delta)
    {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, [this, delta] { switchMember(delta); });
    };
    addStep(QKeySequence(QStringLiteral("Ctrl+Tab")), 1);
    addStep(QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), -1);
    addStep(QKeySequence(QStringLiteral("Ctrl+PgDown")), 1);
    addStep(QKeySequence(QStringLiteral("Ctrl+PgUp")), -1);

    connect(m_model, &FileItemModel::contentsChanged, this, &FileBoxWindow::updateCount);
    // 文件夹被删/不可访问时模型会报 folderMissing，这里只需重绘，让 paintEvent 画出「文件夹不可用」
    connect(m_model, &FileItemModel::folderMissing, this, [this](const QString &) { update(); });
}

// 窗口标志：Tool 让它不出现在任务栏；无边框，并显式关掉系统投影（盒子是贴在桌面上的，不要悬浮阴影）。
// 注意 setWindowFlags 会重建原生窗口（窗口会先被隐藏），所以这里要自己补一次 show()。
void FileBoxWindow::applyFlags()
{
    Qt::WindowFlags f = Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
    // 二选一而不是「不加标志」：不置顶的盒子显式置底，始终待在普通窗口背后，不会挡工作区
    f |= m_cfg.alwaysOnTop ? Qt::WindowStaysOnTopHint : Qt::WindowStaysOnBottomHint;
    if (windowFlags() == f)
        return;   // 标志没变就别重建原生窗口，否则每次换肤都会闪一下
    const bool wasVisible = isVisible();
    setWindowFlags(f);
    if (wasVisible)
        show();
}

// 表面：材质（亚克力/云母/纯色）+ 圆角。圆角有两种实现，二选一：
// 系统圆角（Win11 DWM）效果最好但要求窗口不裁剪，所以成功时要 clearMask；否则退回自绘蒙版。
void FileBoxWindow::applySurface()
{
    const AppSettings &st = AppSettings::get();
    const auto material = static_cast<Platform::Material>(st.material);

    // tint 只是叠在材质上的薄薄一层色偏；云母本身有背板，alpha 要更小才不糊
    QColor tint = st.isDark() ? QColor(14, 16, 22, 24) : QColor(250, 250, 254, 26);
    if (material == Platform::Material::Mica)
        tint.setAlpha(st.isDark() ? 18 : 24);

    Platform::applyMaterial(this, material, tint);

    // 圆角只由窗口负责：优先让 DWM 裁（Win11，带抗锯齿），拿不到就退回窗口蒙版。
    // 设置里的「使用系统圆角」开关已经删掉 —— 自绘圆角那套会和 DWM 的半径打架，
    // 现在恒走系统圆角，老系统自动落到蒙版（两条路径互斥，蒙版留着会盖掉 DWM 的边缘）。
    m_usingMask = !Platform::setRoundedCorners(this, true);
    if (m_usingMask)
        Platform::applyRoundedMask(this, kRadius);
    else
        Platform::clearMask(this);
    update();
}

// 统一应用外观：内容视图参数 → 标题栏形态 → 头部过滤 → 窗口标志 → 内容可见性 → 几何 → 表面。
// 顺序不能随意换：几何依赖当前可见内容的最小尺寸，表面依赖最终高度（胶囊半径 = 高/2）。
void FileBoxWindow::applyAppearance()
{
    const AppSettings &st = AppSettings::get();

    m_model->setShowHidden(st.showHidden);
    const int vm = m_cfg.effectiveViewMode();
    // 视图只有「图标/列表」两种内部样式，堆叠(2)复用图标样式作为底层排版
    m_view->applyAppearance(vm == 1 ? 1 : 0, m_cfg.effectiveIconSize(), st.nameLines);
    m_stackCard->setIconSize(m_cfg.effectiveIconSize());

    m_header->setCapsuleMode(m_cfg.collapsed && m_cfg.effectiveCapsule());
    updateHeader();
    updateHeaderFilter(); // 构造期就要装上（applyMember 不一定被调用）

    // 组件面板自己缓存了配色（QSS / setTextColor / 染色图标），换主题时必须让它重设一遍，
    // 否则深色主题下音乐组件的文字还是浅色主题那套颜色。自绘型面板（天气）不用覆写这个钩子。
    if (m_panel)
        m_panel->applyTheme();

    applyFlags();
    updateBody();
    applyGeometryForState();
    applySurface();
}

// 新建组件时「落地」：把记忆的展开尺寸直接设成当前最小尺寸，再走一遍总闸。
// 只对展开态的组件盒有意义 —— 文件盒没有面板下限，折叠态又是固定高度，都不用收缩。
void FileBoxWindow::shrinkToMinimum()
{
    if (!m_cfg.isWidget() || m_cfg.collapsed)
        return;

    // 构造期 applyGeometryForState() 已经把 minimumWidth/Height 设成
    // 「面板 minimumSizeHint() + 标题栏高度」。把展开尺寸也置成这个下限，
    // 再走一遍 applyGeometryForState() 就会 resize(min, min)。
    m_expandedWidth = qMax(minimumWidth(), 1);
    m_expandedHeight = qMax(minimumHeight(), 1);
    applyGeometryForState();
    persist();   // 立刻持久化，避免用户直接关掉程序时新组件又变回默认大尺寸
}

// 外部整体替换配置：只有 id 对得上当前成员才回写 m_members，否则只影响当前显示的 m_cfg。
void FileBoxWindow::setConfig(const BoxConfig &cfg)
{
    m_cfg = cfg;
    if (m_cur >= 0 && m_cur < m_members.size() && m_members.at(m_cur).id == cfg.id)
        m_members[m_cur] = cfg;
    applyMember();
}

// 刷新内容：组件走面板自己的 refresh（可能触发网络/媒体查询），文件盒走模型重扫目录。
// 组件不走 model->refresh()，因为组件盒的 folder 是空的，扫了也没意义。
void FileBoxWindow::refresh()
{
    if (m_cfg.isWidget()) {
        if (m_panel)
            m_panel->refresh();
        updateHeader();
        return;
    }
    m_model->refresh();
    updateCount();
}

// 计数/徽标的唯一刷新点：文件盒读模型条目数，组件盒读面板 badgeText（空串表示不显示）。
void FileBoxWindow::updateCount()
{
    if (m_header) {
        if (m_cfg.isWidget())
            m_header->setBadge(m_panel ? m_panel->badgeText() : QString());
        else
            m_header->setCount(m_model->itemCount());
    }
    // 胶囊宽度是按「标题 + 计数」现算的，计数一变（比如 9 项 → 10 项）就得重新收一次几何
    if (m_cfg.collapsed && m_cfg.effectiveCapsule())
        applyGeometryForState(); // 胶囊宽度跟随「N 项」变化
    update();
}

// 内容区三选一：折叠时全隐；组件盒只有面板；文件盒在「图标/列表」与「堆叠卡」之间二选一。
// 用可见性切换而不是增删布局项，是为了避免反复操作布局导致尺寸抖动。
void FileBoxWindow::updateBody()
{
    const bool collapsed = m_cfg.collapsed;
    const bool widget = m_cfg.isWidget();
    const bool stacked = !widget && m_cfg.effectiveViewMode() == 2;
    m_view->setVisible(!collapsed && !widget && !stacked);
    m_stackCard->setVisible(!collapsed && !widget && stacked);
    if (m_panel)
        m_panel->setVisible(!collapsed);
}

// 头部一致性刷新：标题、计数、图标、成员徽标、折叠箭头、窗口标题，一次性同步。
// 这些值来源分散（面板/模型/配置），所以集中在这里做，避免各处零散 setText。
void FileBoxWindow::updateHeader()
{
    if (!m_header)
        return;

    m_header->applyTheme();   // 换肤后字号/颜色要重设，放在最前面
    m_header->setTitle(headerTitle());
    if (m_cfg.isWidget())
        m_header->setBadge(m_panel ? m_panel->badgeText() : QString());
    else
        m_header->setCount(m_model->itemCount());
    // 组件用现画的矢量图标；文件盒直接问系统要文件夹图标（可能拿到用户自定义图标）
    if (m_cfg.isWidget())
        m_header->setFolderIcon(iconForWidget(m_cfg.kind, m_cfg.pluginId));
    else
        m_header->setFolderIcon(QFileIconProvider().icon(QFileInfo(m_cfg.folder)));

    // 组件可以隐藏标题栏，让内容铺满整窗（移动 / 菜单改走 Alt+拖动与右键）
    // 文件盒不受 hideHeader 影响 —— 没有标题栏就没法拖动/折叠了
    const bool headerVisible = !m_cfg.hideHeader || !m_cfg.isWidget();
    m_header->setVisible(headerVisible);
    if (m_panel)
        m_panel->setHeaderHidden(!headerVisible);

    // 单成员不给 "1/1"，否则每个盒子都挂个无信息量的角标
    m_header->setMemberBadge(m_members.size() > 1
                                 ? QStringLiteral("%1/%2").arg(m_cur + 1).arg(m_members.size())
                                 : QString());
    m_header->setCollapsed(m_cfg.collapsed);
    setWindowTitle(headerTitle());   // 折叠/胶囊时任务栏与 Alt+Tab 也显示同一个名字
}

// 标题优先级：面板自报（可能含运行期信息，如「天气 · 北京」）> 用户自定义标题 > 注册表默认名。
QString FileBoxWindow::headerTitle() const
{
    if (m_cfg.isWidget() && m_panel) {
        const QString t = m_panel->panelTitle();
        if (!t.isEmpty())
            return t;
    }
    if (!m_cfg.title.isEmpty())
        return m_cfg.title;
    return WidgetRegistry::displayNameFor(m_cfg.kind, m_cfg.pluginId);
}

// ---------------------------------------------------------------- 组件面板

// 组件面板创建：失败（未知 kind / 插件缺失）时保持 m_panel 为 nullptr，
// 后续所有用到 m_panel 的地方都做了判空，窗口退化成「只有标题栏」而不是崩溃。
void FileBoxWindow::createPanel()
{
    if (m_panel)
        return;

    m_panel = WidgetRegistry::create(m_cfg, this);
    if (!m_panel) {
        m_panel = nullptr;
        return;
    }
    // 面板内部状态变了（改城市、切歌）：标题栏要跟着变，同时落盘
    connect(m_panel, &WidgetPanel::panelChanged, this, [this]
    {
        updateHeader();
        updateCount();
        persist();
    });
}

// 销毁面板要分三步：先从布局摘掉（否则布局仍按它的最小尺寸约束窗口），
// 再摘掉父对象（立刻脱离控件树，不会再被布局/绘制），最后 deleteLater 交给事件循环回收。
// 先置空 m_panel 是关键：deleteLater 之前若有回调进来，看到的是「没有面板」而不是野指针。
void FileBoxWindow::destroyPanel()
{
    if (!m_panel)
        return;
    WidgetPanel *p = m_panel;
    m_panel = nullptr;
    if (m_root)
        m_root->ColumnRemoveWidget(p);
    p->hide();
    p->setParent(nullptr);
    p->deleteLater();
}

// 切换成员后的「重新加载」：先把模型指向新成员的目录，再按需重建组件面板。
// 文件盒只在 folder 非空时才 setFolder —— 避免空配置把模型已有的目录视图清掉。
void FileBoxWindow::applyMember()
{
    if (!m_cfg.isWidget()) {
        m_model->setShowHidden(AppSettings::get().showHidden);
        m_model->setManualOrder(m_cfg.manualOrder);
        m_model->setSortMode(m_cfg.effectiveSortMode());
        if (!m_cfg.folder.isEmpty())
            m_model->setFolder(m_cfg.folder);
    } else {
        m_model->setFolder(QString());   // 组件成员没有目录，确保模型不再监听旧文件夹
    }

    // 组件面板与成员强绑定（城市/播放器状态都不同），换成员必须整体重建而不是复用
    destroyPanel();
    if (m_cfg.isWidget()) {
        createPanel();
        if (m_panel && m_root)
            m_root->ColumnAddWidget(m_panel, 1);
    }

    applyAppearance();
    updateHeaderFilter();   // 新成员的 hideHeader 可能不同，过滤器的装/卸要跟着变
    refresh();
}

// 由 App 在构造后调用：members[0] 必须是宿主；currentId 找不到时静默回退到 0 号。
void FileBoxWindow::setGroupMembers(const QVector<BoxConfig> &members, const QString &currentId)
{
    if (members.isEmpty())
        return;
    m_members = members;
    m_cur = 0;
    for (int i = 0; i < m_members.size(); ++i) {
        if (m_members.at(i).id == currentId) {
            m_cur = i;
            break;
        }
    }
    m_cfg = m_members.at(m_cur);
    applyMember();
}

// 切换成员：环形回绕，delta 可正可负（也允许 |delta| > 1，虽然目前调用方只传 ±1）。
void FileBoxWindow::switchMember(int delta)
{
    if (m_members.size() < 2 || delta == 0)
        return;

    m_members[m_cur] = m_cfg; // 保存当前成员的即时状态
    // 先取模再加 n 再取模：C++ 的 % 对负数结果为负，直接取模会越界
    const int n = int(m_members.size());
    m_cur = ((m_cur + delta) % n + n) % n;
    m_cfg = m_members.at(m_cur);

    applyMember();
    // 由 App 记录宿主的 currentMember，重启后才能回到同一成员
    emit currentMemberChanged(m_hostId, m_cfg.id);
    persist();
}

// 尺寸总闸：所有形态切换最后都汇到这里，保证「折叠高度 / 上下限 / 锁定」三件事只在一处生效。
// 记住一条：setFixedSize 会同时改 min 与 max，所以展开分支必须重新 setMinimum/Maximum 把它解开。
void FileBoxWindow::applyGeometryForState()
{
    if (m_cfg.collapsed) {
        if (m_cfg.effectiveCapsule()) {
            // 胶囊宽度跟着标题与计数走：先取 header 的估算值，再夹到 [96, 340]
            const int w = qBound(96, m_header->capsuleWidthHint(), 340);
            setFixedSize(w, BoxHeaderBar::kCapsuleHeight);
        } else {
            // 折叠成「标题条」：高度锁死，宽度可拖但不得窄于 150（再窄标题和按钮就打架）
            setMinimumSize(150, BoxHeaderBar::kHeight);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            setFixedHeight(BoxHeaderBar::kHeight);
            if (width() < 150)
                resize(150, BoxHeaderBar::kHeight);
        }
    } else {
        // 组件按自己的最紧凑版式给出下限，避免窗口被拖得过小导致内容被裁
        int minW = 220;
        int minH = 140;
        if (m_cfg.isWidget() && m_panel) {
            const QSize minContent = m_panel->minimumSizeHint();
            // 标题栏隐藏时内容可以顶到窗口上沿，高度里就不该再算 42px
            const int headerH = m_cfg.hideHeader ? 0 : BoxHeaderBar::kHeight;
            minW = qMax(140, minContent.width());   // 140 是兜底，防止面板自报过窄
            minH = minContent.height() + headerH;
            setMinimumSize(minW, minH);

            // 组件可以给出自己的上限（目前只有音乐组件给了 880x680，再大就只剩空白）
            // 无效尺寸（QSize() 或任一维为 0）表示「不限制」
            const QSize maxContent = m_panel->maximumSizeHint();
            if (maxContent.isValid() && maxContent.width() > 0 && maxContent.height() > 0)
                setMaximumSize(qMax(minW, maxContent.width()),
                               qMax(minH, maxContent.height() + headerH));
            else
                setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        } else {
            // 文件盒没有内容自适应需求，给出通用的可用下限即可
            setMinimumSize(50, 120);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        // 记忆尺寸优先，但不能小于下限：用户上次拖大了就保持，拖小了则被抬回 min
        resize(qMax(minW, m_expandedWidth), qMax(minH, m_expandedHeight));
    }

    // 锁定大小：把当前尺寸钉死（折叠 / 展开仍能切，只是拖边框不再改变宽高）
    // 折叠态本身就固定尺寸，不需要再钉一次；展开时靠上面的 setMinimum/Maximum 解开锁定
    if (m_cfg.lockSize && !m_cfg.collapsed)
        setFixedSize(size());
}

// 折叠/展开：状态机 + 尺寸记忆 + 高度动画。m_applyingState 防止内部调用链再次进来把状态搅乱。
//
// 动画只改窗口尺寸，**内容区固定高度不动**：窗口变矮时内容是被裁掉的（卷帘效果），
// 而不是让布局每帧重新压缩一次——组件面板重排一次就是几十毫秒，动画会抖成幻灯片。
void FileBoxWindow::setCollapsed(bool collapsed)
{
    if (m_applyingState)
        return;
    if (m_cfg.hideHeader && collapsed)
        return; // 头部已隐藏，折叠后窗口就空了
    if (collapsed == m_cfg.collapsed) {
        applyGeometryForState();   // 幂等调用：至少保证几何是最新的
        return;
    }

    m_applyingState = true;

    if (collapsed) {
        // 记录展开态尺寸，供还原使用
        // 160/220 是下限兜底：避免在异常小的尺寸下折叠，还原出一个小到没法用的窗口
        m_expandedHeight = qMax(160, height());
        m_expandedWidth = qMax(220, width());
    }

    const QSize from = size();     // 动画起点 = 切换状态前的尺寸

    m_cfg.collapsed = collapsed;
    m_header->setCollapsed(collapsed);
    m_header->setCapsuleMode(collapsed && m_cfg.effectiveCapsule());
    // 展开时内容立刻显示（动画中靠窗口裁切一点点露出来）；
    // 收起时先别隐藏，否则内容会在动画开始那一帧直接消失，看不到卷上去的过程
    if (!collapsed)
        updateBody();
    applyGeometryForState();       // 算出并直接应用「目标状态」的尺寸与上下限
    const QSize to = size();

    const bool animate = isVisible() && from != to;
    if (!animate) {
        updateBody();              // 收起分支：没有动画就立刻隐藏内容
        applySurface();            // 圆角半径/材质依赖高度，必须在几何确定之后再刷
        persist();
        m_applyingState = false;
        return;
    }

    startCollapseAnimation(from, to, collapsed);
    m_applyingState = false;
}

/**
 * 启动折叠/展开动画。
 * @param from      起点尺寸（切换前的窗口大小）
 * @param to        终点尺寸（目标状态的大小）
 * @param collapsing true = 正在收起
 *
 * 步骤：把上下限放开到能覆盖起止尺寸的区间（否则 resize 会被 min/max 夹住，动画一动就被掐断），
 * 回退到起点尺寸，钉住内容区高度，再让 geometry 动画跑起来。
 */
void FileBoxWindow::startCollapseAnimation(const QSize &from, const QSize &to, bool collapsing)
{
    const int headerH = m_cfg.hideHeader ? 0 : BoxHeaderBar::kHeight;

    // 先立动画标志，再做任何 resize。
    // 否则下面那次「退回起点尺寸」会被 resizeEvent 里的折叠态纠偏抓住：
    // 此刻 m_cfg.collapsed 已经是目标状态，它一看高度不等于标题栏高度就直接 setFixedHeight 按回去，
    // 顺手把 max 也钉死，之后动画每一帧都被夹住，看着就是「一步到位」。
    m_animating = true;

    // 上限放到最大、下限放到「起止高度里更小的那个」，中间任何一帧都不会被夹
    setMinimumSize(qMin(from.width(), to.width()), qMin(from.height(), to.height()));
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    // 内容区在动画期间保持一个固定高度：收起时用当前高度（内容不动、被窗口裁掉），
    // 展开时用目标高度（内容一次排好，之后只是逐渐露出来）
    lockBodyHeight(qMax(0, (collapsing ? from.height() : to.height()) - headerH));

    resize(from);   // applyGeometryForState() 已经把窗口改成目标尺寸了，这里退回起点

    if (!m_collapseAnim) {
        m_collapseAnim = new QPropertyAnimation(this, "geometry", this);
        // 180ms + OutCubic：足够看出「展开」的过程，又不至于让点开盒子有拖沓感
        m_collapseAnim->setDuration(180);
        m_collapseAnim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_collapseAnim, &QPropertyAnimation::finished,
                this, &FileBoxWindow::finishCollapseAnimation);
    }
    m_collapseAnim->stop();
    // 动画 geometry 而不是 size：位置钉死在起点的左上角，只有宽高变化
    m_collapseAnim->setStartValue(QRect(pos(), from));
    m_collapseAnim->setEndValue(QRect(pos(), to));
    m_collapseAnim->start();
}

/**
 * 动画收尾：解除内容区的高度锁定 → 按目标状态隐藏内容 → 重建上下限 → 刷表面 → 落盘。
 * 顺序不能换：applyGeometryForState() 会 resize，必须在解除固定高度之后调用，
 * 否则内容区还钉着旧高度，窗口最小高度会被撑住。
 */
void FileBoxWindow::finishCollapseAnimation()
{
    m_animating = false;
    lockBodyHeight(0);        // 0 = 解除固定高度
    updateBody();             // 收起态在这里才真正隐藏内容
    applyGeometryForState();  // 重建 min/max，并把尺寸落到记忆值
    applySurface();           // 蒙版/圆角按最终尺寸重刷
    persist();
}

/**
 * 动画期间给内容区钉一个固定高度。
 * @param h 内容区高度（px）；<= 0 表示解除固定、交回布局
 *
 * 三个内容控件（列表 / 堆叠卡 / 组件面板）同一时刻只有一个可见，
 * 全部钉一遍最省事，不可见的钉了也没有副作用。
 */
void FileBoxWindow::lockBodyHeight(int h)
{
    QWidget *bodies[] = { m_view, m_stackCard, m_panel };
    for (QWidget *w : bodies) {
        if (!w)
            continue;
        if (h <= 0) {
            w->setMinimumHeight(0);
            w->setMaximumHeight(QWIDGETSIZE_MAX);
        } else {
            w->setFixedHeight(h);
        }
    }
}

// 折叠/展开取反；所有约束（隐藏头部、重入保护）都在 setCollapsed 里，这里不做判断。
void FileBoxWindow::toggleCollapsed()
{
    setCollapsed(!m_cfg.collapsed);
}

// 隐藏头部：只对组件开放。隐藏前必须强制展开，否则窗口会退化成一条没有内容的标题条，
// 而折叠态的还原入口（点标题栏）恰恰也被隐藏了，就再也展开不回来。
void FileBoxWindow::setHeaderHidden(bool hidden)
{
    if (m_cfg.hideHeader == hidden)
        return;

    if (hidden && m_cfg.collapsed)
        setCollapsed(false); // 头部没了，折叠就无从展开

    m_cfg.hideHeader = hidden;
    updateHeaderFilter();
    applyAppearance();
    persist();
}

// 应用级事件过滤器只在「组件 + 隐藏头部」时需要；按需装卸，避免每个窗口都参与全局事件分发。
// 用 m_filtering 单独记录状态，而不是每次去猜 qApp 上有没有装。
void FileBoxWindow::updateHeaderFilter()
{
    const bool need = m_cfg.hideHeader && m_cfg.isWidget();
    if (need == m_filtering)
        return;

    if (need) {
        qApp->installEventFilter(this);
        m_filtering = true;
    } else {
        qApp->removeEventFilter(this);
        m_filtering = false;
    }
}

// 隐藏头部的组件的「最后手段」：窗口内部已没有标题栏可拖，也没有统一的事件入口，
// 于是在 qApp 上拦截本窗口范围内的事件，自己实现拖动与右键菜单。
// 注意所有分支只在真正消费事件时 return true；其余落到 QWidget::eventFilter（通常即不拦截），
// 让按钮、滑块的点击/拖动照常工作。
bool FileBoxWindow::eventFilter(QObject *obj, QEvent *e)
{
    // 只在「组件 + 隐藏头部」时接管：拖动移动，右键任意位置出菜单
    if (!m_filtering || !m_cfg.hideHeader)
        return QWidget::eventFilter(obj, e);

    // 应用级过滤器拿到的事件可能发给了 QWindow（QWidgetWindow），也可能是某个 QWidget，
    // 两种情况都要认，否则鼠标按下会被 QWindow 直接吃掉，根本到不了这里。
    bool mine = false;
    if (auto *w = qobject_cast<QWidget *>(obj))
        mine = (w == this || isAncestorOf(w));
    else if (auto *win = qobject_cast<QWindow *>(obj))
        mine = (win == windowHandle());

    if (!mine)
        return QWidget::eventFilter(obj, e);

    switch (e->type()) {
    case QEvent::ContextMenu: {
        // 没有标题栏就没有「⋯」入口，右键任意位置都弹同一个菜单
        auto *ce = static_cast<QContextMenuEvent *>(e);
        QMenu menu;
        styleMenu(&menu);
        buildHeaderMenu(&menu);
        menu.exec(ce->globalPos());
        return true;
    }
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() != Qt::LeftButton)
            break;

        const QPoint gp = me->globalPosition().toPoint();
        const bool alt = me->modifiers().testFlag(Qt::AltModifier);

        // 没有标题栏时按住任意空白处就能拖；按钮 / 进度条这类控件上不抢事件，
        // 否则点播放、拖进度都会变成拖窗口。想在哪都能拖时按住 Alt。
        if (!alt) {
            bool interactive = false;
            // 从命中的最内层控件往上走：命中的是标签/图标等装饰控件时，
            // 只要祖先里有可交互控件就仍算「可交互」，避免误判。
            for (QWidget *p = childAt(mapFromGlobal(gp)); p && p != this; p = p->parentWidget()) {
                // qdwInteractive：控件自己声明「我要吃鼠标」（例如可点击跳转的歌词视图），
                // 这种控件上按下不应该变成拖窗口
                if (p->property("qdwInteractive").toBool()
                    || p->inherits("QAbstractButton") || p->inherits("QAbstractSlider")
                    || p->inherits("QLineEdit") || p->inherits("QComboBox")
                    || p->inherits("QAbstractItemView") || p->inherits("QAbstractSpinBox")) {
                    interactive = true;
                    break;
                }
            }
            if (interactive)
                break; // 落在可交互控件上，交给控件自己处理
        }

        // 按下就先假定「在拖动」，等 release 时再按 m_dragMoved 判定是否真发生过位移
        m_dragWindowStart = pos();
        m_dragPressGlobal = gp;
        m_dragging = true;
        m_dragMoved = false;
        e->accept();
        return true;
    }
    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent *>(e);
        if (!m_dragging || !(me->buttons() & Qt::LeftButton))
            break;

        const QPoint gp = me->globalPosition().toPoint();
        if (!m_dragMoved) {
            // 手抖几像素不算拖动，避免点一下窗口就挪位
            // 阈值用系统的 startDragDistance（约 10px），比标题栏那条 4px 更保守
            if ((gp - m_dragPressGlobal).manhattanLength() < QApplication::startDragDistance())
                return true;   // 已消费：否则会被当成普通移动传给子控件
            m_dragMoved = true;
        }

        move(m_dragWindowStart + (gp - m_dragPressGlobal));
        e->accept();
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (m_dragging && static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
            const bool moved = m_dragMoved;
            m_dragging = false;
            m_dragMoved = false;
            if (moved) {
                move(snappedPos(pos()));
                persist();
            }
            // 无论是否真的移动都消费掉 release：press 已被消费，再让 release 流到子控件会造成「半次点击」
            e->accept();
            return true;
        }
        break;
    default:
        break;
    }

    return QWidget::eventFilter(obj, e);
}

// 重命名入口：直接转给标题栏的内联编辑框。
// 注意「隐藏头部」的组件里 m_header 不可见，编辑框也会跟着不可见，改名前需要先恢复标题栏。
void FileBoxWindow::beginRenameBox()
{
    m_header->beginEditTitle();
}

// 堆叠展开：弹窗是独立顶层窗口（parent 传 nullptr，自带置顶与 Esc 关闭），
// 这里用成员指针持住并复用同一个实例；数据源始终是 m_model，弹窗内的改动通过 filesMutated 回灌。
void FileBoxWindow::openStackPopup()
{
    if (!m_model || m_model->itemCount() == 0) {
        QMessageBox::information(this, QStringLiteral("堆叠为空"),
                                 QStringLiteral("把文件拖进这个整理盒，再点击堆叠展开。"));
        return;
    }
    if (!m_popup) {
        m_popup = new StackPopup(nullptr);
        // 弹窗里删除/重命名后磁盘已变，模型与标题计数都要跟着刷新
        connect(m_popup, &StackPopup::filesMutated, this, [this]
        {
            m_model->refresh();
            updateCount();
            persist();
        });
    }
    // 锚点是本窗口的全局矩形，弹窗据此决定往哪一侧展开、避免出屏
    const QRect anchor(mapToGlobal(QPoint(0, 0)), size());
    m_popup->showNear(anchor, m_cfg.title, m_cfg.folder, m_cfg.effectiveStackGrid(),
                      m_cfg.effectiveIconSize(), AppSettings::get().nameLines,
                      m_cfg.effectiveSortMode());
}

// 落盘：把「活」在控件里的状态回收进 m_cfg，再整包丢给宿主（App 负责存储）。
// 这里不直接写文件，只 emit stateChanged —— 存储格式与时机是 App 的职责。
void FileBoxWindow::persist()
{
    // 组件的私有状态（音量、城市…）由面板自己序列化
    if (m_cfg.isWidget() && m_panel) {
        QJsonObject state = m_cfg.widgetState;
        m_panel->saveState(state);   // 传副本进去，面板只增改自己认识的键，不覆盖其它键
        m_cfg.widgetState = state;
    }

    // 只有宿主成员决定窗口几何，其它成员保留自己的位置，解组合后能回到原处
    if (m_cfg.id == m_hostId) {
        if (m_cfg.collapsed) {
            // 折叠态的真实尺寸是标题条/胶囊，不能存进去，否则展开后就恢复成一条。
            // 位置用当前 pos，尺寸用折叠前记下的展开尺寸。
            m_cfg.geometry = QRect(pos(), QSize(qMax(50, m_expandedWidth), m_expandedHeight));
        } else {
            m_expandedHeight = height();   // 顺手把最新展开尺寸记进内存，供下次折叠还原
            m_expandedWidth = width();
            m_cfg.geometry = geometry();
        }
    }

    // 文件盒的视图/排序/图标尺寸以控件当前值为准（用户可能用滚轮或拖动改过）
    if (!m_cfg.isWidget()) {
        m_cfg.manualOrder = m_model->currentOrder();
        if (m_cfg.iconSize > 0)   // 0 表示跟随全局，不能被控件算出的具体值覆盖
            m_cfg.iconSize = m_view->iconSizeValue();
        // 堆叠(2) 不是列表控件的属性，控件只会回报 0/1，写回去会把堆叠模式抹掉
        if (m_cfg.viewMode >= 0 && m_cfg.viewMode != 2)
            m_cfg.viewMode = m_view->viewMode();
        if (m_cfg.sortMode >= 0)  // -1 表示跟随全局，保留哨兵值
            m_cfg.sortMode = m_model->sortMode();
    }

    // 同步回组合成员槽位，保证切换成员/重启后拿到的是最新配置
    if (m_cur >= 0 && m_cur < m_members.size())
        m_members[m_cur] = m_cfg;

    emit stateChanged(m_cfg.id);
}

// 每次显示都重刷表面：窗口隐藏期间主题、缩放或 DWM 状态都可能已经变了，蒙版与材质需要重新应用。
void FileBoxWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    applySurface();
}

// 尺寸变化的兜底与形态维持。这里每一次 resize() 之后都立即 return：
// 再往下走会在同一个事件里二次改尺寸，容易和布局互相打架（并触发递归 resize）。
void FileBoxWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);

    // 折叠/展开动画期间窗口每帧都在变尺寸：这时候做任何「纠偏」都会把动画掐断
    // （下面的折叠态分支会把高度按回标题栏高度，动画立刻归位）。
    // 蒙版与落盘也一并跳过，动画结束后 finishCollapseAnimation() 会补一次。
    if (m_animating)
        return;

    // 组件的尺寸上下限：外部 SetWindowPos 或程序内部都可能越过，这里夹一次
    if (m_cfg.isWidget() && m_panel && !m_cfg.collapsed) {
        const QSize mx = m_panel->maximumSizeHint();
        const int headerH = m_cfg.hideHeader ? 0 : BoxHeaderBar::kHeight;
        // 无效上限 / 某一维为 0 都按「不限制」处理（QWIDGETSIZE_MAX 是 Qt 的最大尺寸哨兵）
        const int maxW = (mx.isValid() && mx.width() > 0) ? mx.width() : QWIDGETSIZE_MAX;
        const int maxH = (mx.isValid() && mx.height() > 0) ? mx.height() + headerH
                                                           : QWIDGETSIZE_MAX;
        if (width() > maxW || height() > maxH) {
            resize(qMin(width(), maxW), qMin(height(), maxH));
            return;
        }
        if (width() < minimumWidth() || height() < minimumHeight()) {
            resize(qMax(width(), minimumWidth()), qMax(height(), minimumHeight()));
            return;
        }
    }

    if (m_cfg.collapsed) {
        const bool capsule = m_cfg.effectiveCapsule();
        const int wantH = capsule ? BoxHeaderBar::kCapsuleHeight : BoxHeaderBar::kHeight;
        // 胶囊宽度这里比的是未夹取的 hint；applyGeometryForState 夹取后尺寸不再变化，
        // 就不会再触发 resizeEvent，因此不会形成递归。
        if (capsule && (width() != m_header->capsuleWidthHint() || height() != wantH)) {
            applyGeometryForState();
            return;
        }
        if (!capsule && height() != wantH) {
            setFixedHeight(wantH);
            return;
        }
    }
    // 蒙版是「按当前尺寸」算出来的位图区域，尺寸一变就得重设；
    // 走 DWM 圆角时系统自己跟着窗口形状走，不需要这一步。
    if (m_usingMask)
        applySurface();

    // 缩放后必须落盘：原来只有 moveEvent 会 persist，用户拉完边框重启又回到旧尺寸，
    // 手感上就像「拉了没用」。拖动边框时这里会连续触发，用 250ms 单次定时器合并成一次写盘。
    // 构造期还没 show（isVisible 为 false），此时不写，避免把未定位的几何落到配置里。
    if (isVisible() && !m_cfg.collapsed) {
        if (!m_resizeSaveTimer) {
            m_resizeSaveTimer = new QTimer(this);
            m_resizeSaveTimer->setSingleShot(true);
            m_resizeSaveTimer->setInterval(250);
            connect(m_resizeSaveTimer, &QTimer::timeout, this, [this] { persist(); });
        }
        m_resizeSaveTimer->start();
    }
}

// 缩放热区宽度（物理像素）。无边框窗口没有系统边框，全靠 WM_NCHITTEST 自己判，
// 所以必须自己按 DPI 放大：8 逻辑像素约等于 Windows 标准尺寸边框
// （SM_CXSIZEFRAME + SM_CXPADDEDBORDER）。
int FileBoxWindow::resizeBorderPx() const
{
    return qMax(kResizeBorder, qRound(kResizeBorder * devicePixelRatioF()));
}

// 网格吸附：四舍五入到最近的 gridStep 倍数（不是向下取整，否则窗口会整体偏左上）。
QPoint FileBoxWindow::snappedPos(const QPoint &p) const
{
    const AppSettings &st = AppSettings::get();
    if (!st.snapToGrid)
        return p;
    const int step = qMax(1, st.gridStep);   // 防 0 除，配置异常时退化成 1px 网格
    return QPoint(qRound(double(p.x()) / step) * step,
                  qRound(double(p.y()) / step) * step);
}

// 程序性/系统性移动（多屏恢复、App 摆放）也要落盘；拖动中的移动不写：
// 拖一次会触发成百上千次 moveEvent，每次都 persist 会不断重置 App 的保存防抖定时器。
void FileBoxWindow::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);
    if (!m_dragging)
        persist();
}

// 全部自绘：底色 + 1px 描边 + 空盒子提示。材质只影响底色浓淡，真正的模糊/背板由平台层负责。
//
// **圆角一律交给窗口本身**：Win11 走 DWM（DWMWA_WINDOW_CORNER_PREFERENCE），旧系统/关掉系统圆角时
// 走 applyRoundedMask() 的窗口蒙版。这里只画**直角**矩形——以前这里还画一层 12px 的自绘圆角，
// 和窗口圆角半径对不上，四角会出现「双层圆角 + 一圈灰缝」，看起来像里面又套了一个框。
void FileBoxWindow::paintEvent(QPaintEvent *)
{
    const AppSettings &st = AppSettings::get();

    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    // 内缩半像素：否则 1px 描边会骑在边界上被抗锯齿糊成 2px 的灰边
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    // 材质决定底色浓淡：云母靠 DWM 背板，底色要更薄
    // 纯色没有背板，需要更实的底色才看得出「盒子」形状
    double scale = 1.0;
    const auto material = static_cast<Platform::Material>(st.material);
    if (material == Platform::Material::Solid)
        scale = 1.12;
    else if (material == Platform::Material::Mica)
        scale = 0.5;

    // 透明度是 0-100 的百分比，映射到 0-235 而不是 0-255：拉满时也保留一点透视感
    const int alpha = qBound(0, qRound(m_cfg.effectiveOpacity() / 100.0 * 235.0 * scale), 255);
    QColor tint = st.isDark() ? QColor(22, 23, 29, alpha) : QColor(250, 251, 254, alpha);
    p.fillRect(r, tint);

    p.setPen(QPen(st.borderColor(), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);

    // 空态提示只画在文件盒的列表/图标模式下：堆叠模式自身有卡片空态，组件盒有自己的版式
    if (!m_cfg.isWidget() && !m_cfg.collapsed && m_cfg.effectiveViewMode() != 2 && m_model
        && m_model->itemCount() == 0) {
        // 用视图的几何而不是整窗，避免文字压到标题栏上
        const QRect body(m_view->mapTo(this, QPoint(0, 0)), m_view->size());
        p.setFont(st.uiFont(12));
        p.setPen(st.subTextColor());
        // 区分「目录还在只是没文件」和「目录被删/拔盘」，后者提示目录已不可用
        const QString hint = QFileInfo(m_cfg.folder).exists()
                                 ? QStringLiteral("空文件夹 · 把文件拖进来")
                                 : QStringLiteral("文件夹不可用");
        p.drawText(body.adjusted(12, 0, -12, 0), Qt::AlignCenter | Qt::TextWordWrap, hint);
    }
}

// 键盘兜底：buildUi 里的 QShortcut 已覆盖常规情况，这里再按 QKeySequence 标准键判一次，
// 给「焦点在窗口自身、快捷键未生效」的场合留一条退路。
void FileBoxWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->matches(QKeySequence::NextChild)) { // Ctrl+Tab
        switchMember(1);
        e->accept();
        return;
    }
    if (e->matches(QKeySequence::PreviousChild)) { // Ctrl+Shift+Tab
        switchMember(-1);
        e->accept();
        return;
    }
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
        if (e->key() == Qt::Key_PageDown) {
            switchMember(1);
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_PageUp) {
            switchMember(-1);
            e->accept();
            return;
        }
    }
    QWidget::keyPressEvent(e);
}

// 拖入：只声明「可以接收 URL 列表」，并固定为复制动作；真正的动作在 dropEvent 里按 Shift 再定。
void FileBoxWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
        return;
    }
    QWidget::dragEnterEvent(e);
}

// 放下：过滤掉非本地文件（http/虚拟文件没有可搬运的路径），默认复制，按住 Shift 改为移动。
// mkpath 是防御性的 —— 托管目录可能被用户手动删掉。
void FileBoxWindow::dropEvent(QDropEvent *e)
{
    QStringList paths;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    if (paths.isEmpty()) {
        e->ignore();
        return;
    }
    QDir().mkpath(m_cfg.folder);
    const bool move = e->modifiers().testFlag(Qt::ShiftModifier);
    if (move)
        ShellActions::moveInto(paths, m_cfg.folder);
    else
        ShellActions::copyInto(paths, m_cfg.folder);
    m_model->refresh();
    // 回告拖放源实际采用的动作，避免源端误判为「已移动」而删掉原文件
    e->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
    e->accept();
    persist();
}

// 无边框窗口没有系统边框，WM_NCHITTEST 必须自己算：否则鼠标落在边缘也只会得到 HTCLIENT，
// 永远拖不出缩放光标。这里只在「可缩放状态」下手工给出 8 个方向的命中结果。
bool FileBoxWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    // 只处理 Windows 原生消息；其它平台（eventType 为 "windows_dispatcher_MSG" 等）交回 Qt
    if (eventType == QByteArrayLiteral("windows_generic_MSG")) {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message == WM_NCHITTEST) {
            // 锁定大小 / 胶囊：边缘必须是 HTCLIENT。
            // 这里不能 return false —— 那样会落到 DefWindowProc，
            // 对带 WS_THICKFRAME 的窗口它照样返回 HTLEFT/HTBOTTOMRIGHT，锁定就失效了。
            if (m_cfg.lockSize || (m_cfg.collapsed && m_cfg.effectiveCapsule())) {
                *result = HTCLIENT;
                return true;
            }

            // lParam 里打包的是屏幕坐标，用 short 截断才能正确还原负坐标（副屏在主屏左侧时会出现）
            POINT pt;
            pt.x = static_cast<short>(LOWORD(msg->lParam));
            pt.y = static_cast<short>(HIWORD(msg->lParam));
            RECT wr;
            GetWindowRect(reinterpret_cast<HWND>(winId()), &wr);

            // b = 边热区，c = 角热区。角是 b 的两倍：斜拉要靠「同时贴住两个方向」命中，
            // 只给 b×b 的小方块时基本点不中（原来 6px 就是这样，用户反馈「斜拉伸不好拉」）。
            const int b = resizeBorderPx();
            const int c = b * 2;

            const bool nearLeft   = pt.x >= wr.left && pt.x < wr.left + c;
            const bool nearRight  = pt.x <= wr.right && pt.x > wr.right - c;
            const bool nearTop    = pt.y >= wr.top && pt.y < wr.top + c;
            const bool nearBottom = pt.y <= wr.bottom && pt.y > wr.bottom - c;

            // 边热区窄一些：整条边都用 2b 会让「贴着边点内容」误触成缩放
            const bool leftEdge   = pt.x >= wr.left && pt.x < wr.left + b;
            const bool rightEdge  = pt.x <= wr.right && pt.x > wr.right - b;
            const bool topEdge    = pt.y >= wr.top && pt.y < wr.top + b;
            const bool bottomEdge = pt.y <= wr.bottom && pt.y > wr.bottom - b;

            // 折叠成标题条时高度是固定的：上下边不提供缩放，只保留左右拖动宽度
            if (m_cfg.collapsed && (nearTop || nearBottom))
                return false;

            // 角优先于边：先判两个方向同时命中的情况，否则四角会退化成单边缩放
            if (nearTop && nearLeft)          *result = HTTOPLEFT;
            else if (nearTop && nearRight)    *result = HTTOPRIGHT;
            else if (nearBottom && nearLeft)  *result = HTBOTTOMLEFT;
            else if (nearBottom && nearRight) *result = HTBOTTOMRIGHT;
            else if (leftEdge)                *result = HTLEFT;
            else if (rightEdge)               *result = HTRIGHT;
            else if (topEdge)                 *result = HTTOP;
            else if (bottomEdge)              *result = HTBOTTOM;
            else
                return false;   // 不是边框：交回默认处理，让窗口内容正常接收鼠标
            return true;
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

// ---------------------------------------------------------------- 菜单

// 三个视图项用 checkPrefix 造勾选效果（QAction 不加 checkable，避免出现单选圆点/勾选框样式）。
void FileBoxWindow::addViewMenuActions(QMenu *menu)
{
    const int vm = m_cfg.effectiveViewMode();
    menu->addAction(checkPrefix(vm == 0) + QStringLiteral("图标"), this, [this] { setViewMode(0); });
    menu->addAction(checkPrefix(vm == 1) + QStringLiteral("列表"), this, [this] { setViewMode(1); });
    menu->addAction(checkPrefix(vm == 2) + QStringLiteral("堆叠（点击弹出展开）"), this,
                    [this] { setViewMode(2); });
}

// 排序项的顺序必须与 AppSettings::SortMode / FileItemModel 的枚举一致（0 手动…4 大小），
// 否则勾选位置会和实际排序对不上。
void FileBoxWindow::addSortMenuActions(QMenu *menu)
{
    const int sm = m_model->sortMode();
    const QStringList names = { QStringLiteral("手动（拖动排序）"), QStringLiteral("名称"),
                                QStringLiteral("修改时间"), QStringLiteral("类型"),
                                QStringLiteral("大小") };
    for (int i = 0; i < names.size(); ++i)
        menu->addAction(checkPrefix(sm == i) + names.at(i), this, [this, i] { setSortMode(i); });
}

// 图标尺寸档位：0 是「跟随全局」的哨兵值，其余为实际像素。
// 判定选中用「近似相等（±6px）」而不是严格相等：生效值可能来自全局设置，和档位值不完全一致。
void FileBoxWindow::addSizeMenuActions(QMenu *menu)
{
    const int cur = m_cfg.effectiveIconSize();
    const QList<QPair<QString, int>> sizes = {
        { QStringLiteral("跟随全局"), 0 }, { QStringLiteral("很小"), 32 },
        { QStringLiteral("小"), 44 },     { QStringLiteral("中"), 68 },
        { QStringLiteral("大"), 96 },     { QStringLiteral("特大"), 128 }
    };
    for (const auto &s : sizes) {
        // 「跟随全局」看配置里的原始值是否 0；具体档位则要求已经脱离全局、且数值接近该档
        const bool on = (s.second == 0) ? (m_cfg.iconSize == 0)
                                        : (m_cfg.iconSize > 0 && qAbs(cur - s.second) < 6);
        menu->addAction(checkPrefix(on) + s.first, this, [this, v = s.second] { setIconSize(v); });
    }
}

// 标题栏菜单（右键标题栏 或 点「⋯」）。第一项「重命名」共用，之后文件盒与组件盒分叉，
// 尾部统一是「新建 / 折叠 / 置顶 / 锁定 / 设置 / 删除」这些窗口级操作。
void FileBoxWindow::buildHeaderMenu(QMenu *menu)
{
    menu->addAction(m_cfg.isWidget() ? QStringLiteral("重命名组件") : QStringLiteral("重命名整理盒"),
                    this, &FileBoxWindow::beginRenameBox);

    if (m_cfg.isWidget()) {
        buildWidgetMenu(menu);
    } else {
        menu->addAction(QStringLiteral("更换文件夹…"), this, [this]
        {
            const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择文件夹"),
                                                                  m_cfg.folder);
            if (!dir.isEmpty())
                setBoxFolder(dir, false);   // 用户自己选的目录不算「托管」，删除盒子时不会被清理
        });
        menu->addAction(QStringLiteral("在资源管理器中打开"), this, [this]
        {
            QDir().mkpath(m_cfg.folder);   // 托管目录可能已被外部删除，先补建再打开
            ShellActions::openFolder(m_cfg.folder);
        });

        // 三个子菜单都要单独 styleMenu：QSS 不继承到弹出的子菜单
        QMenu *viewMenu = menu->addMenu(QStringLiteral("查看方式"));
        styleMenu(viewMenu);
        addViewMenuActions(viewMenu);

        QMenu *sortMenu = menu->addMenu(QStringLiteral("排序方式"));
        styleMenu(sortMenu);
        addSortMenuActions(sortMenu);

        QMenu *sizeMenu = menu->addMenu(QStringLiteral("图标大小"));
        styleMenu(sizeMenu);
        addSizeMenuActions(sizeMenu);
    }

    menu->addSeparator();
    buildGroupMenu(menu);

    menu->addAction(QStringLiteral("新建整理盒"), this, [this] { emit newBoxRequested(); });
    // 「新建组件」不在这里：组件 / 插件的添加入口统一在设置面板（组件多了菜单放不下）

    menu->addSeparator();
    // 头部已隐藏时「折叠」没有意义（折叠后再没有入口能展开），所以整项不出现
    if (!m_cfg.hideHeader)
        menu->addAction(m_cfg.collapsed ? QStringLiteral("展开") : QStringLiteral("折叠"), this,
                        [this] { toggleCollapsed(); });
    QAction *top = menu->addAction(QStringLiteral("始终显示在最前"));
    top->setCheckable(true);
    top->setChecked(m_cfg.alwaysOnTop);
    // 切换置顶会改窗口标志（重建原生窗口），因此走 applyAppearance 而不是只改配置
    connect(top, &QAction::toggled, this, [this](bool on)
    {
        m_cfg.alwaysOnTop = on;
        applyAppearance();
        persist();
    });

    QAction *lock = menu->addAction(QStringLiteral("锁定大小"));
    lock->setCheckable(true);
    lock->setChecked(m_cfg.lockSize);
    lock->setToolTip(QStringLiteral("锁定后拖边缘不再改变宽高（移动、折叠不受影响）；勾掉即可恢复"));
    // 勾选时立即 setFixedSize 钉住当前尺寸；取消勾选时同一函数会把 min/max 还原
    connect(lock, &QAction::toggled, this, [this](bool on)
    {
        m_cfg.lockSize = on;
        applyGeometryForState();
        persist();
    });

    menu->addSeparator();
    menu->addAction(QStringLiteral("整理盒设置…"), this, [this] { emit settingsRequested(); });
    menu->addAction(QStringLiteral("删除…"), this, &FileBoxWindow::deleteBox);
}

// 组件专属项：隐藏标题栏（勾选态即当前状态）、刷新、面板自定义项（天气选城市、音乐选播放器…）。
void FileBoxWindow::buildWidgetMenu(QMenu *menu)
{
    QAction *hide = menu->addAction(m_cfg.hideHeader
                                        ? QStringLiteral("显示标题栏")
                                        : QStringLiteral("隐藏标题栏"));
    hide->setCheckable(true);
    hide->setChecked(m_cfg.hideHeader);
    hide->setToolTip(QStringLiteral("隐藏后内容铺满整窗；在空白处直接拖动即可移动，"
                                    "右键出菜单（按住 Alt 可在任意位置拖动）"));
    connect(hide, &QAction::toggled, this, [this](bool on) { setHeaderHidden(on); });

    menu->addAction(QStringLiteral("刷新"), this, [this] { refresh(); });
    if (m_panel) {
        menu->addSeparator();
        m_panel->buildMenu(menu);   // 面板自己往里加条目，窗口不关心具体内容
    }
}

// 组合菜单：切换上一个/下一个、把别的盒子加进来、把自己组合里的非宿主成员移出去。
// 候选列表每次都向 App 现取（m_boxListProvider），所以别处新建/删除盒子后菜单会立刻反映出来。
void FileBoxWindow::buildGroupMenu(QMenu *menu)
{
    const QVector<BoxConfig> all = m_boxListProvider ? m_boxListProvider() : QVector<BoxConfig>();
    QSet<QString> memberIds;
    for (const BoxConfig &m : m_members)
        memberIds.insert(m.id);

    QMenu *group = menu->addMenu(QStringLiteral("组合"));
    styleMenu(group);

    // 只有一个成员时不存在「切换」的概念，整段隐藏，菜单更短
    if (m_members.size() > 1) {
        group->addAction(QStringLiteral("下一个成员（Ctrl+Tab）"), this, [this] { switchMember(1); });
        group->addAction(QStringLiteral("上一个成员（Ctrl+Shift+Tab）"), this, [this] { switchMember(-1); });
        group->addSeparator();
    }

    QMenu *addMenu = group->addMenu(QStringLiteral("加入组合"));
    styleMenu(addMenu);
    int candidates = 0;
    for (const BoxConfig &b : all) {
        // 排掉自己（宿主）和已在组合里的成员，避免重复加入
        if (b.id == m_hostId || memberIds.contains(b.id))
            continue;
        // 没起过名的盒子用注册表默认名兜底，否则菜单里会出现空条目
        const QString label = b.title.isEmpty()
                                  ? WidgetRegistry::displayNameFor(b.kind, b.pluginId)
                                  : b.title;
        // 只发请求，真正的成员关系由 App 改（它要处理「一个盒子只能属于一个组合」）
        addMenu->addAction(label, this, [this, id = b.id] { emit groupChangeRequested(id, true); });
        ++candidates;
    }
    // 没有任何候选时给一条禁用项，用户能看出是「没有」而不是菜单坏了
    if (candidates == 0) {
        addMenu->addAction(QStringLiteral("没有可加入的盒子"), [] {})->setEnabled(false);
    }

    // 宿主自己不能从自己的组合里移出，所以这个子菜单只列非宿主成员
    if (m_members.size() > 1) {
        QMenu *removeMenu = group->addMenu(QStringLiteral("移出成员"));
        styleMenu(removeMenu);
        for (const BoxConfig &m : m_members) {
            if (m.id == m_hostId)
                continue;
            const QString label = m.title.isEmpty()
                                      ? WidgetRegistry::displayNameFor(m.kind, m.pluginId)
                                      : m.title;
            removeMenu->addAction(label, this, [this, id = m.id]
            {
                emit groupChangeRequested(id, false);
            });
        }
    }
}

// 空白处菜单：新建文件夹（建完直接进入重命名）、剪贴板粘贴、全选，然后是与标题栏一致的视图/排序/尺寸项。
void FileBoxWindow::buildEmptyMenu(QMenu *menu)
{
    // 新建后立刻选中并进入重命名，省掉用户再点一次；created 由 ShellActions 回填
    menu->addAction(QStringLiteral("新建文件夹"), this, [this]
    {
        QString created;
        ShellActions::createFolder(m_cfg.folder, QStringLiteral("新建文件夹"), &created);
        m_model->refresh();
        if (!created.isEmpty()) {
            const QModelIndex idx = m_model->indexOfPath(created);
            if (idx.isValid()) {
                m_view->setCurrentIndex(idx);
                m_view->beginRename(idx);
            }
        }
        persist();
    });

    // 只有剪贴板里确实有路径才允许粘贴；内容在菜单构建时取一次快照即可
    const QStringList clip = ShellActions::clipboardPaths();
    QAction *paste = menu->addAction(QStringLiteral("粘贴"), this, &FileBoxWindow::pasteIntoBox);
    paste->setEnabled(!clip.isEmpty());
    menu->addAction(QStringLiteral("全选"), this, [this] { m_view->selectAll(); });
    menu->addSeparator();

    QMenu *viewMenu = menu->addMenu(QStringLiteral("查看方式"));
    styleMenu(viewMenu);
    addViewMenuActions(viewMenu);

    QMenu *sortMenu = menu->addMenu(QStringLiteral("排序方式"));
    styleMenu(sortMenu);
    addSortMenuActions(sortMenu);

    QMenu *sizeMenu = menu->addMenu(QStringLiteral("图标大小"));
    styleMenu(sizeMenu);
    addSizeMenuActions(sizeMenu);

    menu->addSeparator();
    menu->addAction(QStringLiteral("刷新"), this, [this] { refresh(); });
    menu->addAction(QStringLiteral("打开文件夹"), this, [this]
    {
        QDir().mkpath(m_cfg.folder);
        ShellActions::openFolder(m_cfg.folder);
    });
    menu->addAction(QStringLiteral("整理盒设置…"), this, [this] { emit settingsRequested(); });
    menu->addAction(QStringLiteral("删除整理盒…"), this, &FileBoxWindow::deleteBox);
}

// 选中项菜单。文案与可用性跟着「选中数量」走：多选时显示「打开 N 个项目」「叠成一堆」。
// 「打开」按视图的 currentIndex 取路径：在未选中条目上右键时 FileListView 会先把它设为当前项，
// 因此与用户点击的条目一致；多选时则打开当前项而非全部选中项。
void FileBoxWindow::buildItemMenu(QMenu *menu, const QString &path)
{
    const QStringList sel = m_view->selectedPaths();
    const int count = int(sel.size());

    menu->addAction(count > 1 ? QStringLiteral("打开 %1 个项目").arg(count) : QStringLiteral("打开"),
                    this, [this]
    {
        onOpenRequested(m_view->currentIndex().data(FileItemModel::PathRole).toString());
    });
    menu->addAction(QStringLiteral("在资源管理器中显示"), this, [path]
    {
        ShellActions::revealInExplorer(path);
    });
    menu->addSeparator();

    // 剪切/复制按值捕获 sel 快照：动作执行时不再回头读视图的当前选择
    menu->addAction(QStringLiteral("剪切"), this, [sel] { ShellActions::setClipboardPaths(sel, true); });
    menu->addAction(QStringLiteral("复制"), this, [sel] { ShellActions::setClipboardPaths(sel, false); });
    menu->addAction(QStringLiteral("重命名"), this, [this] { m_view->beginRenameOnCurrent(); });

    menu->addSeparator();
    // 堆叠是一个「把多个条目收进子目录」的操作，没有选中项时禁用
    QAction *pile = menu->addAction(count > 1 ? QStringLiteral("把选中的 %1 项叠成一堆…").arg(count)
                                              : QStringLiteral("叠成一堆…"),
                                    this, [this, sel] { makePile(sel); });
    pile->setEnabled(count > 0);
    // 只有命中项本身是 .pile 目录时才提供「拆开」，普通文件夹不显示
    const QFileInfo info(path);
    if (info.isDir() && info.fileName().endsWith(QStringLiteral(".pile"), Qt::CaseInsensitive)) {
        menu->addAction(QStringLiteral("拆开这一堆"), this, [this, path] { unpackPile(path); });
    }
    menu->addSeparator();

    // 删除走回收站而不是直接删；确认开关跟全局设置，失败项汇总后一次性告知
    menu->addAction(QStringLiteral("删除（移到回收站）"), this, [this, sel]
    {
        bool ok = true;
        if (AppSettings::get().confirmDelete) {
            ok = QMessageBox::question(this, QStringLiteral("删除"),
                                       QStringLiteral("将 %1 个项目移到回收站？").arg(sel.size()),
                                       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
                 == QMessageBox::Yes;
        }
        if (!ok)
            return;
        // failed 里是没能删除的路径（被占用、无权限等），部分失败不影响其它项
        QStringList failed;
        if (!ShellActions::moveToRecycleBin(sel, &failed)) {
            QMessageBox::warning(this, QStringLiteral("删除失败"),
                                 QStringLiteral("有 %1 个项目无法删除：\n%2")
                                     .arg(failed.size())
                                     .arg(failed.join(QLatin1Char('\n'))));
        }
        m_model->refresh();
        persist();
    });
    menu->addSeparator();
    // 在「有选中项」的菜单里也放一个新建文件夹，省得用户先取消选择再右键空白
    menu->addAction(QStringLiteral("新建文件夹"), this, [this]
    {
        QString created;
        ShellActions::createFolder(m_cfg.folder, QStringLiteral("新建文件夹"), &created);
        m_model->refresh();
        if (!created.isEmpty()) {
            const QModelIndex idx = m_model->indexOfPath(created);
            if (idx.isValid()) {
                m_view->setCurrentIndex(idx);
                m_view->beginRename(idx);
            }
        }
        persist();
    });
}

// 两个菜单入口只负责「建菜单 + 套样式 + 填内容 + 弹出」，菜单对象是栈上的，exec 返回即销毁。
void FileBoxWindow::onItemContextMenu(const QString &path, const QPoint &globalPos)
{
    QMenu menu;
    styleMenu(&menu);
    buildItemMenu(&menu, path);
    menu.exec(globalPos);
}

// 空白处右键：内容更全（含视图/排序/图标大小与删除整理盒），作用于整个盒子而不是某个条目。
void FileBoxWindow::onEmptyContextMenu(const QPoint &globalPos)
{
    QMenu menu;
    styleMenu(&menu);
    buildEmptyMenu(&menu);
    menu.exec(globalPos);
}

// 打开：.pile 目录视为「一堆文件」，用堆叠窗口展开；普通目录/文件交给系统默认程序。
// 用 fileName 而不是完整路径判断后缀，避免父目录名里带 ".pile" 时误判。
void FileBoxWindow::onOpenRequested(const QString &path)
{
    if (path.isEmpty())
        return;

    const QFileInfo fi(path);
    if (fi.isDir() && fi.fileName().endsWith(QStringLiteral(".pile"), Qt::CaseInsensitive)) {
        // completeBaseName 去掉的正是 ".pile" 这一段，正好当弹窗标题
        openPile(path, fi.completeBaseName());
        return;
    }
    if (fi.isDir())
        ShellActions::openFolder(path);
    else
        ShellActions::open(path);
}

// 展开某一堆：与 openStackPopup 共用同一个弹窗实例，只是数据源换成堆目录而不是当前盒子目录。
void FileBoxWindow::openPile(const QString &folder, const QString &title)
{
    if (!m_popup) {
        m_popup = new StackPopup(nullptr);
        connect(m_popup, &StackPopup::filesMutated, this, [this]
        {
            m_model->refresh();
            updateCount();
            persist();
        });
    }
    const QRect anchor(mapToGlobal(QPoint(0, 0)), size());
    m_popup->showNear(anchor, title, folder, m_cfg.effectiveStackGrid(),
                      m_cfg.effectiveIconSize(), AppSettings::get().nameLines,
                      m_cfg.effectiveSortMode());
}

// 叠成一堆：本质是在盒子目录下建一个 "<名字>.pile" 子目录，把选中项移进去。
// 名字清洗（非法字符替换、去掉用户重复输入的后缀、重名自动加序号）都在这里做，
// 因为目录名要直接落磁盘，Win32 不允许 \/:*?"<>| 这些字符。
void FileBoxWindow::makePile(const QStringList &paths)
{
    if (paths.isEmpty())
        return;

    bool ok = false;
    const QString input = QInputDialog::getText(this, QStringLiteral("叠成一堆"),
                                                QStringLiteral("这一堆叫什么名字？"),
                                                QLineEdit::Normal,
                                                QStringLiteral("新堆"), &ok);
    if (!ok)
        return;   // 用户取消

    QString base = input.trimmed();
    if (base.isEmpty())
        base = QStringLiteral("新堆");   // 只输空格也当成没输
    for (QChar &c : base) {
        if (QStringLiteral("\\/:*?\"<>|").contains(c))
            c = QLatin1Char('_');
    }
    // 用户可能顺手打了 ".pile"，先剥掉，避免出现 "x.pile.pile"
    if (base.endsWith(QStringLiteral(".pile"), Qt::CaseInsensitive))
        base.chop(5);

    QDir().mkpath(m_cfg.folder);
    QString pilePath = m_cfg.folder + QLatin1Char('/') + base + QStringLiteral(".pile");
    // 重名时依次尝试 "名字 (2).pile"、"名字 (3).pile"…（n 从 2 起，第一个重名就是「第 2 堆」）
    int n = 2;
    while (QFileInfo::exists(pilePath))
        pilePath = m_cfg.folder + QLatin1Char('/') + base + QStringLiteral(" (%1).pile").arg(n++);

    if (!QDir().mkpath(pilePath)) {
        QMessageBox::warning(this, QStringLiteral("叠成一堆"),
                             QStringLiteral("无法创建堆：%1").arg(pilePath));
        return;
    }

    // 移动可能部分失败（文件被占用），失败清单单独提示，成功的已经进堆
    const QStringList failed = ShellActions::moveInto(paths, pilePath);
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("叠成一堆"),
                             QStringLiteral("有 %1 个文件没能移入：\n%2")
                                 .arg(failed.size())
                                 .arg(failed.join(QLatin1Char('\n'))));
    }
    m_model->refresh();
    updateCount();
    persist();
}

// 拆堆：把堆目录里的内容移回盒子根目录，全部成功后再删掉空目录。
// 只要有任意一项没移出去就保留目录（否则剩下没移走的文件会被一起删掉）。
void FileBoxWindow::unpackPile(const QString &pileFolder)
{
    const QFileInfo fi(pileFolder);
    if (!fi.isDir())
        return;

    // AllEntries + Hidden + System：堆里可能混入隐藏文件（如 desktop.ini），漏掉会导致目录删不掉
    QStringList children;
    const QFileInfoList list = QDir(pileFolder).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);
    for (const QFileInfo &c : list)
        children << c.absoluteFilePath();

    if (!children.isEmpty()) {
        const QStringList failed = ShellActions::moveInto(children, m_cfg.folder);
        if (!failed.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("拆开这一堆"),
                                 QStringLiteral("有 %1 个项目没能移出：\n%2")
                                     .arg(failed.size())
                                     .arg(failed.join(QLatin1Char('\n'))));
            m_model->refresh();
            return;   // 保留堆目录，用户可重试
        }
    }
    QDir(pileFolder).removeRecursively();
    m_model->refresh();
    updateCount();
    persist();
}

// 排序方式变了：正在显示的堆叠弹窗要重新打开一次，否则它内部仍用旧的排序。
void FileBoxWindow::setSortMode(int mode)
{
    m_cfg.sortMode = mode;
    m_model->setSortMode(mode);
    if (m_popup && m_popup->isVisible())
        openStackPopup();   // showNear 会用新排序重新设置弹窗模型并刷新
    persist();
}

// 视图方式：堆叠(2) 在列表控件里按图标样式排版（卡片只是另一种呈现），内容区切换交给 updateBody。
void FileBoxWindow::setViewMode(int mode)
{
    m_cfg.viewMode = mode;
    m_view->applyAppearance(mode == 1 ? 1 : 0, m_cfg.effectiveIconSize(),
                            AppSettings::get().nameLines);
    updateBody();
    persist();
}

// 折叠形态只在「当前已折叠」时才有立即可见的效果；展开态只改配置，等下次折叠生效。
void FileBoxWindow::setCapsuleMode(int mode)
{
    m_cfg.capsule = mode;
    if (m_cfg.collapsed) {
        m_header->setCapsuleMode(mode > 0);
        applyGeometryForState();   // 胶囊/标题条的尺寸完全不同，必须重算
        applySurface();            // 圆角半径依赖高度
    }
    persist();
}

// 图标尺寸同时影响列表视图与堆叠卡封面。
void FileBoxWindow::setIconSize(int size)
{
    m_cfg.iconSize = size;
    m_view->applyAppearance(m_cfg.effectiveViewMode() == 1 ? 1 : 0, m_cfg.effectiveIconSize(),
                            AppSettings::get().nameLines);
    m_stackCard->setIconSize(m_cfg.effectiveIconSize());
    persist();
}

// 换目录：统一存成 '/' 分隔（JSON 里跨平台可读），并同步模型与头部图标（文件夹图标可能带自定义图案）。
// managed 由调用方决定：程序自动创建的目录为 true，用户自选的目录为 false。
void FileBoxWindow::setBoxFolder(const QString &folder, bool managed)
{
    m_cfg.folder = QDir::fromNativeSeparators(folder);
    m_cfg.managed = managed;
    m_model->setFolder(m_cfg.folder);
    m_header->setFolderIcon(QFileIconProvider().icon(QFileInfo(m_cfg.folder)));
    persist();
    update();
}

// 粘贴：剪贴板是「剪切」时移动，否则复制。文件操作交给 ShellActions，失败不在这里提示。
void FileBoxWindow::pasteIntoBox()
{
    const QStringList paths = ShellActions::clipboardPaths();
    if (paths.isEmpty())
        return;
    if (ShellActions::clipboardIsCut())
        ShellActions::moveInto(paths, m_cfg.folder);
    else
        ShellActions::copyInto(paths, m_cfg.folder);
    m_model->refresh();
    persist();
}

// 删除盒子：自建 QMessageBox（而不是静态 question()）是为了拿到「删除整理盒」这个破坏性按钮的
// 点击结果 —— 只有点它才算确认，点「取消」或直接关窗都按取消处理。
void FileBoxWindow::deleteBox()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("删除整理盒"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("确定删除整理盒「%1」？").arg(m_cfg.title));
    // 托管目录只是本程序创建并管理的，删除盒子不等于删文件，必须写清楚避免用户误会数据会一起消失
    box.setInformativeText(m_cfg.managed
                               ? QStringLiteral("该整理盒的文件夹由本程序托管：\n%1\n\n删除盒子后文件夹仍会保留在磁盘上。")
                                     .arg(m_cfg.folder)
                               : QStringLiteral("关联的文件夹不会被删除。"));
    QPushButton *del = box.addButton(QStringLiteral("删除整理盒"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != del)
        return;

    // 关键：不能在这里直接 emit —— 此时 QMessageBox 还是本窗口的子对象（栈上）。
    // emit 后 App 会 deleteLater() 掉本窗口，MessageBox 作为子对象被一起销毁，
    // exec 返回后栈上的 box 再析构一次就是二次释放（Debug CRT 报 HEAP CORRUPTION）。
    // 因此等对话框彻底析构、函数返回之后，再用 0ms 定时器把删除请求发出去。
    // 定时器以 this 作 context：窗口若先被销毁，这个回调会自动失效，不会野指针。
    const QString id = m_cfg.id;
    QTimer::singleShot(0, this, [this, id] { emit removeRequested(id); });
}
