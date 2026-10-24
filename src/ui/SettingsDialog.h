/**
 * @file SettingsDialog.h
 * 全局设置面板（外观 / 折叠堆叠 / 新盒子默认值 / 行为）的声明。
 *
 * 关键约定 / 坑：
 *  - 本对话框没有「应用 / 取消」，任何改动都立刻写进 AppSettings 并 save()，
 *    再通过 appearanceChanged / behaviorChanged 让 App 刷新所有窗口——即「改了就生效、无法撤销」；
 *  - loadValues() 必须批量 QSignalBlocker，否则初始化时每设一个控件都会触发 pushToSettings()，
 *    导致「打开设置面板」本身就改写配置文件；
 *  - 控件索引与 AppSettings 的枚举值直接对应（除 nameLines / stackGrid 需要 ±1 或 3/5 换算），
 *    改下拉框顺序时必须同步改 pushToSettings()。
 */
#pragma once

#include "mcolumnlayout.h"

#include <QDialog>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;

/**
 * 全局外观 / 行为设置。布局使用 MColumnLayout + MRowLayout。
 * 修改即生效并写回 settings.json。
 *
 * 非模态窗口（App 只保留一个实例并在关闭后销毁），因此不存在「确定 / 取消」的暂存态：
 * 每个控件都直接对应 AppSettings 的一个字段。
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    /** @param parent 父窗口；正常传 nullptr，让设置框独立于盒子窗口的层级关系 */
    explicit SettingsDialog(QWidget *parent = nullptr);

signals:
    /** 外观类字段（主题 / 材质 / 圆角 / 透明度 / 图标尺寸 / 字体行数）已写入 AppSettings 并落盘，需要重绘所有窗口 */
    void appearanceChanged();
    /** 行为类字段（默认视图 / 排序 / 删除确认 / 吸附 / 隐藏文件）已写入并落盘；当前处理方式与外观一致 */
    void behaviorChanged();
    /**
     * 用户点了「添加」：请求创建一个组件盒子。
     * 真正的创建由 App::createWidgetBox() 完成（它要管窗口落地与落盘），本对话框只负责提需求。
     * @param kind     组件类型，取 BoxConfig::KindWeather / KindMusic / KindPlugin
     * @param pluginId 插件 id；内置组件为空串（**不能**传内置 id，否则会被当成插件去找）
     * @param title    新建组件窗口的期望标题（重名由 BoxManager 自动加后缀）
     */
    void widgetAddRequested(int kind, const QString &pluginId, const QString &title);

