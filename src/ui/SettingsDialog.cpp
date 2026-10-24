/**
 * @file SettingsDialog.cpp
 * 设置面板的实现：纯代码搭 UI（无 .ui 文件），控件与 AppSettings 字段一一绑定，改动即落盘。
 *
 * 关键约定 / 坑：
 *  - buildUi() 里才连接信号，loadValues() 又在 QSignalBlocker 保护下灌初值，
 *    两步顺序不能反，否则会「打开面板即改写配置」；
 *  - 下拉框的索引与存储值并非全部等价（nameLines 要 +1、stackGrid 要 3/5 换算），
 *    增删选项时必须同时改 pushToSettings() / loadValues()；
 *  - 面板里改完只负责发信号，真正刷新窗口是 App::applyAppearanceToAll()。
 */
#include "ui/SettingsDialog.h"

#include "core/AppSettings.h"
#include "core/Paths.h"
#include "core/ShellActions.h"
#include "mrowlayout.h"
#include "platform/Autostart.h"
#include "platform/Platform.h"
#include "ui/UiStyle.h"
#include "widgets/WidgetRegistry.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>

#include <functional>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace
{
/** 自绘标题栏高度（px，逻辑像素）。44 是「能放下 14px 标题 + 28px 关闭按钮」的舒适值。 */
constexpr int kHeaderHeight = 44;
/** 窗口圆角半径（px）：与整理盒的 kRadius 保持一致，换窗口时观感连续。 */
constexpr int kRadius = 12;
/** 缩放热区宽度（逻辑像素），实际按 DPI 放大。无边框窗口没有系统边框，全靠 WM_NCHITTEST 自己判。 */
constexpr int kResizeBorder = 6;

/**
 * 设置面板的自绘标题栏：左侧标题 + 右侧关闭按钮 + 拖动整个窗口。
 *
 * 为什么不保留系统标题栏：系统标题栏是 DWM 单独绘制的一块不透明区域，颜色和下方半透明的
 * 亚克力客户区永远对不上，中间会有一道明显的分界。自绘之后标题栏和内容共用同一块玻璃，
 * 看起来是一体的。代价是拖动 / 关闭 / 缩放都要自己实现（关闭见 m_onClose，拖动见下面三个事件）。
 *
 * 没有 Q_OBJECT：它不需要信号槽，关闭动作直接调构造时传入的回调，省掉一份 moc。
 */
class SettingsHeader : public QWidget
{
public:
    /**
     * @param parent  父控件，通常是 SettingsDialog
     * @param onClose 点关闭按钮时的回调（由对话框传入 accept），为空则按钮无效
     */
    explicit SettingsHeader(QWidget *parent, std::function<void()> onClose)
        : QWidget(parent), m_onClose(std::move(onClose))
    {
        setFixedHeight(kHeaderHeight);
        setMouseTracking(true); // 关闭按钮要能收到「只是经过、没有按下」的移动事件才能做悬停态
    }

    /** 高度固定，宽度跟着窗口走；给一个最小宽度让布局别把它压成 0 */
    QSize sizeHint() const override { return QSize(260, kHeaderHeight); }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    /** 关闭按钮的命中矩形（含内边距）；绘制与命中都走它，两边不会错位 */
    QRect closeRect() const;

    std::function<void()> m_onClose; ///< 关闭回调；为空表示不可关
    bool m_dragging = false;         ///< 正在拖动窗口（按下在标题文字区域）
    QPoint m_dragBase;               ///< 按下瞬间「窗口左上角 - 全局鼠标」，move 时相加即可复原手感
    bool m_hoverClose = false;       ///< 鼠标悬停在关闭按钮上
    bool m_pressClose = false;       ///< 关闭按钮已被按下，松手时仍在按钮上才真正关闭
};

// 只画文字与关闭按钮：底色由对话框的 paintEvent 统一画（标题栏和内容是同一块玻璃）
void SettingsHeader::paintEvent(QPaintEvent *)
{
    const AppSettings &st = AppSettings::get();
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    // 标题不吃鼠标事件：它和标题栏是同一个控件，天然就能整条拖动
    p.setFont(st.uiFont(14, QFont::DemiBold));
    p.setPen(st.textColor());
    p.drawText(QRect(20, 0, width() - 20 - kHeaderHeight - 10, height()),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("设置"));

    // 关闭按钮：悬停/按下给一层浅底
    const QRect r = closeRect();
    if (m_hoverClose || m_pressClose) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_pressClose ? st.hoverColor().darker(112) : st.hoverColor());
        p.drawRoundedRect(r, 7, 7);
    }

    // × 用两条线画而不是用字符「✕」：字体里那个符号的字重/居中随字体变，线条可控
    p.setPen(QPen(m_hoverClose ? st.textColor() : st.subTextColor(), 1.4));
    const QPointF c = QRectF(r).center();
    const qreal d = 5.0; // × 的半边长（px）
    p.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
    p.drawLine(QPointF(c.x() - d, c.y() + d), QPointF(c.x() + d, c.y() - d));
}

