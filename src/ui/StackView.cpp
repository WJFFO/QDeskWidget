/**
 * @file StackView.cpp
 * @brief StackCard（盒内堆叠卡）与 StackPopup（展开网格）的实现。
 *
 * 两处细节容易改错：
 *  1. 堆叠的层数、错位像素都是视觉参数，卡片尺寸要先夹出上下限（88×62 起）再画，
 *     否则盒子很小时后面的层会画到控件外，被裁成半张卡。
 *  2. Popup 的自动关闭依赖 WindowDeactivate，弹出 QMenu 会瞬时夺走激活态，
 *     所以 exec() 前后必须夹 m_suppressHide，并且延时 90ms 再判一次 isActiveWindow()。
 */
#include "ui/StackView.h"

#include "core/AppSettings.h"
#include "core/FileItemModel.h"
#include "core/ShellActions.h"
#include "mcolumnlayout.h"
#include "mrowlayout.h"
#include "platform/Platform.h"
#include "ui/FileListView.h"

#include <QApplication>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QToolButton>

namespace
{
/**
 * 给 QMenu 套上项目主题（圆角、强调色高亮、细分割线）。
 * @param m 待美化的菜单，就地修改；调用方负责 exec/show 与生命周期。
 *
 * 颜色用 HexArgb 输出：#AARRGGBB 才能保住 menuBg()/borderColor() 里的 alpha，
 * 换成默认的 name()（HexRgb）会把透明度丢掉，菜单就变成实心色块。
 */
void styleMenu(QMenu *m)
{
    const AppSettings &st = AppSettings::get();
    m->setFont(st.uiFont(13));
    m->setStyleSheet(QStringLiteral(
                         "QMenu{background:%1;border:1px solid %2;border-radius:9px;padding:6px;}"
                         "QMenu::item{padding:6px 20px 6px 8px;border-radius:6px;color:%3;}"
                         "QMenu::item:selected{background:%4;}"
                         "QMenu::separator{height:1px;background:%2;margin:5px 8px;}")
                         .arg(st.menuBg().name(QColor::HexArgb),
                              st.borderColor().name(QColor::HexArgb),
                              st.textColor().name(),
                              st.accent().name()));
}

/**
 * 画一块圆角玻璃底板（堆叠卡与弹窗共用）。
 * @param p 目标画笔，调用方已设置抗锯齿。
 * @param rect 底板矩形（控件坐标）。
 * @param radius 圆角半径（px）。
 * @param opacityScale 额外的不透明度系数，1.0 = 完全跟随设置。
 *        alpha = 用户设置的不透明度百分比 × 235 × 该系数，留出 20/255 的余量避免完全不透。
 */
void paintGlass(QPainter &p, const QRect &rect, int radius, double opacityScale)
{
    const AppSettings &st = AppSettings::get();
    const int alpha = qRound(AppSettings::get().opacity / 100.0 * 235.0 * opacityScale);
    QColor tint = st.isDark() ? QColor(24, 25, 30, alpha) : QColor(248, 249, 252, alpha);

    // 内缩 0.5px：1px 描边压在边界上会有一半被裁掉，看起来像缺了一条边
    QPainterPath path;
    path.addRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p.fillPath(path, tint);
    p.setPen(QPen(st.borderColor(), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}
} // namespace

// ======================================================================
// StackCard
// ======================================================================

// 只做静态配置：手型光标 + 透明底（自绘圆角卡片必须露背景）。
StackCard::StackCard(QWidget *parent)
    : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground);
}

// 只存指针不接管所有权，调用方（FileBoxWindow）负责保证模型活得更久。
void StackCard::setModel(FileItemModel *model)
{
    m_model = model;
    update();
}

// 这里只是记录期望值；真正的绘制尺寸还会被卡片高度二次夹取（见 paintEvent）。
void StackCard::setIconSize(int px)
{
    m_iconSize = qBound(24, px, 128);
    update();
}

