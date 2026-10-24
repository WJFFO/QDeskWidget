#pragma once

#include "core/BoxConfig.h"

#include <QList>
#include <QObject>
#include <QString>

/**
 * @file BoxManager.h
 * 盒子集合的唯一权威来源：负责 boxes.json 的读写，以及新建 / 删除 / 改写的内存操作。
 *
 * 关键约定：所有改动都先落内存再（可选地）落盘，落盘是全量重写而非增量。
 * 增删会同步发 boxAdded / boxRemoved 信号，由 App 负责创建或销毁对应窗口；
 * 而 update() 只改数据、不发信号，适合窗口回写自身状态这类高频调用。
 * 返回的 BoxConfig* 指向内部 QList 元素，列表发生增删后可能失效。
 */

/**
 * 整理盒集合的加载 / 保存 / 增删。
 * 所有整理盒状态持久化到 boxes.json。
 */
class BoxManager : public QObject
{
    Q_OBJECT

public:
    /** @param parent  Qt 父子关系，通常传 App 实例 */
    explicit BoxManager(QObject *parent = nullptr);

    /**
     * 从 boxes.json 全量重建内存列表（会先清空）。
     * 文件不存在、打不开、JSON 破损都静默当作「没有盒子」，不会补写空文件。
     * 副作用：为缺失 folder 的文件盒补出托管目录并立即 mkpath。
     */
    void load();

    /**
     * 把当前 m_boxes 全量写回 boxes.json（带 version 字段）。
     * 通过 QSaveFile 原子替换，写失败静默放弃（下次 save 会再试）。
     */
    void save();

    /// 只读访问整个列表；不要在遍历它的同时调用 create/remove/update
    const QList<BoxConfig> &boxes() const { return m_boxes; }
    /**
     * @param id  盒子唯一 id（8 位十六进制字符串）
     * @return     找到返回内部元素指针（可写），找不到返回 nullptr
     */
    BoxConfig *find(const QString &id);
    /**
     * @param id  盒子唯一 id
     * @return     在 m_boxes 中的下标；找不到返回 -1
     */
    int indexOf(const QString &id) const;

    /**
     * 新建整理盒。
     * folder 为空则自动创建托管目录。
     * @param title    期望标题；为空时用「新整理盒」，重名自动追加 " (2)" 这类后缀
     * @param folder   关联的已有目录；为空则走托管目录 storage/<新 id>
     * @param managed  true 表示托管（忽略 folder，强制用 storage/<id>）；false 且 folder 非空时引用外部目录
     * @return         已加入列表的配置副本（注意不是指针，列表扩容后副本仍有效）
     * 副作用：发 boxAdded 并立即 save()；同时会 mkpath 目标目录。
     */
    BoxConfig create(const QString &title, const QString &folder = QString(),
                     bool managed = true);

    /**
     * 新建小组件（搜索 / 天气 / 音乐 / 插件），不创建文件夹。
     * @param kind      组件类型，取 BoxConfig::KindWeather / KindMusic / KindPlugin
     * @param pluginId  插件 id；非插件组件传空字符串
     * @param title     期望标题，重名处理同 create()
     * @return          已加入列表的配置副本
     * 副作用：发 boxAdded 并立即 save()。
     */
    BoxConfig createWidget(int kind, const QString &pluginId, const QString &title);

    /**
     * 从列表移除盒子并发 boxRemoved、落盘。
     * 不会删除磁盘上的内容目录（托管目录也一样保留，便于用户找回文件）。
     * @param id  盒子 id；不存在时静默返回
     */
    void remove(const QString &id);

    /**
     * 用外部传入的配置整体覆盖同 id 的条目。
     * 只改内存，不 save()、不发信号——调用方随后自己 save 或等退出时统一落盘。
     * @param cfg  新配置，按 cfg.id 定位；id 不存在时静默返回
     */
    void update(const BoxConfig &cfg);

    /**
     * 关闭时把每个盒子的当前状态写回。
     * 目前等价于 update()，id 参数未参与逻辑（不校验与 cfg.id 是否一致）。
     * @param id   盒子 id（暂未使用）
     * @param cfg  窗口当前状态
     */
    void setPending(const QString &id, const BoxConfig &cfg);

signals:
    /// 已加入内存列表后立刻发出（此时尚未 save）；接收方据此新建窗口
    void boxAdded(const BoxConfig &cfg);
    /// 已从内存列表移除后发出（此时尚未 save），只带 id；窗口由接收方自行销毁
    void boxRemoved(const QString &id);

private:
    /// 生成 8 位十六进制 id（UUID 去掉花括号后取前 8 位）
    QString makeId() const;
    /**
     * 生成不与现有盒子重名的标题。
     * @param base  期望标题；空则退回「新整理盒」
     * @return      唯一标题，冲突时形如 "名字 (2)"、"名字 (3)"（编号从 2 起）
     */
    QString uniqueTitle(const QString &base) const;

    QList<BoxConfig> m_boxes;   // 内存中的全部盒子，顺序即 boxes.json 中的顺序，也决定新盒子的错开摆放基准
};
