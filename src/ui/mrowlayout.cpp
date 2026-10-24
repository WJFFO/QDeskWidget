/**
 * @file mrowlayout.cpp
 * @brief MRowLayout 的实现：尾部弹簧的增删 + 自绘背景/边框。
 *
 * 本文件的核心不变量：RowAddWidget() 之后行尾一定有一个 stretch(1)。
 * removeTrailingStretch() 负责在追加前把旧的尾部弹簧摘掉，避免弹簧越加越多；
 * 一旦某个操作绕过它直接 addStretch，行内就会出现多个弹簧平分空白，布局立刻变形。
 */
#include "mrowlayout.h"

#include <QPainterPath>

// 构造只做两件事：建内部横排布局、把默认边距与间距归零，
// 这样自绘的背景才能贴到控件边缘（QLayout 默认边距由样式决定，Windows 上约 9px，
// 不清掉四周就会留一圈空白，圆角背景会小一圈）。
MRowLayout::MRowLayout(QWidget *parent)
    : QWidget{parent}
{
    m_row = new QHBoxLayout(this);

    // 默认边距为 0，让背景能贴合边缘
    m_row->setContentsMargins(0, 0, 0, 0);
    m_row->setSpacing(0);
}

MRowLayout::~MRowLayout() = default;

// ================= 布局管理 =================

// 先摘尾部弹簧再追加，末尾重新补一个：保证「多个控件靠左紧挨」的行为可重复。
void MRowLayout::RowAddWidget(QWidget *w, int stretch, Qt::Alignment alignment)
{
    if (!w)
        return;
    removeTrailingStretch();
    m_row->addWidget(w, stretch, alignment);
    m_row->addStretch(1);
}

// 从尾往头逐项检查，遇到第一个不是弹簧的项就停——只清「尾部连续」的弹簧，
// 中间位置（RowPinToRight 或手动 RowAddStretch 加的）不受影响。
void MRowLayout::removeTrailingStretch()
{
    while (m_row->count() > 0) {
        QLayoutItem *last = m_row->itemAt(m_row->count() - 1);
        if (!last || !last->spacerItem()) {
            break;
        }
        // takeAt 之后 item 的所有权转移给调用方，必须自己 delete，否则每次追加都漏一个
        delete m_row->takeAt(m_row->count() - 1);
    }
}

// 插入不补尾部弹簧：常用于「插到已有控件之间」，弹簧数量由调用方自己维持。
void MRowLayout::RowInsertWidget(int index, QWidget *w, int stretch, Qt::Alignment alignment)
{
    if (!w)
        return;
    m_row->insertWidget(index, w, stretch, alignment);
}

// removeWidget 只是把控件从布局里摘掉，不会隐藏也不会改父对象。
void MRowLayout::RowRemoveWidget(QWidget *w)
{
    if (!w)
        return;
    m_row->removeWidget(w);
}

// 注意范围：删的是「所有非控件项」，包括 RowPinToRight 加在中间的弹簧。
// 判据是 item->widget() 为空（弹簧、以及将来若有嵌套布局都算）。
// 倒序遍历是为了删除后下标不前移，正序会在取走一项后漏掉下一项。
void MRowLayout::RowClearSpacers()
{
    for (int i = m_row->count() - 1; i >= 0; --i) {
        QLayoutItem *item = m_row->itemAt(i);
        if (item && !item->widget()) {
            delete m_row->takeAt(i);
        }
    }
}

// 从头部反复取走，直到布局空。控件会被 setParent(nullptr) 变成独立顶层窗口，
// 调用方若不接管就会「消失」在屏幕上（不是被删，但没有父级）。
void MRowLayout::RowClear()
{
    // 清空布局中的所有子项（控件本身交由调用方管理生命周期）
    while (m_row->count() > 0) {
        QLayoutItem *item = m_row->takeAt(0);
        if (item->widget()) {
            item->widget()->setParent(nullptr);
        }
        delete item;
    }
}

int MRowLayout::RowCount() const
{
    // 只数真实控件：行尾的 stretch 是内部占位，不该算进容量。
    int count = 0;
    for (int i = 0; i < m_row->count(); ++i) {
        if (m_row->itemAt(i)->widget()) {
            ++count;
        }
    }
    return count;
}