QRect SettingsHeader::closeRect() const
{
    constexpr int kSize = 28; // 按钮边长：比标题栏矮 16px，四周留出呼吸空间
    return QRect(width() - kSize - 10, (height() - kSize) / 2, kSize, kSize);
}

void SettingsHeader::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    if (closeRect().contains(e->position().toPoint())) {
        m_pressClose = true;
        update();
        return;
    }
    // 记录差值而不是绝对坐标：拖动过程中鼠标可能瞬移，差值法不会让窗口「跳」一下
    m_dragging = true;
    m_dragBase = window()->pos() - e->globalPosition().toPoint();
}

void SettingsHeader::mouseMoveEvent(QMouseEvent *e)
{
    const bool hover = closeRect().contains(e->position().toPoint());
    if (hover != m_hoverClose) {
        m_hoverClose = hover;
        update();
    }
    // 只在左键按住时才挪窗：否则单纯移动鼠标会把窗口一路拖走
    if (m_dragging && (e->buttons() & Qt::LeftButton))
        window()->move(e->globalPosition().toPoint() + m_dragBase);
    QWidget::mouseMoveEvent(e);
}

void SettingsHeader::mouseReleaseEvent(QMouseEvent *e)
{
    const bool onClose = closeRect().contains(e->position().toPoint());
    m_dragging = false;
    if (m_pressClose) {
        m_pressClose = false;
        update();
        // 按下后拖出按钮再松手 = 取消，和系统标题栏的行为一致
        if (onClose && m_onClose)
            m_onClose();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void SettingsHeader::leaveEvent(QEvent *e)
{
    if (m_hoverClose) {
        m_hoverClose = false;
        update();
    }
    QWidget::leaveEvent(e);
}

/**
 * 把「滑块 + 数值」组装成一行：滑块吃掉剩余宽度，数值贴右。
 * @param slider 由调用方创建并已设好范围的滑块（本函数不改范围）
 * @param value  数值标签；这些控件都在 SettingsDialog 里持有指针，需要在变化时更新文本
 * @param parent 行容器的父控件，通常传对话框本身
 * @return 装好的 MRowLayout，可直接塞进分区列
 * 标签宽 52px：容得下「100%」与「144 px」，固定宽度避免滑块随数值位数抖动。
 */
QWidget *sliderField(QSlider *slider, QLabel *value, QWidget *parent)
{
    auto *row = new MRowLayout(parent);
    row->SetSpacing(10);
    value->setFixedWidth(52); // 固定宽度：否则文本从 "9%" 变 "100%" 时滑块会被挤动
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->RowAddWidget(slider, 1, Qt::AlignVCenter);
    row->RowAddWidget(value, 0, Qt::AlignVCenter);
    row->RowClearSpacers(); // RowAddWidget 会在尾部补弹簧，不清掉滑块就占不满整行
    return row;
}
} // namespace

// 构造只做四件事：定窗口表面、套样式、定最小尺寸、建 UI 并灌初值。
// buildUi() 必须在 loadValues() 之前——先有控件才能灌值
SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("整理盒设置"));
    setModal(false); // 非模态：用户能一边调设置一边看盒子的实时变化

    // 无边框：标题栏由 SettingsHeader 自绘，才能和内容共用同一块玻璃。
    // 保留系统标题栏的话，那块是 DWM 单独画的不透明区域，和下方半透明的客户区永远对不上。
    // 代价：拖动 / 缩放 / 关闭都得自己实现（拖动与关闭在 SettingsHeader，缩放在 nativeEvent）。
    setWindowFlag(Qt::FramelessWindowHint, true);

    // 玻璃底：与整理盒用同一套「分层窗口 + 系统背板」方案。
    // WA_TranslucentBackground 会把窗口变成 WS_EX_LAYERED 且客户区默认全透明，
    // 所以底色必须由 paintEvent 自己画（QSS 里 QDialog 的底色已改成 transparent），
    // 否则整块透明、文字直接糊在壁纸上。
    // 注意这条属性必须在窗口首次 show（真正创建原生窗口）之前设置才有效。
    setAttribute(Qt::WA_TranslucentBackground, true);

    UiStyle::styleDialog(this, /*translucent=*/true);
    // 最小 520×700：本面板没有套 QScrollArea，再小内容就会被挤压
    setMinimumSize(520, 700);

    // 滑块专用：拖动期间把连发的变更合并成 33ms 一次（约 30fps）的应用，
    // 停手 350ms 后才真正落盘。两个定时器都是单次，每次变更重新压满。
    m_liveTimer = new QTimer(this);
    m_liveTimer->setSingleShot(true);
    m_liveTimer->setInterval(33);
    connect(m_liveTimer, &QTimer::timeout, this, [this] { applyLive(); });

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(350);
    connect(m_saveTimer, &QTimer::timeout, this, [] { AppSettings::get().save(); });

    buildUi();
    loadValues();
    reloadWidgetTypes();             // 组件下拉框：用注册表缓存，首次调用会懒扫描一次插件目录
    applySurface(); // 灌完值再施加材质：tint 与圆角都依赖当前设置
}

