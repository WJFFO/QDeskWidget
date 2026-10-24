/**
 * @file FileListView.cpp
 * @brief FileListView 的实现：拖放分流、内联重命名、快捷键与网格尺寸维护。
 *
 * 三条容易踩的线：
 *  1. 「盒内排序」与「移入文件夹」共用同一个 dropEvent，靠 ontoFolder / sourceBox / Ctrl
 *     三个条件区分，改动判定顺序会直接把排序变成移动文件。
 *  2. 重命名以 m_renamePath 为准而不是索引——重命名期间模型可能已被文件监视器刷新。
 *  3. 所有落盘动作之后都要 refresh() + emit contentsMutated()，否则上层不会保存手动顺序。
 */
#include "ui/FileListView.h"
#include "core/AppSettings.h"
#include "core/FileItemModel.h"
#include "core/ShellActions.h"
#include "ui/FileItemDelegate.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QUrl>
#include <QWheelEvent>
#include <climits>

namespace
{
// 盒内拖拽的私有 MIME 类型：只有本程序认识，资源管理器会忽略它而只看 file:// URL。
const char *kInternalMime = "application/x-qdeskbox-items";

/**
 * 生成拖拽时跟随光标的缩略图。
 * @param icon 被拖条目的图标；为空时只画底板（不报错）。
 * @param count 被拖条目数；>1 时右下角画蓝色圆形角标显示数量。
 * @return 72×72 的透明底位图，调用方通过 QDrag::setPixmap 接管。
 *
 * 尺寸固定而非按图标尺寸缩放：拖拽反馈要稳定，不能随用户 Ctrl+滚轮变化。
 */
QPixmap makeDragPixmap(const QIcon &icon, int count)
{
    const int side = 72;          // 缩略图边长，也是热点取半的基准
    QPixmap pm(side, side);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // 半透明深色圆角底板 (30,32,38,170)：保证浅色/深色壁纸上图标都看得清
    QPainterPath path;
    path.addRoundedRect(QRectF(1, 1, side - 2, side - 2), 12, 12);
    p.fillPath(path, QColor(30, 32, 38, 170));
    p.setPen(QPen(QColor(255, 255, 255, 90), 1));
    p.drawPath(path);

    const int is = 44;            // 图标绘制区边长，留出 14px 内边距
    icon.paint(&p, QRect((side - is) / 2, (side - is) / 2, is, is), Qt::AlignCenter, QIcon::Normal);

    if (count > 1) {
        // 多选角标：蓝底 + 深色数字，与 FileItemDelegate 的堆叠角标观感保持一致
        p.setBrush(QColor(76, 194, 255));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(side - 12, side - 12), 10, 10);
        p.setPen(Qt::black);
        QFont f = AppSettings::get().uiFont(11, QFont::DemiBold);
        p.setFont(f);
        p.drawText(QRectF(side - 22, side - 22, 20, 20), Qt::AlignCenter, QString::number(count));
    }
    return pm;
}
} // namespace

// 构造即完成全部静态配置：透明背景 + 自绘委托 + 细滚动条。
// 这里关掉 Qt 原生编辑触发器，重命名改用自建 QLineEdit（否则原生编辑器会盖住委托底色）。
FileListView::FileListView(QWidget *parent)
    : QListView(parent)
{
    m_delegate = new FileItemDelegate(this);
    setItemDelegate(m_delegate);

    setFrameShape(QFrame::NoFrame);
    setSelectionMode(QAbstractItemView::ExtendedSelection);   // 支持 Ctrl/Shift 多选，批量拖动与删除都依赖它
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);             // 既可拖出也可接收
    setDefaultDropAction(Qt::MoveAction);                     // 盒内拖动语义就是「移动/排序」
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);      // 图标模式靠换行，列表模式靠省略号，不需要横滚
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);  // 按像素滚动，滚轮手感更细腻
    setUniformItemSizes(true);                                // 所有单元格同尺寸，Qt 可跳过逐项测量
    setSelectionRectVisible(true);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);                       // 悬停高亮由委托读取 State_MouseOver，必须开视口追踪
    viewport()->setAutoFillBackground(false);
    setAttribute(Qt::WA_TranslucentBackground);                // 透出下层的亚克力/云母材质

    // 全部用 transparent 打底：真正的内容与选中态由委托自绘，QSS 只负责去边框和滚动条外观
    setStyleSheet(QStringLiteral(
        "QListView { background: transparent; border: none; outline: none; }"
        "QListView::item { background: transparent; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 1px; }"
        "QScrollBar::handle:vertical { background: rgba(140,146,160,120); border-radius: 3px; min-height: 28px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(160,168,182,180); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"));
}

