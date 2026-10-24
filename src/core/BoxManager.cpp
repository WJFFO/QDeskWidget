#include "core/BoxManager.h"
#include "core/Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

// BoxManager 实现：boxes.json 的全量读写 + 内存列表的增删改。
// 整体是「读全量 / 写全量」模型：boxes.json 很小（几十条），没有增量更新的必要，
// 也没有多进程并发写同一份文件的场景，因此不做文件锁与脏标记。
// 坑：create/remove 会立即 save()，而 update/setPending 不会，调用方要自己补 save()。

// 构造仅建立 Qt 父子关系，不做任何加载——何时 load() 由 App 决定，避免静态初始化期访问磁盘。
BoxManager::BoxManager(QObject *parent)
    : QObject(parent)
{
}

// 逐条解析并做三件防御：丢空 id、丢已下线的 KindSearch、给缺 folder 的文件盒补托管目录。
// 任何异常都只是跳过单条记录，不会让整个 load 失败。
void BoxManager::load()
{
    m_boxes.clear();
    QFile f(Paths::boxesFile());
    if (!f.open(QIODevice::ReadOnly))
        return;

    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object().value("boxes").toArray();
    for (const QJsonValue &v : arr) {
        BoxConfig c = BoxConfig::fromJson(v.toObject());
        if (c.id.isEmpty())
            continue;
        if (c.kind == BoxConfig::KindSearch)
            continue; // 搜索组件已移除，旧配置直接丢弃
        if (c.folder.isEmpty() && c.kind == BoxConfig::KindFiles) {
            // 早期版本可能没写 folder：重建托管目录，保证窗口有内容可显示
            c.folder = Paths::boxStorageDir(c.id);
            c.managed = true;
            Paths::ensureDir(c.folder);
        }
        m_boxes.append(c);
    }
}

// 全量重写：先用 QSaveFile 写临时文件，commit() 成功才替换正式文件，
// 避免写到一半崩溃把用户的布局文件截断成空。
// version 目前恒为 1，仅作将来升级格式时的判别位；Indented 是为了人工排查方便。
void BoxManager::save()
{
    QJsonArray arr;
    for (const BoxConfig &c : m_boxes)
        arr.append(c.toJson());

    QJsonObject root;
    root["version"] = 1;
    root["boxes"] = arr;

    Paths::ensureDir(Paths::dataDir());
    QSaveFile f(Paths::boxesFile());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.commit();
}

// 线性查找：盒子数量是个位数到几十，不值得建索引，也省去维护 id→下标 的一致性。
int BoxManager::indexOf(const QString &id) const
{
    for (int i = 0; i < m_boxes.size(); ++i)
        if (m_boxes.at(i).id == id)
            return i;
    return -1;
}

// 返回的是内部元素地址，调用方不要跨 create/remove 持有。
BoxConfig *BoxManager::find(const QString &id)
{
    const int i = indexOf(id);
    return i >= 0 ? &m_boxes[i] : nullptr;
}

// 只用 UUID 的前 8 个十六进制位，够用且短：能直接当目录名，也便于日志里肉眼比对。
// 理论上存在碰撞可能，但盒子数量极少，实际不会遇到。
QString BoxManager::makeId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

// 冲突时从 " (2)" 开始递增编号；每次都从头扫一遍列表，所以是 O(n²)，
// 盒子数量小到可以忽略。注意本函数自己不做 trim，由调用方先 trim。
QString BoxManager::uniqueTitle(const QString &base) const
{
    QString title = base.isEmpty() ? QStringLiteral("新整理盒") : base;
    int n = 2;
    auto taken = [this](const QString &t)
    {
        for (const BoxConfig &c : m_boxes)
            if (c.title == t)
                return true;
        return false;
    };
    QString candidate = title;
    while (taken(candidate))
        candidate = QStringLiteral("%1 (%2)").arg(title).arg(n++);
    return candidate;
}

// 注意 managed 与 folder 的优先级：managed 为 true 时即便传了 folder 也会被忽略，
// 统一改用 storage/<新id>；folder 为空也强制托管，避免出现没有内容的盒子。
BoxConfig BoxManager::create(const QString &title, const QString &folder, bool managed)
{
    BoxConfig c;
    c.id = makeId();
    c.title = uniqueTitle(title.trimmed());
    c.managed = managed;
    if (managed || folder.isEmpty()) {
        c.folder = Paths::boxStorageDir(c.id);
        c.managed = true;
    } else {
        c.folder = QDir::fromNativeSeparators(folder); // 存 '/' 分隔，跨平台比较路径时才一致
    }
    Paths::ensureDir(c.folder);

    // 新盒子错开摆放：优先贴着上一个盒子右下偏移 24px，形成叠放效果；
    // 列表为空（或上一个几何无效）时退回到按序号阶梯式的 120 + n%6*36，避免全叠在原点。
    const QRect ref = m_boxes.isEmpty() ? QRect(0, 0, 0, 0) : m_boxes.last().geometry;
    int x = 120 + (m_boxes.size() % 6) * 36;
    int y = 120 + (m_boxes.size() % 6) * 36;
    if (ref.isValid() && !ref.isNull()) {
        x = ref.x() + 24;
        y = ref.y() + 24;
    }
    c.geometry = QRect(x, y, 420, 320);

    m_boxes.append(c);
    emit boxAdded(c);   // 信号在 save() 之前发：接收方建的窗口拿到的就是刚 append 的这份配置
    save();
    return c;
}

// 不建目录（组件没有内容目录），也不设置 folder。
BoxConfig BoxManager::createWidget(int kind, const QString &pluginId, const QString &title)
{
    BoxConfig c;
    c.id = makeId();
    c.kind = kind;
    c.pluginId = pluginId;
    c.title = uniqueTitle(title.trimmed());
    // 组件一律按最小尺寸出生：120×80 小于任何组件的最紧凑版式，
    // FileBoxWindow 构造时会把它夹到面板 minimumSizeHint()，所以不会出现
    // 「先撑开再缩小」的闪跳。App::createWidgetBox() 再调 shrinkToMinimum() 兜底。
    // 位置用 34px 一级的阶梯错开（文件盒那边是 36px），最多错开 6 级后回到起点。
    c.geometry = QRect(160 + (m_boxes.size() % 6) * 34, 140 + (m_boxes.size() % 6) * 34,
                       120, 80);

    m_boxes.append(c);
    emit boxAdded(c);
    save();
    return c;
}

// 只摘掉列表条目：磁盘目录一律保留（含托管目录），用户误删后还能从 storage 里找回文件。
void BoxManager::remove(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    m_boxes.removeAt(i);
    emit boxRemoved(id);
    save();
}

// 按 cfg.id 定位后整体覆盖；不发信号也不落盘，调用方需自行 save()（或等退出统一写）。
void BoxManager::update(const BoxConfig &cfg)
{
    const int i = indexOf(cfg.id);
    if (i < 0)
        return;
    m_boxes[i] = cfg;
}

// 目前只是 update() 的别名：id 参数未参与逻辑，也不校验它与 cfg.id 是否一致。
void BoxManager::setPending(const QString &id, const BoxConfig &cfg)
{
    update(cfg);
}
