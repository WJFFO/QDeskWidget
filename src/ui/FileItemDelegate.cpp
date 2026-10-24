/**
 * @file FileItemDelegate.cpp
 * @brief FileItemDelegate 的绘制实现，含按字符折行的工具函数 wrapText()。
 *
 * 尺寸约定（改动时三处必须同步）：cellSize() 给出网格单元 → sizeHint() 原样返回它
 * → FileListView::updateGrid() 用同一个值设 gridSize。任何一处不一致都会导致
 * 条目被拉伸、点击热区偏移、内联重命名输入框错位。
 * 主题色基本都取自 AppSettings；仅堆叠卡层与角标文字用了写死的白色/近黑色，
 * 因为它们依赖的是「当前底色深浅」而不是主题变量。
 */
#include "ui/FileItemDelegate.h"
#include "core/AppSettings.h"
#include "core/FileItemModel.h"
#include "core/ShellActions.h"

#include <QApplication>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>

namespace
{
/**
 * 按字符折行，最多 maxLines 行，最后一行省略号收尾。
 *
 * 不用 QFontMetrics::elidedText 直接截断，是因为中文文件名没有空格可断，
 * 逐字符累加宽度是唯一能算出「每行放得下几个字」的可靠方式（对纯 ASCII 也成立）。
 *
 * @param fm 已设定字体的度量对象，用于逐字符测宽；调用方需保证与最终绘制字体一致。
 * @param text 原始文件名，可能含扩展名；空串直接返回空列表。
 * @param width 可用宽度（px），≤0 时返回空列表（调用方按「不画」处理）。
 * @param maxLines 最大行数，≥1；==1 时退化为单行中部省略。
 * @return 每行一个元素的列表，元素数 ≤ maxLines；未溢出时就是原文本身。
 */
QStringList wrapText(const QFontMetrics &fm, const QString &text, int width, int maxLines)
{
    QStringList lines;
    if (text.isEmpty() || width <= 0)
        return lines;

    // 一行放得下就别折腾省略号，否则「a.txt」也可能被中间截断
    if (fm.horizontalAdvance(text) <= width) {
        lines << text;
        return lines;
    }
    if (maxLines <= 1) {
        lines << fm.elidedText(text, Qt::ElideMiddle, width);
        return lines;
    }

    QString current;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (fm.horizontalAdvance(current + c) > width && !current.isEmpty()) {
            if (lines.size() == maxLines - 1) {
                // 最后一行：剩余全部内容省略
                // 回退 i - current.size() 个字符，把已攒进 current 的部分一起交出去，
                // 否则最后一行的开头会比实际显示位置靠后，丢字。
                lines << fm.elidedText(text.mid(i - current.size()), Qt::ElideMiddle, width);
                return lines;
            }
            lines << current;
            current.clear();
        }
        current += c;
    }
    // 循环结束时还有剩余（总长刚好占满 maxLines 行的极少数情况），补一行带省略
    if (!current.isEmpty())
        lines << fm.elidedText(current, Qt::ElideMiddle, width);
    return lines;
}
} // namespace

// 构造无额外成员初始化，全部状态由 setter 驱动。
FileItemDelegate::FileItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

// 只改字段，不 update()：重绘由调用方（FileListView::applyAppearance → updateGrid）负责，
// 那里会顺手 doItemsLayout()，一次改动只排布一次。
void FileItemDelegate::setViewMode(int mode)
{
    m_viewMode = mode;
}

// 夹取区间 [24,192] 比 FileListView 的 Ctrl+滚轮区间（32..160）更宽：
// 滚轮只在中段调整，配置文件和外部调用可能给更极端的值，这里统一兜底。
void FileItemDelegate::setIconSize(int px)
{
    m_iconSize = qBound(24, px, 192);
}

// 只允许 1 或 2 行；0 会被抬到 1，3 以上会被压到 2。
void FileItemDelegate::setNameLines(int lines)
{
    m_nameLines = qBound(1, lines, 2);
}