// 悬停只影响最前一层卡片的描边颜色（paintEvent 里靠 m_hover 切换强调色），
// 不改变尺寸也不请求布局，因此进出都只需 update()。
void StackCard::enterEvent(QEnterEvent *e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void StackCard::leaveEvent(QEvent *e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

// 加 rect().contains() 判断：按下后拖到卡片外再松手不应算点击。
void StackCard::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()))
        emit clicked();
    QWidget::mouseReleaseEvent(e);
}

// 卡片堆的绘制流程：空态提示 → 后面几层错位卡片 → 最前面一层（悬停换描边色）
// → 首项图标与名称 → 数量角标。所有尺寸随控件大小收缩，而不是等比缩放。
void StackCard::paintEvent(QPaintEvent *)
{
    const AppSettings &st = AppSettings::get();
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                     | QPainter::SmoothPixmapTransform);

    const int count = m_model ? m_model->itemCount() : 0;
    const QRect body = rect().adjusted(10, 8, -10, -8);   // 四周留白：上方还要放得下骑边的数量角标
    if (body.width() < 40 || body.height() < 30)
        return;                                            // 尺寸过小（如盒子被折叠/缩得很窄）时不画，避免画出畸形卡片

    if (count == 0) {
        p.setFont(st.uiFont(12));
        p.setPen(st.subTextColor());
        p.drawText(body, Qt::AlignCenter, QStringLiteral("空 · 拖入文件"));
        return;
    }

    // 最多画 3 层：再多也看不出层次，只会把错位推到控件外
    const int layers = qMin(3, count);
    QSize card(qMin(body.width() - 26, 172), qMin(body.height() - 22, 116));  // 26/22 是给错位 + 角标预留的空间
    card.setWidth(qMax(88, card.width()));    // 下限：保证图标 + 一行名称放得下
    card.setHeight(qMax(62, card.height()));

    const QPoint topLeft(body.center().x() - card.width() / 2,
                         body.center().y() - card.height() / 2);

    // 后面的卡片先画，向右上错开
    for (int i = layers - 1; i >= 1; --i) {
        const QRectF r(topLeft.x() + i * 9.0, topLeft.y() - i * 9.0, card.width(), card.height());
        QPainterPath path;
        path.addRoundedRect(r, 10, 10);
        // 越靠后的层越暗/越淡，模拟纸张叠放；深色主题靠 alpha，浅色主题靠不透明度
        QColor bg = st.isDark() ? QColor(255, 255, 255, 10 + (layers - i) * 6)
                                : QColor(255, 255, 255, 90 + (layers - i) * 20);
        p.fillPath(path, bg);
        p.setPen(QPen(st.borderColor(), 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    // 最前面的卡片
    const QRect front(topLeft, card);
    QPainterPath frontPath;
    frontPath.addRoundedRect(QRectF(front), 10, 10);
    QColor frontBg = st.isDark() ? QColor(255, 255, 255, 26) : QColor(255, 255, 255, 220);
    p.fillPath(frontPath, frontBg);
    p.setPen(QPen(m_hover ? st.accent() : st.borderColor(), m_hover ? 1.5 : 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(frontPath);

    // 首个项目的图标 + 名称
    // front.height() - 34 是给名称行（18px）和上下留白留的余量，再整体夹进 [28,72]
    const QModelIndex first = m_model->index(0, 0);
    const int iconPx = qBound(28, qMin(m_iconSize, front.height() - 34), 72);
    const QRect iconRect(front.center().x() - iconPx / 2, front.top() + 7, iconPx, iconPx);
    const QIcon icon = first.data(FileItemModel::IconRole).value<QIcon>();
    if (!icon.isNull())
        icon.paint(&p, iconRect, Qt::AlignCenter, QIcon::Normal);

    const QFont nameFont = st.uiFont(12);
    const QFontMetrics fm(nameFont);
    p.setFont(nameFont);
    p.setPen(st.textColor());
    // 名称固定在卡片底部 22px 高的条带里，中部省略以保住扩展名
    const QRect nameRect(front.left() + 8, front.bottom() - 22, front.width() - 16, 18);
    p.drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter,
               fm.elidedText(first.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
                             nameRect.width()));

    // 数量角标：只有 1 项时不画（1 这个数字没有信息量），骑在卡片右上角
    if (count > 1) {
        const QString badge = QString::number(count);
        const QFont badgeFont = st.uiFont(10, QFont::DemiBold);
        const QFontMetrics bfm(badgeFont);
        const int w = qMax(18, bfm.horizontalAdvance(badge) + 10);
        const QRect badgeRect(front.right() - w - 2, front.top() - 6, w, 16);
        p.setFont(badgeFont);
        p.setPen(Qt::NoPen);
        p.setBrush(st.accent());
        p.drawRoundedRect(badgeRect, 8, 8);
        p.setPen(st.isDark() ? QColor(16, 18, 24) : QColor(255, 255, 255));
        p.drawText(badgeRect, Qt::AlignCenter, badge);
    }
}

// ======================================================================
// StackPopup
// ======================================================================

// 构造即把整套交互搭好：模型 + 视图 + 标题栏（标题 / 计数 / 关闭）。
// 窗口标志是 Tool + 无边框 + 置顶：不出现在任务栏，也不会被盒子窗口压住。
StackPopup::StackPopup(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
                   | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);      // 没有它 Esc 键收不到，窗口就关不掉
    setWindowTitle(QStringLiteral("展开"));

    m_model = new FileItemModel(this);

    m_view = new FileListView(this);
    // boxId 固定为 "popup"：弹窗内部的拖动命中「同盒排序」分支，
    // 而从整理盒/资源管理器拖进来的则按跨盒处理（移动或复制）。
    m_view->attach(m_model, QStringLiteral("popup"));
    connect(m_view, &FileListView::openRequested, this, [this](const QString &path)
    {
        // 文件夹用资源管理器打开，普通文件用系统默认程序
        if (QFileInfo(path).isDir())
            ShellActions::openFolder(path);
        else
            ShellActions::open(path);
    });
    connect(m_view, &FileListView::contextMenuOnItem, this,
            [this](const QString &path, const QPoint &gp) { buildContextMenu(path, gp); });
    // 空白处右键只给「刷新」：弹窗里的批量操作都在条目菜单上，空白菜单保持极简
    connect(m_view, &FileListView::contextMenuOnEmpty, this, [this](const QPoint &gp)
    {
        QMenu menu;
        styleMenu(&menu);
        menu.addAction(QStringLiteral("刷新"), this, [this] { refresh(); });
        m_suppressHide = true;      // 同上：避开菜单引起的瞬时失活
        menu.exec(gp);
        m_suppressHide = false;
    });
    connect(m_view, &FileListView::contentsMutated, this, [this]
    {
        // 条目数可能变了，窗口尺寸要跟着重算，再通知上层刷新盒子与落盘
        rebuildSize();
        emit filesMutated();
    });

    // 根列铺满弹窗；窗口本身无边框，所以边距一律 0，圆角由 paintEvent + 遮罩负责
    auto *root = new MColumnLayout(this);
    root->ColumnFillHost(this);
    root->SetMargin(0);
    root->SetSpacing(0);

    // 标题栏：左内边距 14 与卡片视觉对齐，右侧 8 是因为关闭按钮自带内缩
    auto *head = new MRowLayout(this);
    head->SetMargin(14, 0, 8, 0);
    head->SetSpacing(6);

    m_title = new QLabel(head);
    m_count = new QLabel(head);
    m_count->setAttribute(Qt::WA_TransparentForMouseEvents);   // 计数只是装饰，别挡住标题栏的空白点击

    auto *close = new QToolButton(head);
    close->setText(QStringLiteral("✕"));
    close->setAutoRaise(true);
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedSize(24, 24);
    close->setFocusPolicy(Qt::NoFocus);        // 不抢焦点，否则 Esc 会先被按钮吃掉
    close->setToolTip(QStringLiteral("关闭"));
    connect(close, &QToolButton::clicked, this, &QWidget::hide);

    // 标题 stretch=1 吃掉剩余宽度，把计数与关闭按钮顶到右边；
    // RowPinToRight 会先清掉行内已有弹簧，所以标题的拉伸不会被 1:1 分走。
    head->RowAddWidget(m_title, 1, Qt::AlignVCenter);
    head->RowAddWidget(m_count, 0, Qt::AlignVCenter);
    head->RowPinToRight(close, Qt::AlignVCenter);
    head->setFixedHeight(40);                  // 与 rebuildSize() 里的 40 是同一个值，改一边要改两边

    root->ColumnAddWidget(head, 0);            // 标题栏高度固定，不参与纵向拉伸
    root->ColumnAddWidget(m_view, 1);          // 网格占满剩余高度
}

StackPopup::~StackPopup() = default;

// 每次弹出都重新走一遍「设参数 → 算尺寸 → 定位」。
// 模型 setFolder 会触发全量重建，所以内容不会残留上次的。
void StackPopup::showNear(const QRect &anchorGlobal, const QString &title, const QString &folder,
                          int gridCols, int iconSize, int nameLines, int sortMode)
{
    const AppSettings &st = AppSettings::get();

    m_gridCols = (gridCols == 5) ? 5 : 3;   // 只支持 3/5 两档，其它值一律回落到 3
    m_iconSize = iconSize;

    m_title->setFont(st.uiFont(13, QFont::DemiBold));
    m_title->setStyleSheet(QStringLiteral("color:%1;").arg(st.textColor().name()));
    m_title->setText(title);
    m_count->setFont(st.uiFont(11));
    m_count->setStyleSheet(QStringLiteral("color:%1;").arg(st.subTextColor().name()));

    m_model->setSortMode(sortMode);
    m_model->setShowHidden(st.showHidden);   // 隐藏文件开关是全局设置，弹窗不做单独覆盖
    m_model->setFolder(folder);
    m_view->applyAppearance(0, iconSize, nameLines);   // 弹窗永远用图标网格，不受盒子主视图模式影响

    rebuildSize();

    // 定位：优先弹在锚点下方 8px；下方放不下就翻到上方，再不行贴屏幕顶边。
    // 屏幕按锚点中心所在的屏取，多显示器下不会跑到主屏上。
    QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    if (QScreen *scr = QGuiApplication::screenAt(anchorGlobal.center()))
        avail = scr->availableGeometry();

    int x = anchorGlobal.left();
    int y = anchorGlobal.bottom() + 8;
    if (y + height() > avail.bottom())
        y = anchorGlobal.top() - height() - 8;
    if (y < avail.top())
        y = avail.top() + 8;
    // 横向夹在屏幕内；右侧边界用 qMax 兜底，防止窗口比屏幕还宽时算出非法区间
    x = qBound(avail.left() + 8, x, qMax(avail.left() + 8, avail.right() - width() - 8));

    move(x, y);
    show();
    raise();                 // Tool 窗口之间有层级竞争，raise 保证压在其它 Tool 之上
    activateWindow();
    setFocus();              // 拿到焦点 Esc 才有效；焦点随后会转移给内部的 FileListView
    // 材质必须在 show 之后应用：平台实现需要已创建的原生窗口句柄
    Platform::applyMaterial(this, static_cast<Platform::Material>(st.material));
    update();
}

// 先读盘再算尺寸：条目数决定行数，行数决定窗口高度。顺序反过来会按旧数据定尺寸。
void StackPopup::refresh()
{
    m_model->refresh();
    rebuildSize();
}

// 尺寸由「列数 × 单元格」直接算出，不依赖视图的 sizeHint：
// 弹窗是固定大小的 Tool 窗口，必须自己给定 setFixedSize，否则会退化成一个很小的空白窗。
void StackPopup::rebuildSize()
{
    const QSize cell = m_view->gridCellSize();
    // qMax(1, ...)：空文件夹也留一格高度，否则窗口会只剩一条标题栏
    const int count = qMax(1, m_model->itemCount());
    const int cols = m_gridCols;
    // 行数夹在 [1, cols]：条目多于 cols² 时窗口不再增高，多出来的靠视图滚动查看
    const int rows = qBound(1, (count + cols - 1) / cols, cols);
    const int margin = 10;

    // cell.height() 已含名称折行所需高度，这里不再额外加行距
    const int w = cols * cell.width() + margin * 2 + 12;      // 宽度比高度多留 12px（经验值，高度侧无对应项），给右侧富余
    const int h = 40 + rows * cell.height() + margin * 2;     // 40 = 标题栏固定高度，与构造里的 setFixedHeight(40) 对齐
    setFixedSize(w, h);

    // 标题每次重建都会再省略一次，累积下来会越来越短——但下一次 showNear() 会重设完整文本，
    // 所以只在同一目录下反复 rebuild 时可见，可接受。
    const QFontMetrics fm(m_title->font());
    const int titleWidth = qMax(60, w - 150);                 // 150 = 计数 + 关闭按钮 + 边距的预估占用
    m_title->setText(fm.elidedText(m_title->text(), Qt::ElideMiddle, titleWidth));
    m_count->setText(QStringLiteral("%1 项").arg(m_model->itemCount()));
}

// 只画底板；圆角与背景遮罩在 resizeEvent 里另外设置，两者必须用同一个半径 12。
void StackPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    paintGlass(p, rect(), 12, 1.0);
}

// 透明窗口不会有原生圆角，必须每次尺寸变化后重设一次遮罩（遮罩尺寸依赖当前大小）。
void StackPopup::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    Platform::applyRoundedMask(this, 12);
}

