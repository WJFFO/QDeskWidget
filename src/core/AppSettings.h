#pragma once

#include <QColor>
#include <QFont>
#include <QStringList>

/**
 * @file AppSettings.h
 * 全局设置单例：外观（主题 / 材质 / 圆角 / 不透明度 / 字体）与行为（吸附 / 排序 / 隐藏文件）。
 *
 * 关键约定：它是一个「可写的全局变量集合」，各处直接写 AppSettings::get().xxx，
 * 没有 setter 也没有变更信号——改完要显式 save()，且不会自动通知已存在的窗口重绘。
 * 所有数值字段在 load() 里会被 qBound 夹到合法区间，代码里因此可以假设取值合法。
 */

/**
 * 全局设置：外观 / 行为。持久化到 settings.json。
 */
struct AppSettings
{
    /// 主题，load 时夹到 0..1
    enum Theme { Dark = 0, Light = 1 };
    /// 视图模式；与 BoxConfig::effectiveViewMode() 返回的值同域
    enum ViewMode { IconView = 0, ListView = 1, StackView = 2 };
    /// 排序方式；0 = 手动，此时才用得到 BoxConfig::manualOrder
    enum SortMode { SortManual = 0, SortName = 1, SortDate = 2, SortType = 3, SortSize = 4 };
    /// 窗口背景材质，直接 static_cast 给 Platform::Material，故枚举值必须与其顺序一致
    enum MaterialMode { Acrylic = 0, Mica = 1, Solid = 2 };

    int theme = Dark;          // 0 深色 / 1 浅色，越界值在 load 时夹回
    int material = Acrylic;   // 亚克力 / 云母 / 纯色
    bool systemCorners = true; // 已废弃：设置界面的开关已删掉，圆角恒走 DWM（拿不到才退窗口蒙版）；
                               // 字段保留只为不破坏旧 settings.json 的读写（读进来也没人用）
    int opacity = 78;          // 玻璃底色不透明度（0-100），实际生效范围是 10-100
    int iconSize = 68;         // 默认图标尺寸，像素；实际生效范围 32-160
    int nameLines = 2;         // 文件名行数 1/2
    int defaultViewMode = IconView;   // 盒子未单独指定 viewMode 时用它
    int defaultSortMode = SortManual; // 盒子未单独指定 sortMode 时用它
    bool confirmDelete = true;        // 删除文件前弹确认（实际删除走回收站）
    bool snapToGrid = true;           // 松手时是否把窗口左上角四舍五入对齐到 gridStep 的整数倍
    int gridStep = 16;                // 吸附网格步长，像素；实际生效范围 2-64
    bool showHidden = false;          // 是否显示隐藏 / 系统文件
    bool capsuleCollapsed = true; // 已废弃：胶囊模式早已移除，设置界面里的开关也已删掉；
                                  // 字段保留只为不破坏旧 settings.json 的读写（读进来也没人用）
    int stackGrid = 3;            // 堆叠弹窗网格 3 或 5；load 时非 5 一律归 3

    // ---- 小组件 ----
    QString weatherCity;          // 天气组件默认城市
    QString musicAppId;           // 音乐组件优先显示的播放器（空 = 跟随系统当前会话）
    int widgetRefreshMin = 1;     // 组件自动刷新间隔（分钟），0 关闭，上限 60

    /**
     * 取全局唯一实例（函数内 static，首次调用时才 load() 磁盘）。
     * 返回值可写：AppSettings::get().opacity = 90 是合法用法。
     * 非线程安全，只允许在 GUI 线程使用。
     */
    static AppSettings &get();

    /**
     * 从 settings.json 读入并覆盖当前值；顺手做旧配置迁移与区间夹取。
     * 文件缺失或解析失败时全部保留结构体默认值，不会报错、也不会重建文件。
     */
    void load();

    /**
     * 全量写回 settings.json（QSaveFile 原子替换）。
     * 失败时静默返回，不抛异常——设置丢失比崩溃可接受。
     */
    void save();

    /// 是否深色主题
    bool isDark() const { return theme == Dark; }

    // ---- 主题令牌：都随 theme 切换，调用处不必自己判断明暗 ----
    QColor accent() const;        // 强调色
    QColor textColor() const;     // 主文字
    QColor subTextColor() const;  // 次要文字
    QColor glassTint() const;     // 玻璃底色（带 alpha，由 opacity 换算而来）
    QColor borderColor() const;   // 1px 描边
    QColor hoverColor() const;    // 悬停底色
    QColor menuBg() const;        // 弹出菜单底色
    /**
     * 生成统一 UI 字体。
     * @param px      字号，单位是像素（不是磅），按屏幕实际像素渲染
     * @param weight  QFont::Weight 枚举值，默认 Normal；传整数会被 static_cast 成枚举
     * @return        固定使用「Microsoft YaHei UI」并开启全像素 hinting 的字体
     */
    QFont uiFont(int px, int weight = QFont::Normal) const;
};