// 列表模式行高固定 36px（单行文字 + 上下留白），不随 nameLines / iconSize 变化——
// 列表模式本来就不折行，跟着图标尺寸浮动只会让行高忽高忽低。
// 图标模式高度 = 上留白 6 + 图标 + 下留白 6 + 文字块 + 底部 8；宽度至少 84，
// 保证最小图标下也有可点的横向空间。
QSize FileItemDelegate::cellSize() const
{
    const AppSettings &st = AppSettings::get();
    if (m_viewMode == 1)
        return QSize(200, 36);

    const QFontMetrics fm(st.uiFont(12));
    const int textHeight = fm.height() * m_nameLines + 2;   // +2 给下行留 1px 余量，避免末行被裁
    const int w = qMax(m_iconSize + 26, 84);                // 单元宽 = 图标宽 + 26；下限 84 保证小图标时名字仍有横向空间
    const int h = m_iconSize + 12 + textHeight + 8;         // 12 = 图标上下各 6px，8 = 底部留白
    return QSize(w, h);
}

// 名称区域必须与 paint() 里实际画文字的位置严格一致，否则重命名输入框会跑偏。
QRect FileItemDelegate::textRect(const QRect &cell) const
{
    const AppSettings &st = AppSettings::get();
    const QFontMetrics fm(st.uiFont(12));
    const int textHeight = fm.height() * m_nameLines + 2;

    if (m_viewMode == 1) {
        // 列表模式：左侧留 42px 给图标，右侧留 190px 给类型/大小元信息
        return QRect(cell.left() + 42, cell.top(), qMax(40, cell.width() - 190), cell.height());
    }
    // 图标模式：图标区（6 + iconSize + 6）之下、左右各留 6px
    const int top = cell.top() + 6 + m_iconSize + 6;
    return QRect(cell.left() + 6, top, cell.width() - 12, textHeight);
}