// 分区标题：统一 objectName="section"，由 dialogCss 里的 QLabel#section 规则负责字号与加粗
MColumnLayout *SettingsDialog::makeSection(const QString &title)
{
    const AppSettings &st = AppSettings::get();
    auto *col = new MColumnLayout(this);
    col->SetSpacing(8);
    auto *label = new QLabel(title, col);
    label->setObjectName(QStringLiteral("section"));
    // 12px DemiBold：比正文小一档但更重，形成「组标签」的视觉层级
    label->setFont(st.uiFont(12, QFont::DemiBold));
    col->ColumnAddWidget(label);
    return col;
}

// 纯代码搭 UI。分区顺序 = 用户心智顺序：外观 → 折叠堆叠 → 新盒子默认值 → 行为，
// 越靠前越常改。所有控件都以 this 为父，生命周期跟对话框一致。
void SettingsDialog::buildUi()
{
    const AppSettings &st = AppSettings::get();

    // 外层列铺满整个对话框（ColumnFillHost 会把 this 的布局接管成零边距包装布局）：
    // 只有它贴着窗口边缘，负责纵向排「标题栏 + 内容」两段
    auto *outer = new MColumnLayout(this);
    outer->ColumnFillHost(this);
    outer->SetMargin(0);
    outer->SetSpacing(0);

    // 自绘标题栏：和内容共用同一块玻璃，中间没有系统标题栏那道分界
    outer->ColumnAddWidget(new SettingsHeader(this, [this] { accept(); }));

    // 内容列单独一层：这样标题栏能左右通到窗口边，内容又保留 20px 边距
    auto *root = new MColumnLayout(this);
    root->SetMargin(20, 6, 20, 18); // 上边距小一点：标题栏已经给过呼吸空间
    root->SetSpacing(14);           // 分区间距比分区内的 8px 大一档，形成分组感
    outer->ColumnAddWidget(root, 1);

    // ---------------- 材质 ----------------
    MColumnLayout *appearance = makeSection(QStringLiteral("外观"));

    m_theme = new UiStyle::StyleComboBox(this);
    // 索引即 AppSettings::Theme，顺序不能改（pushToSettings 直接取 currentIndex）
    m_theme->addItems({ QStringLiteral("深色玻璃"), QStringLiteral("浅色玻璃") });
    appearance->ColumnAddWidget(UiStyle::formRow(this, QStringLiteral("主题"), m_theme));

    m_material = new UiStyle::StyleComboBox(this);
    // 索引即 AppSettings::MaterialMode：0 亚克力 / 1 云母 / 2 纯色
    m_material->addItems({ QStringLiteral("亚克力（Acrylic）"),
                           QStringLiteral("云母（Mica，Win11 22H2+）"),
                           QStringLiteral("纯色半透明") });
    appearance->ColumnAddWidget(UiStyle::formRow(this, QStringLiteral("材质"), m_material));

    // 老系统上即便选「云母」也只会得到亚克力效果，这种情况才提示；
    // 云母 / 亚克力的观感差异说明不占版面（下拉框里的名字已经够用了）
    if (!Platform::supportsMica()) {
        auto *materialHint = new QLabel(this);
        materialHint->setObjectName(QStringLiteral("hint"));
        materialHint->setFont(st.uiFont(11));
        materialHint->setWordWrap(true);
        materialHint->setText(QStringLiteral("当前系统低于 Windows 11 22H2，云母会自动按亚克力渲染。"));
        appearance->ColumnAddWidget(materialHint);
    }

    m_opacity = new QSlider(Qt::Horizontal, this);
    // 滑杆下限 20：再低盒子就几乎看不见了，属误操作区间（存储层允许到 10，这里故意不放开）
    m_opacity->setRange(20, 100);
    m_opacityValue = new QLabel(this);
    m_opacityValue->setObjectName(QStringLiteral("hint"));
    appearance->ColumnAddWidget(
        UiStyle::formRow(this, QStringLiteral("不透明度"), sliderField(m_opacity, m_opacityValue, this)));

    m_iconSize = new QSlider(Qt::Horizontal, this);
    // 32-144 px：32 是列表视图下仍可辨认的下限，144 是常见 2K/4K 桌面的上限。
    // 步长 4 让拖动时不会停在 67 这种奇怪数值上
    m_iconSize->setRange(32, 144);
    m_iconSize->setSingleStep(4);
    m_iconSizeValue = new QLabel(this);
    m_iconSizeValue->setObjectName(QStringLiteral("hint"));
    appearance->ColumnAddWidget(
        UiStyle::formRow(this, QStringLiteral("图标大小"), sliderField(m_iconSize, m_iconSizeValue, this)));

    m_nameLines = new UiStyle::StyleComboBox(this);
    // 注意：这里索引 0/1 对应存储值 1/2，写入时要 +1
    m_nameLines->addItems({ QStringLiteral("单行"), QStringLiteral("两行") });
    appearance->ColumnAddWidget(UiStyle::formRow(this, QStringLiteral("文件名"), m_nameLines));

    root->ColumnAddWidget(appearance);

    // ---------------- 组件 ----------------
    // 组件 / 插件的添加入口**只**放在这里：右键菜单里挂一长串「新建组件」子项，
    // 装几个插件之后菜单就长得放不下了。菜单里只留「新建整理盒」。
    MColumnLayout *widgets = makeSection(QStringLiteral("组件"));

    m_widgetCombo = new UiStyle::StyleComboBox(this);
    m_widgetAdd = UiStyle::primaryButton(QStringLiteral("添加"), this);
    m_widgetAdd->setMinimumWidth(72); // 比标准主按钮窄一档：它跟在输入框后面，太宽会挤掉下拉框

    {
        auto *row = new MRowLayout(this);
        row->SetSpacing(10);
        auto *lab = new QLabel(QStringLiteral("添加组件"), row);
        lab->setFixedWidth(84); // 与 formRow 的标签宽度对齐
        lab->setFont(st.uiFont(13));
        row->RowAddWidget(lab, 0, Qt::AlignVCenter);
        row->RowAddWidget(m_widgetCombo, 1, Qt::AlignVCenter);
        row->RowAddWidget(m_widgetAdd, 0, Qt::AlignVCenter);
        row->RowClearSpacers(); // 去掉尾部弹簧，下拉框才能真正吃掉剩余宽度
        widgets->ColumnAddWidget(row);
    }

    // 选中组件的说明不单独占一行（以前是常驻两行文字，纯占高度）：
    // 改成下拉框自己的 tooltip，鼠标停上去就能看到，版面不受影响（见 reloadWidgetTypes）

    {
        auto *tools = new MRowLayout(this);
        tools->SetSpacing(8);
        tools->SetMargin(94, 0, 0, 0); // 缩进到与下拉框左边缘对齐（84 标签 + 10 间距）
        auto *rescan = UiStyle::ghostButton(QStringLiteral("重新扫描插件"), this);
        auto *openDir = UiStyle::ghostButton(QStringLiteral("打开插件目录"), this);
        tools->RowAddWidget(rescan, 0, Qt::AlignVCenter);
        tools->RowAddWidget(openDir, 0, Qt::AlignVCenter);
        // 不调 RowClearSpacers：保留自动补的尾部弹簧，两个按钮就靠左排
        widgets->ColumnAddWidget(tools);

        connect(rescan, &QPushButton::clicked, this, [this] { reloadWidgetTypes(true); });
        connect(openDir, &QPushButton::clicked, this, []
        {
            // 目录可能还不存在（新装、从没放过插件），先建出来再打开
            Paths::ensureDir(WidgetRegistry::pluginsDir());
            ShellActions::openFolder(WidgetRegistry::pluginsDir());
        });
    }

    m_widgetStatus = new QLabel(this);
    m_widgetStatus->setObjectName(QStringLiteral("hint"));
    m_widgetStatus->setFont(st.uiFont(11));
    widgets->ColumnAddWidget(m_widgetStatus);

    root->ColumnAddWidget(widgets);

    // ---------------- 折叠与堆叠 ----------------
    MColumnLayout *parts = makeSection(QStringLiteral("折叠 / 堆叠"));

    m_stackGrid = new UiStyle::StyleComboBox(this);
    // 索引 → 存储值 3 / 5，不是 1:1（见 pushToSettings / loadValues）
    m_stackGrid->addItems({ QStringLiteral("3 × 3"), QStringLiteral("5 × 5") });
    parts->ColumnAddWidget(UiStyle::formRow(this, QStringLiteral("堆叠弹出网格"), m_stackGrid));

    root->ColumnAddWidget(parts);

    // ---------------- 视图默认值 ----------------
    MColumnLayout *defaults = makeSection(QStringLiteral("新整理盒默认"));

    m_viewMode = new UiStyle::StyleComboBox(this);
    // 顺序即 AppSettings::ViewMode：0 图标 / 1 列表 / 2 堆叠
    m_viewMode->addItems({ QStringLiteral("图标网格"), QStringLiteral("列表"),
                           QStringLiteral("堆叠") });
    defaults->ColumnAddWidget(UiStyle::formRow(this, QStringLiteral("查看方式"), m_viewMode));

    m_sortMode = new UiStyle::StyleComboBox(this);
    // 顺序即 AppSettings::SortMode：0 手动 / 1 名称 / 2 修改时间 / 3 类型 / 4 大小
    m_sortMode->addItems({ QStringLiteral("手动（拖动排序）"), QStringLiteral("名称"),
                           QStringLiteral("修改时间"), QStringLiteral("类型"),
                           QStringLiteral("大小") });
    defaults->ColumnAddWidget(UiStyle::formRow(this, QStringLiteral("排序方式"), m_sortMode));

    root->ColumnAddWidget(defaults);

    // ---------------- 行为 ----------------
    MColumnLayout *behavior = makeSection(QStringLiteral("行为"));

    // 三个开关都只影响「之后的操作」，不改动已存在文件的任何状态
    m_confirmDelete = new QCheckBox(QStringLiteral("删除文件前询问（默认移到回收站）"), this);
    m_snap = new QCheckBox(QStringLiteral("拖动整理盒时吸附到网格"), this);
    m_showHidden = new QCheckBox(QStringLiteral("显示隐藏 / 系统文件"), this);
    behavior->ColumnAddWidget(m_confirmDelete);
    behavior->ColumnAddWidget(m_snap);
    behavior->ColumnAddWidget(m_showHidden);

    // 开机自启：唯一一个「写系统、不写 settings.json」的开关。
    // 说明文字不再常驻（只当错误提示位用），平时这一行就只有开关本身
    m_autoStart = new QCheckBox(QStringLiteral("开机自启动"), this);
    behavior->ColumnAddWidget(m_autoStart);

    m_autoStartHint = new QLabel(this);
    m_autoStartHint->setObjectName(QStringLiteral("hint"));
    m_autoStartHint->setFont(st.uiFont(11));
    m_autoStartHint->setWordWrap(true);
    // 隐藏时布局会完全跳过它，所以「没有错误」时它不占一像素
    m_autoStartHint->setVisible(false);
    if (!Autostart::isSupported()) {
        // 非 Windows 构建：开关置灰而不是隐藏，位置稳定、也解释了为什么点不动
        m_autoStart->setEnabled(false);
        m_autoStart->setToolTip(QStringLiteral("当前平台没有实现开机自启"));
        m_autoStartHint->setText(QStringLiteral("当前平台不支持开机自启（仅 Windows 实现）。"));
        m_autoStartHint->setVisible(true);
    }
    {
        // 用一行容器做出 24px 缩进（勾选框方块 + spacing 的宽度），
        // 让这段文字明显从属于上面那个开关，而不是一个新条目
        auto *row = new MRowLayout(this);
        row->SetMargin(24, 0, 0, 0);
        row->RowAddWidget(m_autoStartHint, 1, Qt::AlignTop);
        row->RowClearSpacers(); // 不清掉尾部弹簧，说明文字只有一半宽就换行了
        behavior->ColumnAddWidget(row);
    }

    root->ColumnAddWidget(behavior);

    // 弹簧把按钮压到底部：窗口被拉高时内容不会被拉开
    root->ColumnAddStretch(1);

    auto *close = UiStyle::primaryButton(QStringLiteral("完成"), this);
    root->ColumnAddWidget(UiStyle::buttonRow(this, { close }));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    // ---- 变更即生效 ----
    // 下拉框用 currentIndexChanged、勾选框用 toggled、滑块用 valueChanged，
    // 统一忽略信号参数后转调 pushToSettings()（真正取值的动作在那边统一做）
    connect(m_theme, &QComboBox::currentIndexChanged, this, [this](int) { pushToSettings(); });
    connect(m_material, &QComboBox::currentIndexChanged, this, [this](int) { pushToSettings(); });
    // 滑块多一步：先刷新右侧数值文本，再合并刷新。
    // 不直接 pushToSettings() —— 拖动时 valueChanged 每秒几十次，
    // 每次都落盘 + 重套样式表 + 全窗口重画，拖起来就是一顿一顿的。
    connect(m_opacity, &QSlider::valueChanged, this, [this](int v)
    {
        m_opacityValue->setText(QStringLiteral("%1%").arg(v));
        m_liveTimer->start();
        m_saveTimer->start();
    });
    connect(m_iconSize, &QSlider::valueChanged, this, [this](int v)
    {
        m_iconSizeValue->setText(QStringLiteral("%1 px").arg(v));
        m_liveTimer->start();
        m_saveTimer->start();
    });
    connect(m_nameLines, &QComboBox::currentIndexChanged, this, [this](int) { pushToSettings(); });
    connect(m_viewMode, &QComboBox::currentIndexChanged, this, [this](int) { pushToSettings(); });
    connect(m_sortMode, &QComboBox::currentIndexChanged, this, [this](int) { pushToSettings(); });
    connect(m_stackGrid, &QComboBox::currentIndexChanged, this, [this](int) { pushToSettings(); });
    connect(m_confirmDelete, &QCheckBox::toggled, this, [this](bool) { pushToSettings(); });
    connect(m_snap, &QCheckBox::toggled, this, [this](bool) { pushToSettings(); });
    connect(m_showHidden, &QCheckBox::toggled, this, [this](bool) { pushToSettings(); });
    // 自启是个「立即产生系统副作用」的开关，不能进 pushToSettings()：
    // 那里是 AppSettings 的全量回写，跟注册表没有关系，写多少次都不会改变启动项
    connect(m_autoStart, &QCheckBox::toggled, this, [this](bool on)
    {
        if (Autostart::setEnabled(on)) {
            m_autoStartHint->setVisible(false); // 成功就把可能的旧错误提示收起来
            return;
        }
        // 失败（无权限 / 系统拒绝）：把勾恢复成真实状态，界面不能撒谎。
        // QSignalBlocker 必须加，否则 setChecked 会再触发一次本 lambda，来回抖动
        {
            const QSignalBlocker block(m_autoStart);
            m_autoStart->setChecked(Autostart::isEnabled());
        }
        m_autoStartHint->setText(QStringLiteral("设置失败：没能写入注册表启动项（可能被安全软件拦截），"
                                                "开机自启保持不变。"));
        m_autoStartHint->setVisible(true);
    });

    // ---- 组件分区 ----
    connect(m_widgetAdd, &QPushButton::clicked, this, &SettingsDialog::addSelectedWidget);
    connect(m_widgetCombo, &QComboBox::currentIndexChanged, this, [this](int idx)
    {
        // 说明文字挂在 tooltip 角色上，切选项时同步到下拉框自身的 tooltip
        m_widgetCombo->setToolTip(idx >= 0 ? m_widgetCombo->itemData(idx, Qt::ToolTipRole).toString()
                                           : QString());
    });
}

