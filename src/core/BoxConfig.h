#pragma once

#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QStringList>

/**
 * @file BoxConfig.h
 * 单个盒子（整理盒 / 小组件）的持久化配置：纯数据结构 + JSON 序列化 + 全局回退取值。
 *
 * 关键约定：外观类数值字段用 -1 / 0 作哨兵表示「跟随全局设置」，因此渲染前必须走
 * effectiveXxx()，直接读成员会把哨兵当成真实尺寸 / 模式传给 UI。
 * 序列化是「可选字段为空就不写」，fromJson 侧靠结构体默认值兜底，所以新增字段
 * 天然向后兼容；但哨兵值的语义一旦改了就必须同时改 fromJson 的默认值。
 */

/**
 * 一个盒子（Box）的持久化配置。
 *
 * 盒子有两种形态：
 *  - 文件整理盒（kind = KindFiles）：folder 是一个真实文件夹，managed 表示该文件夹
 *    由本程序在 data/storage/<id> 下创建并托管，否则为用户关联的现有文件夹；
 *  - 小组件（kind = KindWeather / KindMusic / KindPlugin）：
 *    内容由组件面板提供，folder 通常为空，插件组件用 pluginId 区分实现。
 *
 * 多个盒子还可以「组合」到一个窗口里：组长窗口把其它盒子的 id 放在 groupMembers，
 * currentMember 记录当前显示的是哪个成员。
 *
 * 本结构体只做数据承载，不含任何窗口 / 磁盘操作；改名、移动、删目录等都归
 * BoxManager 与 FileBoxWindow 负责。
 */
struct BoxConfig
{
    /// 盒子类型，序列化进 boxes.json 的 "kind" 字段，取值必须保持稳定
    enum Kind {
        KindFiles = 0,    // 文件整理盒：内容是 folder 指向的真实目录
        KindSearch = 1,   // 搜索组件已移除，该值仅用于识别并丢弃旧配置
        KindWeather = 2,  // 天气小组件
        KindMusic = 3,    // 音乐小组件
        KindPlugin = 4    // 第三方插件小组件，具体实现由 pluginId 决定
    };

    QString id;         // 8 位十六进制（BoxManager::makeId 生成，UUID 前 8 位），同时用作托管目录名 storage/<id>
    QString title;      // 窗口标题；BoxManager 保证全局不重名，重名自动追加 " (2)" 这类后缀
    QString folder;     // 内容目录的绝对路径（统一为 '/' 分隔）；kind != KindFiles 时通常为空
    bool managed = false; // true = 目录由本程序在 storage/<id> 下创建并托管；false = 用户已有目录，只做引用（删盒子都不删目录）

    int kind = KindFiles;   // 见 Kind；缺省按文件整理盒处理，兼容没有 kind 的超旧配置
    QString pluginId;        // kind == KindPlugin 时的插件 id
    QJsonObject widgetState; // 组件自定义状态（城市、目录、音量…）

    QStringList groupMembers; // 该窗口聚合的其它盒子 id（不含自己）
    QString currentMember;    // 当前显示的成员 id，空表示显示自己

    /** 组件可以隐藏标题栏，让内容铺满整窗（此时 Alt+拖动移动、右键出菜单） */
    bool hideHeader = false;

    QRect geometry = QRect(120, 120, 420, 320); // 窗口位置与尺寸，序列化时拆成 x/y/w/h 四个键
    int iconSize = 0;    // 0 表示跟随全局
    int viewMode = -1;   // -1 跟随全局，0 图标，1 列表，2 堆叠
    int sortMode = -1;   // -1 跟随全局
    int capsule = -1;    // -1 跟随全局，0 折叠成标题条，1 折叠成胶囊
    int stackGrid = 0;   // 0 跟随全局，3 / 5
    bool collapsed = false;     // 折叠成标题条：文件网格与组件面板都隐藏，只留标题栏
    bool alwaysOnTop = false;   // 置顶窗口
    bool lockSize = false;      // 锁定大小：拖边框不再改变窗口尺寸
    int opacity = -1;    // 玻璃底色不透明度 0-100；-1 跟随全局（注意 effectiveOpacity 用 > 0 判断，显式 0 也会回落全局）
    QStringList manualOrder; // 手动排序时的文件顺序；存的是文件名而非完整路径，因为重命名/移动后路径会变

    /**
     * 序列化成 boxes.json 里的一条记录。
     * @return 可直接塞进数组的 JSON 对象；可选字段为空时不会出现在结果里
     */
    QJsonObject toJson() const;

    /**
     * 从 boxes.json 的一条记录还原配置，缺字段一律回落到结构体默认值。
     * @param o  单条盒子的 JSON 对象（不是整个文件）
     * @return     解析出的配置；不校验 id 是否为空，交由 BoxManager::load 丢弃
     */
    static BoxConfig fromJson(const QJsonObject &o);

    /// 是否小组件（天气 / 音乐 / 插件）；注意已废弃的 KindSearch 也会返回 true
    bool isWidget() const { return kind != KindFiles; }
    /// 是否是多盒子组合窗口的组长
    bool hasGroupMembers() const { return !groupMembers.isEmpty(); }

    /** 解析后的实际取值：成员值有效就用成员值，否则回落到 AppSettings::get() 里的全局值 */
    int effectiveIconSize() const;
    int effectiveViewMode() const;
    int effectiveSortMode() const;
    int effectiveOpacity() const;
    bool effectiveCapsule() const;   // 恒为 false，见实现处说明
    int effectiveStackGrid() const;
};