// 接管模型所有权；两个信号分开连：先刷新网格，再向上通报「内容变了」。
// 两条连接同源（都来自 contentsChanged），靠 connect 的注册顺序保证「先排布、后通知」。
void FileListView::attach(FileItemModel *model, const QString &boxId)
{
    m_model = model;
    m_boxId = boxId;
    m_model->setParent(this);
    setModel(model);

    connect(model, &FileItemModel::contentsChanged, this, [this] { updateGrid(); });
    connect(model, &FileItemModel::contentsChanged, this, &FileListView::contentsMutated);
}

int FileListView::viewMode() const
{
    // 问委托而不是问 QListView：QListView::viewMode() 返回 IconMode/ListMode 枚举，
    // 与配置里的 0/1 不是同一套编号，混用会把列表当成图标模式。
    return m_delegate->viewMode();
}

// 委托持有唯一一份图标尺寸，视图不再另存，避免两处不同步。
int FileListView::iconSizeValue() const
{
    return m_delegate->iconSize();
}

// 委托缺失时的 94×126 只是兜底常量，正常路径（构造后）永远走左边。
QSize FileListView::gridCellSize() const
{
    return m_delegate ? m_delegate->cellSize() : QSize(94, 126);
}

// 委托与视图两侧都要设：委托决定怎么画，QListView 决定怎么摆。
void FileListView::applyAppearance(int viewMode, int iconSize, int nameLines)
{
    m_delegate->setViewMode(viewMode);
    m_delegate->setIconSize(iconSize);
    m_delegate->setNameLines(nameLines);

    if (viewMode == 0) {
        // 图标模式：从左到右自动换行；Movement=Free 才能配合「拖到某项上 = 移入文件夹」的命中逻辑
        setViewMode(QListView::IconMode);
        setFlow(QListView::LeftToRight);
        setWrapping(true);
        setResizeMode(QListView::Adjust);
        setSpacing(4);
        setMovement(QListView::Free);
    } else {
        // 列表模式：单列纵向排布，尺寸固定，不允许 Qt 自己搬动条目（排序由模型负责）
        setViewMode(QListView::ListMode);
        setFlow(QListView::TopToBottom);
        setWrapping(false);
        setResizeMode(QListView::Fixed);
        setSpacing(0);
        setMovement(QListView::Static);
    }
    updateGrid();
}

// 列表模式的格子宽度必须跟随视口：否则右侧「类型 · 大小」元信息会跑出可视区。
// 这里直接解引用 m_delegate 不判空：updateGrid() 只由本类在构造之后调用，
// 对外的 gridCellSize() 才需要兜底（外部可能更早拿到视图指针）。
void FileListView::updateGrid()
{
    if (m_delegate->viewMode() == 0) {
        setGridSize(m_delegate->cellSize());
    } else {
        setGridSize(QSize(qMax(120, viewport()->width()), m_delegate->cellSize().height()));
    }
    doItemsLayout();          // 改 gridSize 后必须强制重排，否则新尺寸下一帧才生效
    viewport()->update();
}

// 逐项反查路径而不是直接取 data(PathRole)，是为了统一走模型的「索引→路径」校验。
QStringList FileListView::selectedPaths() const
{
    QStringList out;
    if (!m_model)
        return out;
    const QModelIndexList idxs = selectionModel() ? selectionModel()->selectedIndexes() : QModelIndexList();
    for (const QModelIndex &i : idxs) {
        const QString p = m_model->pathAt(i);
        if (!p.isEmpty())
            out << p;
    }
    return out;
}