// 直接透传，不参与尾部弹簧管理（不会先清旧的再补新的）；
// 连续调用会叠加多个弹簧，它们平分空白，行的排布随之改变。
void MRowLayout::RowAddStretch(int stretch)
{
    m_row->addStretch(stretch);
}

// 右对齐的「重型」做法：清掉所有弹簧 → 取出 w → 补一个弹簧 → 重新追加 w。
// 之所以不用 addStretch + addWidget 简单组合，是因为 w 可能已经在布局中间，
// 必须先摘出来才能改变它的顺序。
void MRowLayout::RowPinToRight(QWidget *w, Qt::Alignment alignment)
{
    if (!w) {
        return;
    }

    // 先摘掉行里所有弹簧：本行的弹簧只用来把东西顶到右边，
    // 留着旧的会和新加的那个平分空白，位置就偏了。
    for (int i = m_row->count() - 1; i >= 0; --i) {
        QLayoutItem *item = m_row->itemAt(i);
        if (item && !item->widget()) {
            delete m_row->takeAt(i);
        }
    }

    // 如果 w 已经在布局里，先把它取出来（只删布局项，控件本身不动）。
    const int index = m_row->indexOf(w);
    if (index >= 0) {
        delete m_row->takeAt(index);
    }

    // 一个弹簧 + 控件收尾，w 就被顶到行尾，其余控件仍然靠左。
    // 若行内还有 stretch=1 的控件，两者会 1:1 平分空白，w 只能到中间——这时要改用 RowClearSpacers()。
    m_row->addStretch(1);
    m_row->addWidget(w, 0, alignment);
}

// 把本行挂到 host 上铺满。两层「包装」是必要的：
// host 可能已经有布局（例如业务代码先建了 QVBoxLayout），此时直接加进去即可；
// 没有布局时先建一个零边距的 QHBoxLayout，否则 addWidget 在宿主上无处安放。
void MRowLayout::RowFillHost(QWidget *host)
{
    if (!host || host == this)
        return;

    // 宿主自身没有布局时，建一个零边距的横排布局来承载本行。
    // 这层包装只存在于自定义布局内部，调用方（业务代码）不需要感知。
    QLayout *hostLayout = host->layout();
    if (!hostLayout) {
        auto *wrapper = new QHBoxLayout(host);
        wrapper->setContentsMargins(0, 0, 0, 0);
        wrapper->setSpacing(0);
        hostLayout = wrapper;
    }

    // 幂等：已经挂在宿主布局里就不再重复添加
    if (hostLayout->indexOf(this) >= 0)
        return;

    setParent(host);              // 先改父级，避免 addWidget 时 Qt 再把它挪一次
    hostLayout->addWidget(this);
}

// ================= 外观控制 =================
// 下面这组 setter 都只改字段 + update()（请求重绘），不触发 relayout：
// 颜色/圆角/线宽属于纯绘制参数，不影响任何尺寸，因此不会引起布局抖动。

void MRowLayout::SetBackgroundColor(const QColor &color)
{
    m_backgroundColor = color;
    update();
}

QColor MRowLayout::BackgroundColor() const
{
    return m_backgroundColor;
}

void MRowLayout::SetHoverColor(const QColor &color)
{
    m_hoverColor = color;
    update();
}

QColor MRowLayout::HoverColor() const
{
    return m_hoverColor;
}

void MRowLayout::SetPressedColor(const QColor &color)
{
    m_pressedColor = color;
    update();
}

QColor MRowLayout::PressedColor() const
{
    return m_pressedColor;
}

void MRowLayout::SetBorderColor(const QColor &color)
{
    m_borderColor = color;
    update();
}

void MRowLayout::SetBorderWidth(int width)
{
    m_borderWidth = width;
    update();
}

void MRowLayout::SetRadius(int radius)
{
    m_radius = radius;
    update();
}

int MRowLayout::Radius() const
{
    return m_radius;
}