/**
 * 刷新组件下拉框。
 *
 * 打开面板时**不重扫**（rescan=false）：reloadPlugins() 会卸载所有已加载的插件，
 * 正在显示的插件组件面板会跟着失效。用户新拷了 DLL 进来，点「重新扫描插件」即可。
 */
void SettingsDialog::reloadWidgetTypes(bool rescan)
{
    if (rescan)
        WidgetRegistry::reloadPlugins();

    const int keep = m_widgetCombo->currentIndex();
    m_widgetCombo->blockSignals(true);
    m_widgetCombo->clear();
    for (const WidgetTypeInfo &t : WidgetRegistry::types()) {
        const int idx = m_widgetCombo->count();
        // 插件加后缀，和内置组件区分开
        m_widgetCombo->addItem(t.plugin ? QStringLiteral("%1（插件）").arg(t.name) : t.name);
        // 三个角色：kind / pluginId（内置项必须是空串，否则会被当成插件去找）/ 干净的名字
        m_widgetCombo->setItemData(idx, t.kind, Qt::UserRole);
        m_widgetCombo->setItemData(idx, t.plugin ? t.id : QString(), Qt::UserRole + 1);
        m_widgetCombo->setItemData(idx, t.name, Qt::UserRole + 2);
        m_widgetCombo->setItemData(idx, t.description.isEmpty() ? t.name : t.description,
                                   Qt::ToolTipRole);
    }
    m_widgetCombo->blockSignals(false);

    const int n = m_widgetCombo->count();
    // 重扫后尽量停在原来选的那一项，找不到就回第 0 项
    m_widgetCombo->setCurrentIndex(n == 0 ? -1 : qBound(0, keep, n - 1));
    // setCurrentIndex 在 blockSignals 之后仍可能因为索引没变而不发信号，这里直接同步一次 tooltip，
    // 保证「打开面板」时就带着当前组件的说明
    m_widgetCombo->setToolTip(n > 0 ? m_widgetCombo->itemData(m_widgetCombo->currentIndex(),
                                                              Qt::ToolTipRole).toString()
                                    : QString());
    m_widgetAdd->setEnabled(n > 0);
    m_widgetStatus->setText(rescan
                                ? QStringLiteral("已重新扫描：%1 个可用组件（已在运行的插件面板需要重新添加）")
                                      .arg(n)
                                : QStringLiteral("共 %1 个可用组件").arg(n));
}