// 绘制顺序有讲究：背景 → 堆叠卡层 → 图标 → 数量角标 → 文件名。
// 角标必须画在图标之后，否则会被图标盖住；名称最后画，保证不被前述元素遮挡。
void FileItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                             const QModelIndex &index) const
{
    const AppSettings &st = AppSettings::get();
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                            | QPainter::SmoothPixmapTransform);

    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered  = option.state & QStyle::State_MouseOver;
    const QRect cell = option.rect;

    // ---- 背景 / 选中态 ----
    // 四周内缩 3px：相邻条目的圆角之间有缝，选中块不会连成一片，视觉上更透气
    const QRectF bgRect = QRectF(cell).adjusted(3, 3, -3, -3);
    QPainterPath bgPath;
    bgPath.addRoundedRect(bgRect, 8, 8);
    if (selected) {
        // 选中用强调色半透明填充（深色 72 / 浅色 52，深浅底上对比度接近）+ 同色描边
        QColor c = st.accent();
        c.setAlpha(st.isDark() ? 72 : 52);
        painter->fillPath(bgPath, c);
        QColor border = st.accent();
        border.setAlpha(150);
        painter->setPen(QPen(border, 1));
        painter->drawPath(bgPath);
    } else if (hovered) {
        // 悬停只铺一层极淡的底色，不描边：避免和选中态混淆
        painter->fillPath(bgPath, st.hoverColor());
    }

    const QIcon icon = index.data(FileItemModel::IconRole).value<QIcon>();
    const QString name = index.data(Qt::DisplayRole).toString();
    const bool isDir = index.data(FileItemModel::IsDirRole).toBool();

    if (m_viewMode == 1) {
        // ---- 列表模式 ----
        // 列表里的图标只是辅助标识，压到 16..28px：更大的话行高 36px 会显得拥挤。
        // 末选的 22 只是兜底：setIconSize() 已把 m_iconSize 夹到 ≥24，正常路径取不到。
        const int iconPx = qBound(16, m_iconSize > 0 ? qMin(m_iconSize, 28) : 22, 28);
        const QRect iconRect(cell.left() + 10, cell.center().y() - iconPx / 2, iconPx, iconPx);
        if (!icon.isNull())
            icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal);

        const QFont font = st.uiFont(13);
        painter->setFont(font);
        painter->setPen(st.textColor());
        const QRect nameRect = textRect(cell);
        const QFontMetrics fm(font);
        // 列表模式不折行：中部省略，保证扩展名（文件名尾部）尽量可见
        painter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                          fm.elidedText(name, Qt::ElideMiddle, nameRect.width()));

        // 右侧元信息
        const QFont metaFont = st.uiFont(11);
        painter->setFont(metaFont);
        painter->setPen(st.subTextColor());
        const QRect metaRect(cell.right() - 175, cell.top(), 165, cell.height());  // 右边留 10px，与左侧对称
        QString meta = index.data(FileItemModel::TypeRole).toString();
        if (!isDir) {
            // 文件夹没有有意义的「大小」，只有文件才追加尺寸
            const qint64 size = index.data(FileItemModel::SizeRole).toLongLong();
            meta += QStringLiteral("  ·  %1").arg(ShellActions::humanSize(size));
        }
        painter->drawText(metaRect, Qt::AlignVCenter | Qt::AlignRight, meta);
    } else {
        // ---- 图标网格模式 ----
        // 水平居中，垂直从顶部 7px 开始（与 cellSize() 的上留白一致）
        const QRect iconRect(cell.left() + (cell.width() - m_iconSize) / 2,
                             cell.top() + 7, m_iconSize, m_iconSize);

        const bool isPile = index.data(FileItemModel::IsPileRole).toBool();
        const int pileCount = index.data(FileItemModel::PileCountRole).toInt();

        // 手动堆叠：在文件夹图标背后画两层错位卡片，右下角挂数量角标
        // 从远到近画（layer 2 → 1），后画的压在上面；向右上错开 off 像素，模拟一叠卡片。
        if (isPile) {
            const int off = qBound(3, m_iconSize / 12, 7);   // 随图标缩放但夹在 [3,7]，太小看不出层次、太大溢出格子
            for (int layer = 2; layer >= 1; --layer) {
                const QRect r = iconRect.adjusted(layer * off, -layer * off,
                                                  layer * off, -layer * off);
                QPainterPath path;
                path.addRoundedRect(QRectF(r), 7, 7);
                // 越远的层越透明：深色加 alpha、浅色加白度，两套主题下都能拉开层次
                const QColor bg = st.isDark()
                                      ? QColor(255, 255, 255, 12 + layer * 9)
                                      : QColor(255, 255, 255, 120 + layer * 40);
                painter->fillPath(path, bg);
                painter->setPen(QPen(st.borderColor(), 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(path);
            }
        }

        if (!icon.isNull())
            icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal);

        if (isPile && pileCount > 0) {
            // 数量角标：贴在图标右下角，宽度随位数自适应，高度固定 16、圆角 8 = 胶囊形
            const QString badge = QString::number(pileCount);
            const QFont badgeFont = st.uiFont(10, QFont::DemiBold);
            const QFontMetrics bfm(badgeFont);
            const int w = qMax(18, bfm.horizontalAdvance(badge) + 10);   // +10 = 左右各 5px 内边距
            const QRect badgeRect(iconRect.right() - w + 4, iconRect.bottom() - 12, w, 16);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setFont(badgeFont);
            painter->setPen(Qt::NoPen);
            painter->setBrush(st.accent());
            painter->drawRoundedRect(badgeRect, 8, 8);
            // 角标文字用与强调色反差最大的固定色：深色主题配近黑，浅色主题配白
            painter->setPen(st.isDark() ? QColor(16, 18, 24) : QColor(255, 255, 255));
            painter->drawText(badgeRect, Qt::AlignCenter, badge);
            painter->setBrush(Qt::NoBrush);
        }

        const QFont font = st.uiFont(12);
        const QFontMetrics fm(font);
        painter->setFont(font);
        // 注意三目的两个分支取的是同一个颜色，实际等价于 setPen(st.textColor())：
        // 名称颜色目前不随选中态变化，选中感完全靠上面的半透明背景块承担。
        painter->setPen(selected ? st.textColor() : st.textColor());

        // 逐行手绘而不是 drawText 一次搞定：需要每行单独居中，且行数由 wrapText 控制
        const QRect tr = textRect(cell);
        const QStringList lines = wrapText(fm, name, tr.width(), m_nameLines);
        int y = tr.top();
        for (const QString &line : lines) {
            painter->drawText(QRect(tr.left(), y, tr.width(), fm.height()),
                              Qt::AlignHCenter | Qt::AlignTop, line);
            y += fm.height();
        }
    }

    painter->restore();
}

// 忽略 option/index：尺寸只由当前模式与图标尺寸决定（配合 setUniformItemSizes(true)）。
QSize FileItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    Q_UNUSED(option)
    Q_UNUSED(index)
    return cellSize();
}
