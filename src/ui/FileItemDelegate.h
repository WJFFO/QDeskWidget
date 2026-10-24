/**
 * @file FileItemDelegate.h
 * @brief 整理盒条目的自绘委托：图标 / 名称 / 元信息 / 选中与悬停态，外加手动堆叠外观。
 *
 * 之所以完全自绘而不套 QSS：视图跑在亚克力/云母材质上，原生 item 的实心底色会把
 * 背景糊住；自绘还能拿到精确的 textRect()，供 FileListView 定位内联重命名输入框。
 * 该委托不持有数据，全部信息通过 index.data(FileItemModel::XxxRole) 取。
 */
#pragma once

#include <QStyledItemDelegate>

/**
 * 图标网格 / 列表两种绘制模式的委托。
 * 自绘圆角悬停、选中背景，保证亚克力背景下的通透观感。
 *
 * 「堆叠」不是本委托的第三种模式：堆叠项在数据层就是一个以 .pile 结尾的普通文件夹，
 * 委托只是在图标模式下额外画两层错位卡片 + 数量角标（见 IsPileRole / PileCountRole）。
 */
class FileItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    /** @param parent 父对象，通常为 FileListView；本委托不拥有模型。 */
    explicit FileItemDelegate(QObject *parent = nullptr);

    /**
     * @brief 切换绘制模式。
     * @param mode 0=图标网格，1=列表（单列、左侧小图标、右侧元信息）。
     *             只影响绘制分支与 cellSize()，不改视图的排布策略。
     */
    void setViewMode(int mode);   // 0 图标，1 列表
    /** @return 当前绘制模式 0/1。 */
    int viewMode() const { return m_viewMode; }

    /**
     * @brief 设置图标边长。
     * @param px 期望边长（px），内部夹到 [24,192]；列表模式下还会再压到 ≤28 绘制。
     */
    void setIconSize(int px);
    /** @return 当前图标边长（px），已含夹取。 */
    int iconSize() const { return m_iconSize; }

    /**
     * @brief 设置文件名最多显示行数。
     * @param lines 只会被夹到 1 或 2（超过 2 一律按 2 处理）——两行已够区分长文件名。
     */
    void setNameLines(int lines);
    /** @return 当前名称行数 1/2。 */
    int nameLines() const { return m_nameLines; }

    /**
     * @return 单元格尺寸（px）：图标模式按图标边长 + 名称行数动态算出，列表模式固定 200×36。
     *
     * 列表模式的行高刻意不跟随 iconSize / nameLines——列表本来不折行，行高浮动只会跳。
     */
    QSize cellSize() const;
    /**
     * @brief 算出名称文字的绘制区域，供 FileListView 把内联重命名输入框压在名字上。
     * @param cell 整个条目的矩形（不是文字矩形），通常取自 QListView::visualRect()。
     * @return 名称区域；图标模式在图标下方，列表模式在图标右侧并给右侧元信息留出空间。
     *
     * 必须与 paint() 中实际 drawText 的位置逐像素一致，否则编辑器会明显跑偏。
     */
    QRect textRect(const QRect &cell) const;

    /**
     * @brief 绘制单个条目。
     * @param painter 目标画笔，函数内部 save()/restore()，调用方状态不受影响。
     * @param option 含 rect（条目矩形）与 state（Selected / MouseOver 决定背景）。
     * @param index 模型索引，角色读取失败时按空图标 / 空名降级绘制。
     */
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    /**
     * @brief 条目尺寸直接等于 cellSize()，忽略 option 与 index。
     * @return 当前模式下的单元格尺寸。
     *
     * 与 cellSize() 保持一致是硬约束：视图用 cellSize() 设 gridSize，若两者不等，
     * 条目会被拉伸或截断，点击热区也会错位。
     */
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    int m_viewMode = 0;      // 0=图标网格，1=列表；决定 paint() 与 cellSize() 走哪个分支
    int m_iconSize = 68;     // 图标边长（px），有效范围 [24,192]，默认与 AppSettings::iconSize 对齐
    int m_nameLines = 2;     // 名称最多行数，取值仅 1 或 2
};