/** 「添加」：只发请求（真正建窗口的是 App），面板不关，方便连着加几个。 */
void SettingsDialog::addSelectedWidget()
{
    const int idx = m_widgetCombo->currentIndex();
    if (idx < 0)
        return;
    const int kind = m_widgetCombo->itemData(idx, Qt::UserRole).toInt();
    const QString pluginId = m_widgetCombo->itemData(idx, Qt::UserRole + 1).toString();
    const QString name = m_widgetCombo->itemData(idx, Qt::UserRole + 2).toString();

    emit widgetAddRequested(kind, pluginId, name);
    // 给一行反馈，否则点完没有任何动静，用户会以为没生效
    m_widgetStatus->setText(QStringLiteral("已添加「%1」，可以把它拖到想要的位置").arg(name));
}

// 灌初值。12 个 QSignalBlocker 覆盖除两个数值标签之外的全部控件：
// 少了任何一个，setCurrentIndex / setValue 都会触发 pushToSettings()，
// 于是「打开设置面板」这个动作本身就会重写 settings.json 并刷新所有窗口。
void SettingsDialog::loadValues()
{
    const AppSettings &st = AppSettings::get();
    QSignalBlocker b1(m_theme), b2(m_material), b3(m_opacity), b4(m_iconSize),
        b5(m_nameLines), b6(m_viewMode), b7(m_sortMode), b8(m_stackGrid),
        b9(m_confirmDelete), b10(m_snap), b11(m_showHidden), b12(m_autoStart);

    m_theme->setCurrentIndex(st.theme);
    m_material->setCurrentIndex(st.material);
    m_opacity->setValue(st.opacity);
    // 数值文本要单独设：valueChanged 被阻断了，滑块自己不会去更新标签
    m_opacityValue->setText(QStringLiteral("%1%").arg(st.opacity));
    m_iconSize->setValue(st.iconSize);
    m_iconSizeValue->setText(QStringLiteral("%1 px").arg(st.iconSize));
    // 存储值是 1/2，索引从 0 起，故 -1
    m_nameLines->setCurrentIndex(st.nameLines - 1);
    m_viewMode->setCurrentIndex(st.defaultViewMode);
    m_sortMode->setCurrentIndex(st.defaultSortMode);
    // 只有 5 对应索引 1，其余值（含被 load 归一化的非法值）一律落到 3×3
    m_stackGrid->setCurrentIndex(st.stackGrid == 5 ? 1 : 0);
    m_confirmDelete->setChecked(st.confirmDelete);
    m_snap->setChecked(st.snapToGrid);
    m_showHidden->setChecked(st.showHidden);
    // 自启不在 settings.json 里，真值从注册表读：用户可能在任务管理器里直接关过
    m_autoStart->setChecked(Autostart::isEnabled());
}