// ================= 间距 / 边距 =================
// 与外观 setter 不同，这三个会真正改变内部布局的几何，Qt 会自动重新布局。

void MRowLayout::SetSpacing(int spacing)
{
    m_row->setSpacing(spacing);
}

int MRowLayout::Spacing() const
{
    return m_row->spacing();
}

void MRowLayout::SetMargin(int left, int top, int right, int bottom)
{
    m_row->setContentsMargins(left, top, right, bottom);
}

void MRowLayout::SetMargin(int margin)
{
    m_row->setContentsMargins(margin, margin, margin, margin);
}

// ================= 交互 =================

// 关闭时一并复位悬停/按下状态：否则下次打开会带着旧的按下态直接画成深色。
void MRowLayout::SetHoverEnabled(bool enabled)
{
    m_hoverEnabled = enabled;
    // 关闭时恢复状态
    if (!enabled) {
        m_isHovered = false;
        m_isPressed = false;
    }
    update();
}

bool MRowLayout::IsHoverEnabled() const
{
    return m_hoverEnabled;
}

// ================= 事件处理 =================

// 背景色优先级：按下 > 悬停 > 常态。只有 m_hoverEnabled 打开时才考虑前两者，
// 且对应颜色必须有效（isValid()），未设置的颜色会自动回落到上一级。
// 画布先内缩半线宽，保证 1px 描边完整落在控件内。
void MRowLayout::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 决定当前应该使用的背景色
    QColor bg = m_backgroundColor;
    if (m_hoverEnabled && m_isPressed && m_pressedColor.isValid()) {
        bg = m_pressedColor;
    } else if (m_hoverEnabled && m_isHovered && m_hoverColor.isValid()) {
        bg = m_hoverColor;
    }

    QRectF rect = this->rect();
    // 有边框时内缩半个线宽，避免边框被裁掉
    if (m_borderWidth > 0) {
        rect.adjust(m_borderWidth * 0.5, m_borderWidth * 0.5,
                    -m_borderWidth * 0.5, -m_borderWidth * 0.5);
    }

    // 绘制圆角背景：alpha == 0 视为「不要背景」，避免透明色仍触发一次填充
    if (bg.isValid() && bg.alpha() > 0) {
        QPainterPath path;
        path.addRoundedRect(rect, m_radius, m_radius);
        painter.fillPath(path, bg);
    }

    // 绘制边框：与背景用同一个 rect 与半径，两者才能严丝合缝
    if (m_borderWidth > 0 && m_borderColor.isValid()) {
        QPen pen(m_borderColor, m_borderWidth);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect, m_radius, m_radius);
    }
}

// 未开启高亮时连状态位都不动，保持「纯布局容器」的零开销行为。
void MRowLayout::enterEvent(QEnterEvent *event)
{
    if (m_hoverEnabled) {
        m_isHovered = true;
        update();
    }
    QWidget::enterEvent(event);
}

// 离开时顺手清 pressed：按下后拖出控件再松手，release 事件不会回到这里，
// 不主动复位就会一直保持按下色。
void MRowLayout::leaveEvent(QEvent *event)
{
    if (m_hoverEnabled) {
        m_isHovered = false;
        m_isPressed = false;
        update();
    }
    QWidget::leaveEvent(event);
}

// 只认左键；pressed() 在这里发，注意它不判断落点是否在子控件上
// （子控件会先吃掉事件，所以实际只有点在行内空白处才会到这里）。
void MRowLayout::mousePressEvent(QMouseEvent *event)
{
    if (m_hoverEnabled && event->button() == Qt::LeftButton) {
        m_isPressed = true;
        update();
        emit pressed();
    }
    QWidget::mousePressEvent(event);
}

// released() 无条件发（只要开启了高亮且是左键），clicked() 才要求「按下过」。
// 因此「在别处按下、移进来松手」只会触发 released，不会误报点击。
void MRowLayout::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_hoverEnabled && event->button() == Qt::LeftButton) {
        bool wasPressed = m_isPressed;
        m_isPressed = false;
        update();
        emit released();
        if (wasPressed) {
            emit clicked();
        }
    }
    QWidget::mouseReleaseEvent(event);
}