// 拖出：同时提供 file:// URL（给资源管理器）和内部 JSON（给另一个整理盒）。
// 内部载荷带 boxId，接收端据此判断「同盒 → 排序」「跨盒 → 移动」。
void FileListView::startDrag(Qt::DropActions supportedActions)
{
    Q_UNUSED(supportedActions)
    if (!m_model)
        return;
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;

    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &p : paths)
        urls << QUrl::fromLocalFile(p);
    mime->setUrls(urls);

    QJsonObject payload;
    payload["box"] = m_boxId;
    QJsonArray arr;
    for (const QString &p : paths)
        arr.append(p);
    payload["paths"] = arr;
    mime->setData(kInternalMime, QJsonDocument(payload).toJson(QJsonDocument::Compact));

    // 缩略图只取当前项图标：多选时不必拼多图，数量已由角标表达
    const QIcon icon = currentIndex().data(FileItemModel::IconRole).value<QIcon>();
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const QPixmap pm = makeDragPixmap(icon, int(paths.size()));
    drag->setPixmap(pm);
    drag->setHotSpot(QPoint(pm.width() / 2, pm.height() / 2));   // 热点居中，指针落在缩略图正中

    // 三种动作都允许，让落点（资源管理器/桌面/其他盒子）自行裁决；默认 Move 符合整理语义
    drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::MoveAction);

    // 拖出经常意味着文件已被外部移走，刷新一次让界面跟上磁盘
    if (m_model)
        m_model->refresh();
}

// 进门门槛：内部格式优先判 Move，否则带 URL 就按 Copy；不认识的事件交回基类。
// 这里只是「默认动作」，真正的落点动作在 dragMoveEvent / dropEvent 里还会被改写，
// 所以不必在这里读 Ctrl / Shift。
void FileListView::dragEnterEvent(QDragEnterEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (mime->hasUrls() || mime->hasFormat(kInternalMime)) {
        e->setDropAction(mime->hasFormat(kInternalMime) ? Qt::MoveAction : Qt::CopyAction);
        e->accept();
        return;
    }
    QListView::dragEnterEvent(e);
}

// 拖动过程中持续更新落点动作：内部拖动默认移动，按住 Ctrl 变成复制。
// 顺带把当前项切到悬停的文件夹上，让委托画出选中态，提示「松手会放进这里」。
void FileListView::dragMoveEvent(QDragMoveEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (!mime->hasUrls() && !mime->hasFormat(kInternalMime)) {
        QListView::dragMoveEvent(e);
        return;
    }
    const bool internal = mime->hasFormat(kInternalMime);
    e->setDropAction(internal && !e->modifiers().testFlag(Qt::ControlModifier)
                         ? Qt::MoveAction
                         : Qt::CopyAction);

    // 悬停在文件夹上时给出合成高亮
    const QModelIndex idx = indexAt(e->position().toPoint());
    if (idx.isValid() && m_model && m_model->isDirIndex(idx))
        setCurrentIndex(idx);
    e->accept();
}