// 只认 Esc：弹窗是 Tool 窗口，没有标题栏关闭按钮之外的收起方式，
// 其余按键（方向键/Delete/F2）都必须下传，由持有焦点的 FileListView 处理。
void StackPopup::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        hide();          // 只隐藏不销毁：窗口与内部模型由 FileBoxWindow 复用
        return;
    }
    QWidget::keyPressEvent(e);
}

// 自动关闭策略：窗口失活后延时 90ms 再确认一次是否真的不活跃。
// 直接 hide 会误杀「点开右键菜单」「拖拽经过别的窗口」这类瞬时失活场景。
bool StackPopup::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate && !m_suppressHide) {
        // 让菜单 / 拖拽等瞬时激活先结束
        QTimer::singleShot(90, this, [this]
        {
            // 延时期间可能已重新激活（点了本窗口的菜单项），所以这里要再判一次
            if (!m_suppressHide && isVisible() && !isActiveWindow())
                hide();
        });
    }
    return QWidget::event(e);
}

// 菜单在栈上构造 + exec() 阻塞式弹出：出口只有一行，所以 m_suppressHide 的复位
// 紧跟在 exec() 之后（中途 return 都发生在 exec 之前或回调里，不影响这里的配对）。
void StackPopup::buildContextMenu(const QString &path, const QPoint &globalPos)
{
    // 删除动作作用于「当前选中的全部项」，而不是被右键的那一个 path，两者可以不一致
    const QStringList sel = m_view->selectedPaths();
    QMenu menu;
    styleMenu(&menu);

    menu.addAction(QStringLiteral("打开"), this, [path]
    {
        if (QFileInfo(path).isDir())
            ShellActions::openFolder(path);
        else
            ShellActions::open(path);
    });
    menu.addAction(QStringLiteral("在资源管理器中显示"), this, [path]
    {
        ShellActions::revealInExplorer(path);
    });
    menu.addSeparator();
    // 重命名只针对当前项（FileListView 的 currentIndex），不随 path 变化
    menu.addAction(QStringLiteral("重命名"), this, [this] { m_view->beginRenameOnCurrent(); });
    menu.addAction(QStringLiteral("删除（移到回收站）"), this, [this, sel]
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
        ShellActions::moveToRecycleBin(sel);
        // 先 refresh 重算尺寸，再通知盒子：顺序与 contentsMutated 的处理保持一致
        refresh();
        emit filesMutated();
    });

    // 菜单是独立窗口，exec 期间本窗口会收到 WindowDeactivate，不屏蔽就会被自动关掉
    m_suppressHide = true;
    menu.exec(globalPos);
    m_suppressHide = false;
}