// 离散控件（下拉框 / 勾选框）的入口：应用 + 落盘 + 补一个行为信号。
// 滑块不走这里，见 applyLive() 上面的说明。
void SettingsDialog::pushToSettings()
{
    applyLive();
    AppSettings::get().save();
    emit behaviorChanged();
}

// 全量回写「轻量版」：只写内存 + 让窗口重画，不落盘、不重套 QSS（除非主题真的变了）。
// 全量覆盖而不是差量：不需要记住哪些字段被改过，字段也就十几个。
// **开机自启不在这里**：它写的是注册表启动项，由 m_autoStart 的 toggled 单独处理（见 buildUi）。
void SettingsDialog::applyLive()
{
    AppSettings &st = AppSettings::get();
    st.theme = m_theme->currentIndex();
    st.material = m_material->currentIndex();
    st.opacity = m_opacity->value();
    st.iconSize = m_iconSize->value();
    st.nameLines = m_nameLines->currentIndex() + 1;      // 索引 0/1 → 存储 1/2
    st.defaultViewMode = m_viewMode->currentIndex();
    st.defaultSortMode = m_sortMode->currentIndex();
    st.stackGrid = m_stackGrid->currentIndex() == 1 ? 5 : 3; // 索引 → 3 / 5
    st.confirmDelete = m_confirmDelete->isChecked();
    st.snapToGrid = m_snap->isChecked();
    st.showHidden = m_showHidden->isChecked();

    // 主题没变就不重套：重新解析整张 QSS + 全量子控件重绘，是拖滑块时最贵的一步
    const bool themeChanged = (st.theme != m_appliedTheme);
    if (themeChanged) {
        m_appliedTheme = st.theme;
        setStyleSheet(UiStyle::dialogCss(/*translucent=*/true));
    }

    // 材质或主题变了要重打一次系统背板：主题决定标题栏/边框的明暗，材质决定模糊方式；
    // 不透明度只影响 paintEvent 的染色，update() 就够
    if (st.material != m_appliedMaterial || themeChanged)
        applySurface();
    update();

    // 只发一个：App 把两个信号接到同一个槽，都发等于把所有窗口的外观刷两遍
    emit appearanceChanged();
}