// 落点分流的总闸：
//   1) 同盒 + 没拖到文件夹 + 没按 Ctrl + 来源是自己 → 纯排序（改模型行序）
//   2) 其余情况 → 复制/移动进 targetDir（悬停文件夹则为该文件夹，否则为当前目录）
void FileListView::dropEvent(QDropEvent *e)
{
    if (!m_model) {
        e->ignore();
        return;
    }
    const QMimeData *mime = e->mimeData();
    const QPoint pos = e->position().toPoint();
    const QModelIndex hoverIndex = indexAt(pos);

    QStringList paths;
    bool internal = false;
    QString sourceBox;                 // 仅内部拖拽有效；空串表示来源不是整理盒
    if (mime->hasFormat(kInternalMime)) {
        internal = true;
        const QJsonObject o = QJsonDocument::fromJson(mime->data(kInternalMime)).object();
        sourceBox = o.value("box").toString();
        for (const QJsonValue &v : o.value("paths").toArray())
            paths << v.toString();
    } else if (mime->hasUrls()) {
        // 外部拖入：只接受本地文件，忽略 http/邮件等虚拟 URL
        for (const QUrl &u : mime->urls())
            if (u.isLocalFile())
                paths << u.toLocalFile();
    }

    if (paths.isEmpty()) {
        e->ignore();
        return;
    }

    const bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);

    // 拖到文件夹图标上 → 移动/复制进该文件夹
    const bool ontoFolder = hoverIndex.isValid() && m_model->isDirIndex(hoverIndex);
    const QString targetDir = ontoFolder ? m_model->pathAt(hoverIndex) : m_model->folder();

    // 同盒子内部、非拖到子文件夹、没按 Ctrl → 纯排序。
    // 最后一个条件 e->source() == this 不可省：model->moveRow 只能改本模型的行序，
    // 若来源是另一个视图（哪怕 boxId 相同），也必须落到下面的「复制/移动」分支。
    const bool reorder = internal && sourceBox == m_boxId && !ontoFolder && !ctrl
                         && e->source() == this;

    if (reorder) {
        // 多选时也只用第一个被选中的行做搬移源：moveRow 一次只能搬一行。
        int from = currentIndex().row();
        if (!selectedIndexes().isEmpty())
            from = selectedIndexes().first().row();
        const int to = insertRowAt(pos);
        if (from >= 0 && m_model->moveRow(from, to)) {
            e->setDropAction(Qt::MoveAction);
            e->accept();
            emit contentsMutated();     // 手动顺序变了，需要上层落盘
            return;
        }
        e->ignore();
        return;
    }

    // 复制 or 移动：内部拖拽默认移动（Ctrl 复制）；外部拖入默认复制（Shift 移动），
    // 与资源管理器的习惯保持一致。
    bool wantMove = false;
    if (internal)
        wantMove = !ctrl;
    else
        wantMove = e->modifiers().testFlag(Qt::ShiftModifier); // 外部拖入默认复制

    QStringList failed;
    if (wantMove)
        failed = ShellActions::moveInto(paths, targetDir);
    else
        failed = ShellActions::copyInto(paths, targetDir);

    m_model->refresh();
    if (failed.isEmpty()) {
        e->setDropAction(wantMove ? Qt::MoveAction : Qt::CopyAction);
        e->accept();
    } else {
        // 有失败项时仍 accept，只把动作降级为 IgnoreAction，避免源端再自行补一次搬运。
        // failed 里的路径在这里被丢弃，不会提示用户。
        e->setDropAction(Qt::IgnoreAction);
        e->accept();
    }
    emit contentsMutated();
}

// 双击条目即请求上层打开；空白处交回基类（保留默认的框选/展开行为）。
void FileListView::mouseDoubleClickEvent(QMouseEvent *e)
{
    const QModelIndex idx = indexAt(e->position().toPoint());
    if (idx.isValid() && m_model) {
        emit openRequested(m_model->pathAt(idx));
        return;
    }
    QListView::mouseDoubleClickEvent(e);
}

// 右键菜单只是「发信号」：菜单本体由 FileBoxWindow 构建，本类不持有菜单对象。
// 在未选中的条目上右键会先把它设为当前项，让菜单的「对谁操作」与视觉一致。
void FileListView::contextMenuEvent(QContextMenuEvent *e)
{
    const QModelIndex idx = indexAt(e->pos());
    if (idx.isValid()) {
        if (!selectionModel()->isSelected(idx))
            setCurrentIndex(idx);
        emit contextMenuOnItem(m_model->pathAt(idx), e->globalPos());
    } else {
        clearSelection();                       // 空白处右键 = 取消选择，后续菜单作用于整个盒子
        emit contextMenuOnEmpty(e->globalPos());
    }
    e->accept();
}

