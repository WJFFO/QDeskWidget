/**
 * @file mcolumnlayout.cpp
 * @brief MColumnLayout 的实现：纵向排版透传 + 自绘背景/边框。
 *
 * 与 MRowLayout 最大的不同：这里没有「尾部弹簧」的自动维护，
 * ColumnAddWidget() 纯粹是 QVBoxLayout::addWidget 的转发，追加顺序即最终顺序。
 * 需要把内容顶到顶部/底部时，必须显式 ColumnAddStretch()，否则剩余空间会均匀留白。
 */
#include "mcolumnlayout.h"

#include <QPainterPath>

// 默认边距/间距归零：自绘背景要贴边，且业务侧的间距统一由 SetMargin/SetSpacing 控制。
MColumnLayout::MColumnLayout(QWidget *parent)
    : QWidget{parent}
{
    m_column = new QVBoxLayout(this);

    // 默认边距为 0，让背景能贴合边缘
    m_column->setContentsMargins(0, 0, 0, 0);
    m_column->setSpacing(0);
}

MColumnLayout::~MColumnLayout() = default;

// ================= 布局管理 =================

// 纯转发，不加尾部弹簧：这一点与 MRowLayout::RowAddWidget 有本质区别。
void MColumnLayout::ColumnAddWidget(QWidget *w, int stretch, Qt::Alignment alignment)
{
    if (!w)
        return;
    m_column->addWidget(w, stretch, alignment);
}

// 插入位置按「布局项」索引算，弹簧也占位，所以 index 与控件序号不一定相等。
void MColumnLayout::ColumnInsertWidget(int index, QWidget *w, int stretch, Qt::Alignment alignment)
{
    if (!w)
        return;
    m_column->insertWidget(index, w, stretch, alignment);
}

// removeWidget 只摘布局项，控件仍然可见（只是不再受布局管理）。
void MColumnLayout::ColumnRemoveWidget(QWidget *w)
{
    if (!w)
        return;
    m_column->removeWidget(w);
}

// 控件被 setParent(nullptr) 后变成独立顶层窗口；要复用得重新 addWidget，
// 否则它会「飘」在屏幕上且不随本列移动。
void MColumnLayout::ColumnClear()
{
    // 清空布局中的所有子项（控件本身交由调用方管理生命周期）
    while (m_column->count() > 0) {
        QLayoutItem *item = m_column->takeAt(0);
        if (item->widget()) {
            item->widget()->setParent(nullptr);
        }
        delete item;
    }
}

// 直接返回布局项数（含弹簧），与 RowCount() 只数控件不同；调用方勿据此推断控件个数。
int MColumnLayout::ColumnCount() const
{
    return m_column->count();
}

// 弹簧不归本类管理：传 0 等于加一个固定 0 高的占位项，几乎不可见但会占一个布局项下标，
// 后续 ColumnInsertWidget 的 index 要把它算进去。
void MColumnLayout::ColumnAddStretch(int stretch)
{
    m_column->addStretch(stretch);
}

// 与 RowFillHost 同构：宿主没有布局时先补一个零边距包装布局，再把自己挂进去。
void MColumnLayout::ColumnFillHost(QWidget *host)
{
    if (!host || host == this)
        return;

    // 宿主自身没有布局时，建一个零边距的竖排布局来承载本列。
    // 这层包装只存在于自定义布局内部，调用方（业务代码）不需要感知。
    QLayout *hostLayout = host->layout();
    if (!hostLayout) {
        auto *wrapper = new QVBoxLayout(host);
        wrapper->setContentsMargins(0, 0, 0, 0);
        wrapper->setSpacing(0);
        hostLayout = wrapper;
    }

    // 幂等：已经挂在宿主布局里就不再重复添加
    if (hostLayout->indexOf(this) >= 0)
        return;

    setParent(host);              // 先改父级，避免 addWidget 时 Qt 再挪一次
    hostLayout->addWidget(this);
}

// ================= 外观控制 =================
// 这组 setter 都只改字段 + update()（请求重绘），不影响尺寸，因此不会触发重新布局。