protected:
    /**
     * 自绘面板底色与 1px 描边。
     * 窗口是 WA_TranslucentBackground（= WS_EX_LAYERED），QSS 里 QDialog 的底色是 transparent，
     * 真正的「玻璃」是系统背板，这里只叠一层随不透明度变浓的染色 + 描边。
     * 不画的话整块是全透明的，文字会直接飘在桌面壁纸上。
     * @param e 忽略（只关心「该重画了」这个事实）
     */
    void paintEvent(QPaintEvent *e) override;
    /**
     * 每次显示前重新施加一次材质与圆角。
     * 材质/不透明度可能在对话框隐藏期间被别处改过，show 时补一次最省心；
     * 同时此刻 winId() 一定有效，平台层调用不会失败。
     * @param e 交给基类处理
     */
    void showEvent(QShowEvent *e) override;
    /**
     * 无边框窗口的缩放热区：靠 WM_NCHITTEST 手工返回 HTLEFT / HTBOTTOMRIGHT 等命中码。
     * 没有它用户就再也没法把面板拉大（系统边框已经被 FramelessWindowHint 去掉）。
     * @param eventType 事件类型，只处理 "windows_generic_MSG"
     * @param message   MSG*，重点看 WM_NCHITTEST
     * @param result    命中码输出；返回 true 表示已处理
     * @return 未处理的消息交回 QDialog 默认实现
     */
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    /**
     * 施加窗口表面：材质（亚克力 / 云母 / 纯色）+ 窗口圆角。
     * 窗口是无边框分层窗口，圆角不会被系统自动加上，必须显式要 DWM 圆角；
     * 老系统失败时才退回窗口蒙版（两者互斥，详见实现）。
     */
    void applySurface();
    /**
     * 建一个带标题的分区列，并把标题控件加进去。
     * @param title 分区标题（如「外观」「行为」），用 objectName="section" 走加粗小字样式
     * @return 新建的分区列布局（父对象为本对话框），调用方继续往里 ColumnAddWidget
     */
    MColumnLayout *makeSection(const QString &title);
    /** 搭出整个面板：标题 + 四个分区 + 完成按钮，并把每个控件的变更信号接到 pushToSettings() */
    void buildUi();
    /**
     * 刷新「组件」分区里的下拉框：重新枚举内置组件 + 已加载插件。
     * @param rescan true = 先 reloadPlugins() 重扫插件目录（按钮触发时用）；
     *               false = 只用注册表已有内容（打开面板时用，避免把正在运行的插件面板卸掉）
     */
    void reloadWidgetTypes(bool rescan = false);
    /** 「添加」按钮：把下拉框当前选中的组件请求交给 App 创建，并在下方给一行反馈文字 */
    void addSelectedWidget();
    /**
     * 把 AppSettings 的当前值灌进各控件，并同步两个滑块的数值文本。
     * 全程被 QSignalBlocker 包住，因此不会反过来触发 pushToSettings()。
     */
    void loadValues();
    /**
     * 把控件当前值写回 AppSettings（全量覆盖，不是差量）、save() 落盘，然后刷自身样式表
     * 并发出变更信号。**只给下拉框 / 勾选框这类「一次变动就完事」的控件用**；
     * 滑块走 applyLive() + 停手落盘，否则拖动时每像素写一次盘。
     */
    void pushToSettings();
    /**
     * 应用的「轻量版」：只把值写进内存并让窗口重画，**不落盘**。
     *
     * 拖滑块时 valueChanged 会连发几十次，以前每次都 save() + 重套 QSS + 全窗口刷新，
     * 拖起来就是一顿一顿的。现在合并到 33ms 一次（`m_liveTimer`），停手 350ms 后才写盘
     * （`m_saveTimer`）；QSS 也只在主题真的变了才重套。
     */
    void applyLive();

    QComboBox *m_theme = nullptr;          // 主题：索引即 AppSettings::Theme（0 深色 / 1 浅色）
    QComboBox *m_material = nullptr;       // 材质：索引即 AppSettings::MaterialMode（0 亚克力 / 1 云母 / 2 纯色）
    QSlider *m_opacity = nullptr;          // 玻璃不透明度，单位 %，界面范围 20-100（存储允许更低，这里不放开）
    QLabel *m_opacityValue = nullptr;      // 紧跟滑块右侧的数值文本（"78%"），固定 52px 宽防抖动
    QSlider *m_iconSize = nullptr;         // 图标边长，单位像素；范围 32-144，步长 4（对齐常见图标栅格）
    QLabel *m_iconSizeValue = nullptr;     // 图标尺寸数值文本（"68 px"），同上固定宽度
    QComboBox *m_nameLines = nullptr;      // 文件名行数：索引 0/1 对应存储值 1/2（写入时 +1）
    QComboBox *m_viewMode = nullptr;       // 新盒子默认查看方式：0 图标 / 1 列表 / 2 堆叠，与 AppSettings::ViewMode 同域
    QComboBox *m_sortMode = nullptr;       // 新盒子默认排序：0 手动 / 1 名称 / 2 修改时间 / 3 类型 / 4 大小
    QComboBox *m_stackGrid = nullptr;      // 堆叠弹出网格：索引 0 → 3×3，1 → 5×5（写入时换算成 int 3/5）
    QCheckBox *m_confirmDelete = nullptr;  // 删除文件前是否弹确认框（实际删除始终走回收站）
    QCheckBox *m_snap = nullptr;           // 拖动整理盒松手时是否吸附网格；步长固定用 AppSettings::gridStep，面板不提供调节
    QCheckBox *m_showHidden = nullptr;     // 文件列表是否显示隐藏 / 系统文件

    QTimer *m_liveTimer = nullptr;         // 滑块拖动期间的合并刷新（33ms 单次）
    QTimer *m_saveTimer = nullptr;         // 停手 350ms 后落盘（单次）
    int m_appliedTheme = -1;               // 已经套过 QSS 的主题；-1 = 还没套过
    int m_appliedMaterial = -1;            // 已经施加过的材质；-1 = 还没施加过。材质没变就不重复调 DWM

    // ---- 组件分区 ----
    QComboBox *m_widgetCombo = nullptr;    // 可添加的组件（内置 + 插件）；索引对应填充顺序，取值走 itemData
    QPushButton *m_widgetAdd = nullptr;    // 「添加」按钮；列表为空时禁用
    QLabel *m_widgetStatus = nullptr;      // 分区下方的反馈文字（可选组件的描述 / 刚添加了什么）
    QLabel *m_widgetDesc = nullptr;        // 当前选中组件的说明文字
};