// 快捷键分发。注意 F2 / Delete / Enter 处理完直接 return，不再下传基类，
// 否则基类会再做一次默认处理（例如 Enter 触发编辑）。
void FileListView::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F2:
        beginRenameOnCurrent();
        return;
    case Qt::Key_Delete:
        if (m_model) {
            const QStringList paths = selectedPaths();
            if (!paths.isEmpty()) {
                bool ok = true;
                if (AppSettings::get().confirmDelete) {
                    // 默认按钮是「是」，但走的是回收站，属于可撤销操作
                    QMessageBox::StandardButton r = QMessageBox::question(
                        this, QStringLiteral("删除"),
                        QStringLiteral("将 %1 个项目移到回收站？").arg(paths.size()),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                    ok = (r == QMessageBox::Yes);
                }
                if (ok) {
                    ShellActions::moveToRecycleBin(paths);   // 回收站而非永久删除
                    m_model->refresh();
                    emit contentsMutated();
                }
            }
        }
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_model && currentIndex().isValid()) {
            emit openRequested(m_model->pathAt(currentIndex()));
            return;
        }
        break;
    case Qt::Key_Escape:
        cancelRename();          // 非重命名态是空操作，所以不必先判 m_renaming
        break;
    default:
        break;
    }
    QListView::keyPressEvent(e);
}

// 只有列表模式需要跟随宽度重算格子；图标模式是固定单元格，宽度变化不影响 gridSize。
void FileListView::resizeEvent(QResizeEvent *e)
{
    QListView::resizeEvent(e);
    if (m_delegate->viewMode() == 1)
        updateGrid();
}

// Ctrl+滚轮缩放图标：步长固定 8px，范围 [32,160]。
// 只有尺寸真的变了才发信号，避免到边界后仍然持续触发配置写盘。
void FileListView::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
        const int delta = e->angleDelta().y();
        const int next = qBound(32, m_delegate->iconSize() + (delta > 0 ? 8 : -8), 160);
        if (next != m_delegate->iconSize()) {
            m_delegate->setIconSize(next);
            updateGrid();
            emit iconSizeWheeled(next);
        }
        e->accept();
        return;
    }
    QListView::wheelEvent(e);
}

// 失焦视为「用户确认」：点别处即提交，避免编辑器一直挂着。
// 注意这会连带触发一次磁盘重命名，属于有意为之的行为。
void FileListView::focusOutEvent(QFocusEvent *e)
{
    QListView::focusOutEvent(e);
    if (m_renaming)
        commitRename();
}

// 把落点换算成插入行号（用于盒内排序）。
// 先找「落点所在的格子」，找不到再退化为「距离最近的项」，保证拖到空白处也能排到合理位置。
int FileListView::insertRowAt(const QPoint &pos) const
{
    if (!m_model)
        return 0;
    const int count = m_model->rowCount();
    if (count == 0)
        return 0;

    for (int i = 0; i < count; ++i) {
        const QRect r = visualRect(m_model->index(i, 0));
        if (!r.isValid() || !r.contains(pos))
            continue;
        if (viewMode() == QListView::ListMode)
            return pos.y() < r.center().y() ? i : i + 1;    // 列表：只看纵向中线
        if (pos.y() < r.center().y())
            // 图标模式下格子比条目宽，横向落点暂不参与判断：三目两支都是 i（上半格统一插到本项前），
            // 想改成「左半/右半」只需改右边的分支，其它地方不用动。
            return (pos.x() < r.center().x()) ? i : i;
        return i + 1;
    }

    // 落在所有格子之外：按中心点欧氏距离找最近的项（dx²+dy²，不需要开方）
    int nearest = count;
    int bestDist = INT_MAX;
    for (int i = 0; i < count; ++i) {
        const QRect r = visualRect(m_model->index(i, 0));
        if (!r.isValid())
            continue;
        const int dx = pos.x() - r.center().x();
        const int dy = pos.y() - r.center().y();
        const int dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            nearest = i;
        }
    }
    if (nearest >= count)
        return count;

    // 找到最近项后仍按「中线上下」决定插到它前面还是后面
    const QRect r = visualRect(m_model->index(nearest, 0));
    if (viewMode() == QListView::ListMode)
        return pos.y() < r.center().y() ? nearest : nearest + 1;
    if (pos.y() < r.top())
        return nearest;
    return (pos.y() < r.center().y()) ? nearest : nearest + 1;
}

// 只转发当前索引；currentIndex() 无效时 beginRename() 内部会直接返回，这里不重复判断。
void FileListView::beginRenameOnCurrent()
{
    beginRename(currentIndex());
}

