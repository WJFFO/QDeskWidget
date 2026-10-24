/**
 * @file UiStyle.h
 * 对话框 / 表单的统一样式与布局助手（header-only，全部 inline，无 .cpp）。
 *
 * 关键约定 / 坑：
 *  - 这里的 QSS 是「字符串 + .arg() 填色」而不是资源文件：令牌全部来自 AppSettings，
 *    因此换主题后必须重新调用 dialogCss() / panelCss() 并重新 setStyleSheet()，
 *    旧样式表不会自动跟着全局设置变；
 *  - 模板里占位符编号必须从 %1 连续排到 %N，且 .arg() 的实参顺序要和编号一一对应——
 *    多参数重载是按顺序替换最低编号占位符的，跳号或错位会静默填错颜色；
 *  - 本文件只管「长得一致」，不管布局归属：返回的 MRowLayout / MColumnLayout 由调用方挂到父控件上。
 */
#pragma once

#include "core/AppSettings.h"
#include "mcolumnlayout.h"
#include "mrowlayout.h"

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPainterPath>
#include <QPushButton>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QWidget>


/**
 * 对话框与表单的统一样式 / 布局助手。
 * 表单行统一用 MRowLayout 组装，列用 MColumnLayout 组装。
 */
namespace UiStyle
{
/**
 * 下拉框：**右侧箭头自绘**，不带系统那套「深色底 + 竖直分隔线」的箭头区。
 *
 * 为什么要子类化：QSS 里只要给 `QComboBox::drop-down` 写了规则，样式引擎就不再绘制默认箭头，
 * 而 `::down-arrow` 又必须配 `image:` 才画得出来（没有图片就什么都没有，实测箭头会整个消失）。
 * 与其引入一套随主题变色的图片资源，不如直接画两条线：颜色取自 AppSettings、任意 DPI 都清晰。
 *
 * 没有 Q_OBJECT：不新增信号槽，只想改绘制；QComboBox 的元对象照常可用。
 */
class StyleComboBox : public QComboBox
{
public:
    /** @param parent 父控件，交给 QComboBox */
    explicit StyleComboBox(QWidget *parent = nullptr) : QComboBox(parent) {}

protected:
    /**
     * 先让样式表画框与编辑区，再补一个 chevron，最后画文字。
     * @param 忽略（只要知道该重画了）
     */
    void paintEvent(QPaintEvent *) override
    {
        const AppSettings &st = AppSettings::get();

        QStylePainter p(this);
        QStyleOptionComboBox opt;
        initStyleOption(&opt);

        // 关键：把箭头子控件从绘制集合里摘掉。系统的箭头区（那块比输入框更深的底 + 分隔线）
        // 就是跟着 SC_ComboBoxArrow 一起画出来的，摘掉之后框和文字仍由 QSS 负责。
        opt.subControls &= ~QStyle::SC_ComboBoxArrow;
        p.drawComplexControl(QStyle::CC_ComboBox, opt);

        // chevron（向下的小折线），右对齐且垂直居中；位置要在 QSS 给的 24px 箭头区内
        constexpr qreal kW = 9.0;  // 折线宽度（px）
        constexpr qreal kH = 5.0;  // 折线高度（px）
        const QPointF c(width() - 20.0, height() / 2.0);
        QPainterPath chevron;
        chevron.moveTo(c.x() - kW / 2, c.y() - kH / 2);
        chevron.lineTo(c.x(), c.y() + kH / 2);
        chevron.lineTo(c.x() + kW / 2, c.y() - kH / 2);

        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(st.subTextColor(), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(chevron);

        // 文字最后画：QStyleSheetStyle 下 drawComplexControl 不负责绘制当前项文本
        p.drawControl(QStyle::CE_ComboBoxLabel, opt);
    }
};


/**
 * 生成对话框通用样式表（QDialog / QLabel / QLineEdit / QComboBox / 单选复选 / 滑块 / 按钮）。
 * @param translucent 对话框自身要不要「透明底」。
 *        false（默认）= 用不透明底色，适配普通窗口；
 *        true = 只输出 `QDialog{background:transparent;}`，让窗口的亚克力/云母背板透出来
 *        （设置面板用这档；此时窗口必须同时 setAttribute(WA_TranslucentBackground)，
 *         并由调用方自己画底色与描边，否则整块是全透明的）。
 * @return 已把主题令牌填好的 QSS 字符串；明暗两套用不同的硬编码底色（深 #20222A / 浅 #F7F8FB，
 *         translucent=true 时这两色都不使用），其余颜色取自 AppSettings 的主题令牌——
 *         全部写死在 QSS 里，不经由 QPalette 解析。
 *
 * 令牌对应关系（改模板前先核对）：
 *   %1 对话框底色  %2 主文字  %3 次要文字/hint/禁用  %4 输入框底色
 *   %5 描边与滑块槽  %6 强调色（焦点边框 / 选中底色 / 滑块 / 主按钮）  %7 主按钮悬停色
 * 注意：控件底色（%4）在透明底下依然是不透明的，只有对话框那一层变透明——
 *       否则输入框叠在模糊背板上会看不清。
 */
inline QString dialogCss(bool translucent = false)
{
    const AppSettings &st = AppSettings::get();
    const QString bg      = st.isDark() ? QStringLiteral("#20222A") : QStringLiteral("#F7F8FB");
    const QString field   = st.isDark() ? QStringLiteral("#2A2C35") : QStringLiteral("#FFFFFF");
    const QString text    = st.textColor().name();
    const QString sub     = st.subTextColor().name();
    const QString border  = st.isDark() ? QStringLiteral("#3A3D48") : QStringLiteral("#DDE0E6");
    const QString accent  = st.accent().name();

    // 透明底时把「对话框底色」这条规则换掉，其余规则完全复用
    const QString dialogRule = translucent ? QStringLiteral("QDialog{background:transparent;}")
                                           : QStringLiteral("QDialog{background:%1;}");

    return QStringLiteral(
               "%1"
               "QLabel{color:%2;background:transparent;}"
               // hint / section 靠 objectName 选中，创建控件时必须显式 setObjectName
               "QLabel#hint{color:%3;}"
               "QLabel#section{color:%3;font-weight:600;}"
               "QLineEdit{background:%4;border:1px solid %5;border-radius:7px;padding:6px 9px;color:%2;"
               "selection-background-color:%6;}"
               "QLineEdit:focus{border:1px solid %6;}"
               "QComboBox{background:%4;border:1px solid %5;border-radius:7px;padding:5px 9px;color:%2;}"
               // 右侧箭头区默认会按原生控件绘制：一块比输入框更深的底 + 一条竖直分隔线。
               // 这里把子控件的边框和底色全去掉，只留箭头本身，看起来才是「一张输入框 + 一个箭头」。
               // 注意：**不要**再给 ::down-arrow 加 width/height 之类的规则——一旦命中，
               // 样式引擎就不再画默认箭头，又没有 image 可画，箭头会整个消失（踩过）。
               "QComboBox::drop-down{subcontrol-origin:padding;subcontrol-position:center right;"
               "width:24px;border:none;background:transparent;}"
               // 下拉列表是独立的顶层视图，不写这条会退回系统默认的白底黑字
               "QComboBox QAbstractItemView{background:%4;color:%2;border:1px solid %5;"
               "selection-background-color:%6;selection-color:#FFFFFF;outline:none;}"
               "QRadioButton,QCheckBox{color:%2;spacing:8px;}"
               "QSlider::groove:horizontal{height:4px;background:%5;border-radius:2px;}"
               "QSlider::sub-page:horizontal{background:%6;border-radius:2px;}"
               // margin:-6px 是为了让 14px 的圆形手柄垂直居中在 4px 的槽上
               "QSlider::handle:horizontal{width:14px;height:14px;margin:-6px 0;border-radius:7px;"
               "background:%6;}"
               "QPushButton{background:transparent;border:1px solid %5;border-radius:7px;"
               "padding:7px 18px;color:%2;}"
               "QPushButton:hover{border:1px solid %6;}"
               // 主按钮靠 objectName="primary" 选中（见 primaryButton）
               "QPushButton#primary{background:%6;border:1px solid %6;color:#FFFFFF;font-weight:600;}"
               "QPushButton#primary:hover{background:%7;}"
               "QPushButton:disabled{color:%3;border-color:%5;}")
        .arg(translucent ? dialogRule : dialogRule.arg(bg),
             text, sub, field, border, accent,
             st.isDark() ? QStringLiteral("#6FD2FF") : QStringLiteral("#0F82D6"));
}

/**
 * 给对话框套上统一字体与 dialogCss()。
 * @param dlg         目标对话框；内部直接 setStyleSheet，会覆盖该控件之前设置过的样式表
 * @param translucent 是否透明底（见 dialogCss 的说明）；为 true 时本函数**不会**替你设
 *                    WA_TranslucentBackground，调用方要在构造期自己设，否则透明底不起作用
 * 注意：这是「构造时快照」，之后主题变了要重新调用（SettingsDialog 里就是在 applyLive() 中重设）。
 */
inline void styleDialog(QDialog *dlg, bool translucent = false)
{
    dlg->setFont(AppSettings::get().uiFont(13));
    dlg->setStyleSheet(dialogCss(translucent));
}

/**
 * 组件面板样式：背景透明（玻璃由外层盒子画），控件走主题色。
 * @return 供 WidgetPanel 内部使用的 QSS。
 *
 * 与 dialogCss() 的关键差异：所有底色都是半透明白/黑（rgba），因为面板叠在盒子的玻璃表面上，
 * 不透明底色会把亚克力 / 云母效果整块盖住。令牌：%1 主文字 %2 次文字 %3 输入底
 * %4 描边 %5 强调色 %6 选中底（悬停与选中共用同一色，靠透明度区分强弱）。
 */
inline QString panelCss()
{
    const AppSettings &st = AppSettings::get();
    const QString text   = st.textColor().name();
    const QString sub    = st.subTextColor().name();
    const QString accent = st.accent().name();
    // 深色用「白上叠加」、浅色用「黑上叠加」，这样同一 alpha 在两种主题下观感接近
    const QString field  = st.isDark() ? QStringLiteral("rgba(255,255,255,0.07)")
                                       : QStringLiteral("rgba(255,255,255,0.55)");
    const QString border = st.isDark() ? QStringLiteral("rgba(255,255,255,0.13)")
                                       : QStringLiteral("rgba(15,18,24,0.11)");
    const QString sel    = st.isDark() ? QStringLiteral("rgba(76,194,255,0.22)")
                                       : QStringLiteral("rgba(0,103,192,0.16)");

    return QStringLiteral(
               "QWidget{background:transparent;}"
               "QLabel{color:%1;background:transparent;}"
               "QLabel#sub{color:%2;}"
               "QLineEdit{background:%3;border:1px solid %4;border-radius:8px;"
               "padding:6px 10px;color:%1;selection-background-color:%6;}"
               "QLineEdit:focus{border:1px solid %5;}"
               // 列表/树本身透明，靠外层玻璃透出来；::item 才画悬停与选中块
               "QListWidget,QTreeWidget{background:transparent;border:none;outline:none;color:%1;}"
               "QListWidget::item{padding:3px 6px;border-radius:6px;}"
               "QListWidget::item:hover{background:%6;}"
               "QListWidget::item:selected{background:%6;color:%1;}"
               "QPushButton{background:transparent;border:1px solid %4;border-radius:7px;"
               "padding:5px 12px;color:%1;}"
               "QPushButton:hover{border:1px solid %5;}"
               "QSlider::groove:horizontal{height:4px;background:%4;border-radius:2px;}"
               "QSlider::sub-page:horizontal{background:%5;border-radius:2px;}"
               // 面板里的滑块比对话框里的细一档（12px），margin 相应改为 -5px 保持垂直居中
               "QSlider::handle:horizontal{width:12px;height:12px;margin:-5px 0;"
               "border-radius:6px;background:%5;}"
               // 滚动条压到 8px 极简样式：面板窄，标准宽度会挤掉内容
               "QScrollBar:vertical{background:transparent;width:8px;margin:2px 0;}"
               "QScrollBar::handle:vertical{background:rgba(127,127,127,0.35);border-radius:4px;"
               "min-height:24px;}"
               // 去掉上下箭头与翻页区，只保留可拖动的手柄
               "QScrollBar::add-line,QScrollBar::sub-line{height:0;}"
               "QScrollBar::add-page,QScrollBar::sub-page{background:transparent;}")
        .arg(text, sub, field, border, accent, sel);
}

/**
 * 创建「主按钮」（实心强调色）。
 * @param text   按钮文字
 * @param parent 父控件，交给 QPushButton；为空则为独立窗口（正常都传父）
 * @return 已设好 objectName="primary" 的按钮指针；样式由 dialogCss() 的 QPushButton#primary 提供，
 *         因此**必须先套上 dialogCss 才看得出效果**，否则就是个普通按钮。
 * 最小宽 88px 是让「创建 / 完成」这类两字按钮看起来不会太瘦。
 */
inline QPushButton *primaryButton(const QString &text, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setObjectName(QStringLiteral("primary"));
    b->setCursor(Qt::PointingHandCursor);
    b->setMinimumWidth(88);
    return b;
}

/**
 * 创建「幽灵按钮」（透明底、描边）。
 * @param text   按钮文字
 * @param parent 父控件
 * @return 未设 objectName 的普通按钮，走 dialogCss 的 QPushButton 默认规则（取消 / 浏览 / 次要操作用）
 */
inline QPushButton *ghostButton(const QString &text, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setCursor(Qt::PointingHandCursor);
    b->setMinimumWidth(80); // 略窄于主按钮，视觉上让主操作更突出
    return b;
}

/**
 * 一行表单：[标签][控件]，控件占满剩余宽度
 * @param parent     行容器的父控件，通常传对话框本身
 * @param label      左侧标签文字
 * @param field      右侧输入控件（本函数不改它的 sizePolicy）
 * @param labelWidth 标签固定宽度（px），默认 84——够放「不透明度」「堆叠弹出网格」这类最长标签，
 *                   统一宽度才能让各行的输入框左边缘对齐
 * @return 组装好的行；行内标签不拉伸、控件拉伸系数 1
 */
inline MRowLayout *formRow(QWidget *parent, const QString &label, QWidget *field,
                           int labelWidth = 84)
{
    auto *row = new MRowLayout(parent);
    row->SetSpacing(10);
    auto *lab = new QLabel(label, row);
    lab->setFixedWidth(labelWidth);
    lab->setFont(AppSettings::get().uiFont(13));
    row->RowAddWidget(lab, 0, Qt::AlignVCenter);
    row->RowAddWidget(field, 1, Qt::AlignVCenter);
    row->RowClearSpacers(); // 去掉尾部弹簧，让控件真正占满
    return row;
}

/**
 * 一行按钮：右对齐
 * @param parent  行容器的父控件
 * @param buttons 按钮列表，按给定顺序从左到右排列（调用方负责顺序，如「取消 / 创建」）
 * @return 组装好的行；开头插入一个水平 Expanding 的空白控件，把按钮推到右侧
 *
 * 实现注意：这里靠「插一个会膨胀的 QWidget」而不是拉伸某个按钮，所以不会改变按钮自身的宽度；
 * 最后仍要 RowClearSpacers() 清掉 RowAddWidget 补的尾部弹簧，否则右侧会多出一段无法压缩的空白。
 */
inline MRowLayout *buttonRow(QWidget *parent, const QList<QWidget *> &buttons)
{
    auto *row = new MRowLayout(parent);
    row->SetSpacing(8);
    auto *spacer = new QWidget(row);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    for (QWidget *b : buttons)
        row->RowAddWidget(b, 0, Qt::AlignVCenter);
    // 插到索引 0（按钮之前）：由它吃掉左侧全部空白，把按钮整体推到右边
    row->RowInsertWidget(0, spacer);
    row->RowClearSpacers();
    return row;
}

} // namespace UiStyle