// 面板底色 + 描边：真正的模糊/背板由平台层负责，这里只叠染色。
// 公式与 FileBoxWindow::paintEvent 完全一致，否则设置面板和旁边的整理盒会有明显色差。
void SettingsDialog::paintEvent(QPaintEvent *)
{
    const AppSettings &st = AppSettings::get();
    const auto material = static_cast<Platform::Material>(st.material);

    // 材质决定底色浓淡：云母靠 DWM 背板，底色要更薄；纯色没有背板，需要更实的底色
    double scale = 1.0;
    if (material == Platform::Material::Solid)
        scale = 1.12;
    else if (material == Platform::Material::Mica)
        scale = 0.5;

    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    // 半像素内缩：1px 描边压在半像素上才不会被抗锯齿糊成 2px 灰边
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    // 透明度按百分比映射到 alpha，并在对话框上抬一档下限：
    // 面板里全是小字，和「图标 + 文件名」的盒子不同，太透会看不清
    const int alpha = qBound(0, qRound(st.opacity / 100.0 * 235.0 * scale), 255);
    QColor tint = st.isDark() ? QColor(22, 23, 29, qMax(alpha, 150))
                              : QColor(250, 251, 254, qMax(alpha, 158));

    // **只画直角**：圆角是窗口自己的事（DWM 裁切 / 窗口蒙版，见 applySurface）。
    // 这里再画一层自绘圆角就会和窗口圆角半径对不上，四角出现「双层圆角 + 一圈缝」。
    p.fillRect(r, tint);
    p.setPen(QPen(st.borderColor(), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
}

// 显示前补一次材质：对话框隐藏期间可能被别的地方改过设置，此刻 winId() 也一定有效
void SettingsDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    if (m_appliedTheme != AppSettings::get().theme) {
        m_appliedTheme = AppSettings::get().theme;
        setStyleSheet(UiStyle::dialogCss(/*translucent=*/true));
    }
    applySurface();
}

// 施加材质与窗口圆角。与 FileBoxWindow::applySurface() 同源。
void SettingsDialog::applySurface()
{
    const AppSettings &st = AppSettings::get();
    const auto material = static_cast<Platform::Material>(st.material);

    // tint 只是叠在材质上的薄薄一层色偏；云母本身有背板，alpha 要更小才不糊
    QColor tint = st.isDark() ? QColor(14, 16, 22, 24) : QColor(250, 250, 254, 26);
    if (material == Platform::Material::Mica)
        tint.setAlpha(st.isDark() ? 18 : 24);

    Platform::applyMaterial(this, material, tint);

    // 圆角只由窗口负责：优先让 DWM 裁（无边框窗口不会自动圆角，必须显式要求），
    // Win10 或调用失败时退回窗口蒙版。**两者只能有一个生效** ——
    // 蒙版留着会盖掉 DWM 圆角的抗锯齿边缘，出现一圈硬边。
    if (Platform::setRoundedCorners(this, true))
        Platform::clearMask(this);
    else
        Platform::applyRoundedMask(this, kRadius);

#ifdef Q_OS_WIN
    // 无边框窗口默认没有 WS_THICKFRAME。此时 nativeEvent 虽然返回了 HTRIGHT / HTBOTTOMRIGHT，
    // 系统也不会进入缩放循环（实测：拖右边缘纹丝不动）。补上这个样式位才能真正拉动边缘缩放；
    // DWM 合成下它不会画出那个 3D 立体边框，只是让系统认账。
    HWND hwnd = reinterpret_cast<HWND>(winId());
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & WS_THICKFRAME))
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME);
#endif

    m_appliedMaterial = st.material;
    update();
}