// 内联重命名：编辑器懒创建（只在第一次 F2 时构造），之后复用。
// 几何位置取自委托的 textRect()，让输入框正好压在名字上，视觉上像原地编辑。
void FileListView::beginRename(const QModelIndex &idx)
{
    if (!m_model || !idx.isValid())
        return;
    m_renameIndex = idx;
    m_renamePath = m_model->pathAt(idx);     // 后续一切操作以路径为准，索引可能被刷新作废
    if (m_renamePath.isEmpty())
        return;

    const AppSettings &st = AppSettings::get();
    if (!m_editor) {
        // 父对象是 viewport()：编辑器需随滚动区域一起裁剪/移动，而不是浮在整视图上
        m_editor = new QLineEdit(viewport());
        m_editor->setFrame(false);
        m_editor->setFont(st.uiFont(13));
        m_editor->installEventFilter(this);         // 拦截 Esc / Enter，避免先触发基类行为
        connect(m_editor, &QLineEdit::editingFinished, this, &FileListView::commitRename);
        connect(m_editor, &QLineEdit::returnPressed, this, &FileListView::commitRename);
    }
    // 每次进入都重刷样式：主题可能已切换（深浅色 / 强调色）
    m_editor->setStyleSheet(
        QStringLiteral("QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:5px;padding:1px 5px;}")
            .arg(st.isDark() ? "#2A2C33" : "#FFFFFF",
                 st.textColor().name(),
                 st.accent().name()));

    const QString name = idx.data(Qt::DisplayRole).toString();
    m_editor->setText(name);

    QRect cell = visualRect(idx);
    QRect tr = m_delegate->textRect(cell);
    tr.adjust(-2, -2, 2, 2);                     // 外扩 2px，避免文字贴边
    tr = tr.intersected(viewport()->rect());     // 项在边缘时会被裁，先与视口求交
    if (tr.width() < 60)                          // 太窄则向右撑开，上限 180 或视口剩余宽度
        tr.setWidth(qMin(180, viewport()->width() - tr.left() - 4));
    m_editor->setGeometry(tr);
    m_editor->show();
    m_editor->raise();
    m_editor->setFocus(Qt::OtherFocusReason);

    // 只选中主文件名，不含扩展名
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    m_editor->setSelection(0, dot > 0 ? dot : int(name.size()));   // dot<=0（隐藏文件/无扩展名）时全选
    m_renaming = true;
}

// m_renaming 是唯一守门标志：editingFinished 与 returnPressed 可能先后都到，
// 靠它保证一次重命名只落盘一次。
void FileListView::commitRename()
{
    if (!m_renaming || !m_editor)
        return;
    m_renaming = false;
    const QString newName = m_editor->text().trimmed();
    m_editor->hide();

    // 空名或与原文件名相同 → 视为放弃，不碰磁盘（避免 Windows 上的「同名」错误）
    if (!newName.isEmpty() && newName != QFileInfo(m_renamePath).fileName()) {
        if (!ShellActions::rename(m_renamePath, newName)) {
            QMessageBox::warning(this, QStringLiteral("重命名失败"),
                                 QStringLiteral("无法重命名为「%1」，可能存在同名文件或名称非法。").arg(newName));
        }
    }
    // 失败也照常刷新并通报：让上层重新读盘，界面回到真实状态
    if (m_model)
        m_model->refresh();
    emit contentsMutated();
}

// 取消路径：只复位标志并隐藏编辑器，绝不落盘。
void FileListView::cancelRename()
{
    if (!m_renaming)
        return;
    m_renaming = false;
    if (m_editor)
        m_editor->hide();
}

// 编辑器上的按键要抢在 QLineEdit 默认处理之前：
// Esc 走取消（不会发出 editingFinished 之外的意外信号），Enter 走提交。
// 其余事件统一交回基类，保持 QListView 自身的过滤逻辑可用。
bool FileListView::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_editor) {
        if (e->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(e);
            if (ke->key() == Qt::Key_Escape) {
                cancelRename();
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                commitRename();
                return true;
            }
        }
    }
    return QListView::eventFilter(obj, e);
}
