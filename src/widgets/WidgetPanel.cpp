/**
 * @file WidgetPanel.cpp
 * 面板基类的「透明化」实现：构造时清掉系统背景，paintEvent 什么都不画。
 *
 * 坑：Qt 的 QWidget 默认会填系统底色，若不清掉 WA_NoSystemBackground / autoFillBackground，
 * 内容区就会变成一块不透明灰块，把 FileBoxWindow 的玻璃材质整个盖住。
 */
#include "widgets/WidgetPanel.h"

// parent 一路传给 QWidget，面板不需要在这里建立任何子对象或布局
WidgetPanel::WidgetPanel(QWidget *parent)
    : QWidget(parent)
{
    // 背景由外层窗口绘制，面板保持透明
    // 两个属性要一起关：WA_NoSystemBackground 让 Qt 不去填系统底色，
    // autoFillBackground 关掉调色板填充，缺一个都会出现灰底
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    // 这里也不设 sizePolicy：面板由窗口加进内容区布局并铺满，尺寸约束只走
    // minimumSizeHint()/maximumSizeHint()，自己写 policy 反而和布局打架
}

// 故意不调用 QWidget::paintEvent（那会走样式表绘制），也不 open QPainter：
// 本类自己一个像素都不画，父窗口已经先把玻璃背景画好了
void WidgetPanel::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e)
    // 不绘制：让外层的玻璃背景透上来
}