// 无边框窗口没有系统边框，缩放热区只能靠 WM_NCHITTEST 自己判（和整理盒同一套做法）。
// 只给 6px：设置面板的内容有 20px 边距，不会误触。
bool SettingsDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType == QByteArrayLiteral("windows_generic_MSG")) {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message == WM_NCHITTEST) {
            // lParam 里是屏幕坐标，用 short 截断才能还原负坐标（副屏在主屏左侧时会出现）
            POINT pt;
            pt.x = static_cast<short>(LOWORD(msg->lParam));
            pt.y = static_cast<short>(HIWORD(msg->lParam));
            RECT wr;
            GetWindowRect(reinterpret_cast<HWND>(winId()), &wr);

            // 按 DPI 放大热区宽度，否则 150% 缩放下会窄得点不中
            const int b = qMax(kResizeBorder, qRound(kResizeBorder * devicePixelRatioF()));
            // 角给两倍宽度：斜拉要「同时贴住两个方向」才命中，只给 b×b 基本点不中
            const int c = b * 2;

            const bool nearLeft   = pt.x >= wr.left && pt.x < wr.left + c;
            const bool nearRight  = pt.x <= wr.right && pt.x > wr.right - c;
            const bool nearTop    = pt.y >= wr.top && pt.y < wr.top + c;
            const bool nearBottom = pt.y <= wr.bottom && pt.y > wr.bottom - c;

            const bool leftEdge   = pt.x >= wr.left && pt.x < wr.left + b;
            const bool rightEdge  = pt.x <= wr.right && pt.x > wr.right - b;
            const bool topEdge    = pt.y >= wr.top && pt.y < wr.top + b;
            const bool bottomEdge = pt.y <= wr.bottom && pt.y > wr.bottom - b;

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
                return false;   // 不是边框：交回默认处理，让控件正常接收鼠标
            return true;
        }
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