void MColumnLayout::SetBackgroundColor(const QColor &color)
{
    m_backgroundColor = color;
    update();
}

QColor MColumnLayout::BackgroundColor() const
{
    return m_backgroundColor;
}

void MColumnLayout::SetHoverColor(const QColor &color)
{
    m_hoverColor = color;
    update();
}

QColor MColumnLayout::HoverColor() const
{
    return m_hoverColor;
}

void MColumnLayout::SetPressedColor(const QColor &color)
{
    m_pressedColor = color;
    update();
}

QColor MColumnLayout::PressedColor() const
{
    return m_pressedColor;
}

void MColumnLayout::SetBorderColor(const QColor &color)
{
    m_borderColor = color;
    update();
}

void MColumnLayout::SetBorderWidth(int width)
{
    m_borderWidth = width;
    update();
}

void MColumnLayout::SetRadius(int radius)
{
    m_radius = radius;
    update();
}

int MColumnLayout::Radius() const
{
    return m_radius;
}

// ================= 间距 / 边距 =================
// 与外观 setter 不同，这三个改变内部布局的几何，Qt 会自动重新布局。

void MColumnLayout::SetSpacing(int spacing)
{
    m_column->setSpacing(spacing);
}

int MColumnLayout::Spacing() const
{
    return m_column->spacing();
}

void MColumnLayout::SetMargin(int left, int top, int right, int bottom)
{
    m_column->setContentsMargins(left, top, right, bottom);
}

void MColumnLayout::SetMargin(int margin)
{
    m_column->setContentsMargins(margin, margin, margin, margin);
}

// ================= 交互 =================

// 关闭时一并复位悬停/按下状态，避免下次打开直接带着旧按下态绘制。
void MColumnLayout::SetHoverEnabled(bool enabled)
{
    m_hoverEnabled = enabled;
    // 关闭时恢复状态
    if (!enabled) {
        m_isHovered = false;
        m_isPressed = false;
    }
    update();
}

bool MColumnLayout::IsHoverEnabled() const
{
    return m_hoverEnabled;
}

// ================= 事件处理 =================

// 背景色优先级：按下 > 悬停 > 常态；前两者仅在 m_hoverEnabled 开启且颜色有效时生效。
// 画布先内缩半线宽，保证描边完整落在控件内部而不是被裁一半。
void MColumnLayout::paintEvent(QPaintEvent *event)
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

    // 绘制圆角背景：alpha == 0 视为不要背景（避免透明色也走一次填充）
    if (bg.isValid() && bg.alpha() > 0) {
        QPainterPath path;
        path.addRoundedRect(rect, m_radius, m_radius);
        painter.fillPath(path, bg);
    }

    // 绘制边框：与背景共用同一 rect / 半径，两者才能对齐
    if (m_borderWidth > 0 && m_borderColor.isValid()) {
        QPen pen(m_borderColor, m_borderWidth);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect, m_radius, m_radius);
    }
}

// 未开启高亮时连状态位都不动，保持纯布局容器的零开销行为。
void MColumnLayout::enterEvent(QEnterEvent *event)
{
    if (m_hoverEnabled) {
        m_isHovered = true;
        update();
    }
    QWidget::enterEvent(event);
}

// 离开时顺手清 pressed：按下后拖出控件，release 不会回到这里，不主动复位会一直保持按下色。
void MColumnLayout::leaveEvent(QEvent *event)
{
    if (m_hoverEnabled) {
        m_isHovered = false;
        m_isPressed = false;
        update();
    }
    QWidget::leaveEvent(event);
}

// 只认左键。子控件会先消费事件，所以能走到这里的点击都落在列内空白处。
void MColumnLayout::mousePressEvent(QMouseEvent *event)
{
    if (m_hoverEnabled && event->button() == Qt::LeftButton) {
        m_isPressed = true;
        update();
        emit pressed();
    }
    QWidget::mousePressEvent(event);
}

// released() 无条件发（左键 + 高亮开启），clicked() 才要求「本列上按下过」，
// 因此「在别处按下、移进来松手」不会误报点击。
void MColumnLayout::mouseReleaseEvent(QMouseEvent *event)
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
