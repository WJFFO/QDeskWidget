/**
 * @file WidgetRegistry.h
 * 组件注册表：内置组件（天气 / 音乐）的静态清单 + 插件目录的扫描结果，对上层只暴露
 * 「有哪些类型可新建」「按配置造一个面板」两件事。
 *
 * 关键约定：
 *  - 插件扫描是懒加载的（首次用到才扫，之后缓存），装了新 DLL 必须显式 reloadPlugins()；
 *  - create() 造出的面板已经 restoreState 过，调用方不用再灌配置，但不负责 show()；
 *  - 文件整理盒 / 已废弃的搜索组件在这里返回 nullptr，这是正常返回而不是错误。
 */
#pragma once

#include "core/BoxConfig.h"
#include "widgets/WidgetPanel.h"

#include <QString>
#include <QVector>

/**
 * 可新建的组件类型：内置项（见 builtins()，现为天气 / 音乐）+ 用户插件。
 * 只用于填设置面板里的「添加组件」下拉框，不含任何运行时状态。
 */
struct WidgetTypeInfo
{
    QString id;          // 内置组件 id（weather / music）或插件的 pluginId；search 是已移除的旧值
    QString name;        // 菜单里显示的名字
    QString description; // 菜单项下方的说明文字，可为空
    int kind = BoxConfig::KindPlugin; // 对应的 BoxConfig::Kind；内置项用来回查名字，插件恒为 KindPlugin
    bool plugin = false; // true = 来自插件目录；displayNameFor() 靠它区分「按 id 查」还是「按 kind 查」
};

/**
 * 组件注册表。全部是自由函数 + 内部静态状态，没有实例可创建。
 *
 * 只能在 GUI 线程用：首次调用会 LoadLibrary 插件 DLL 并 new 插件控件，插件也假定
 * 自己与宿主同线程；内部静态状态是懒构造的，第一次取用会有一次磁盘扫描的开销。
 */
namespace WidgetRegistry
{

/**
 * 插件目录：<数据目录>/plugins（不保证存在，调用方自己按需创建）。
 * @return 目录绝对路径
 */
QString pluginsDir();

/**
 * 清空并重新扫描插件目录（新增 / 删除 DLL 后调用）。
 * 扫描失败的 DLL 只打 warning 并跳过，不影响其它插件与内置组件；
 * 会先卸载已加载的插件，因此正在显示的插件面板会失效。
 */
void reloadPlugins();

/**
 * 内置组件 + 已加载的插件（首次调用会自动触发扫描）。
 * @return 可直接填进「添加组件」下拉框的列表；内置项在前，插件按目录枚举顺序追加
 * @note 每次都是全新的值拷贝，调用方可以随手排序/删项，不影响注册表内部状态。
 */
QVector<WidgetTypeInfo> types();

/**
 * 按配置创建面板；文件整理盒返回 nullptr。
 * @param cfg    盒子配置：kind 决定类型，pluginId 定位插件，widgetState 直接灌给面板
 * @param parent 父控件，交给面板
 * @return 新面板（未 show、未布局），调用方负责 setParent/显示；
 *         kind 为文件盒、已废弃的搜索组件、或插件未找到/创建失败时返回 nullptr
 *
 * @note 内置组件的 restoreState() 在这里就调过了，所以返回时面板已是配置里的样子；
 *       拿到的指针归 Qt 父子关系管，调用方不要 delete，用 destroyPanel 那套流程回收。
 */
WidgetPanel *create(const BoxConfig &cfg, QWidget *parent);

/**
 * 组件中文名（用于窗口标题兜底）。
 * @param kind     盒子类型；非插件时按它匹配内置项
 * @param pluginId 插件 id，仅 kind == KindPlugin 时使用
 * @return 内置名 / 插件显示名；插件找不到时回落成 pluginId 本身，全空时返回「组件」或「整理盒」
 */
QString displayNameFor(int kind, const QString &pluginId);

/**
 * 已加载的插件数量（首次调用会自动触发扫描）。
 * @return 本次扫描成功、且确实实现了 IWidgetPlugin 的 DLL 个数
 */
int pluginCount();

} // namespace WidgetRegistry
