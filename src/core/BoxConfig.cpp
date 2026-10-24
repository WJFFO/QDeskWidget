#include "core/BoxConfig.h"
#include "core/AppSettings.h"

#include <QJsonArray>

// 本文件只做两件事：字段与 JSON 的互相映射、哨兵值回落到全局设置。
// 序列化刻意保持「扁平 + 可选字段缺省不写」：x/y/w/h 直接平铺在顶层而不是嵌成
// 子对象，是为了让手改 boxes.json 时足够直观，也让旧版本读到未知字段时直接忽略。

// 写出全部必填字段；pluginId / widgetState / groupMembers / currentMember 为空时省略，
// 以减少 boxes.json 体积，同时依赖 fromJson 的默认值还原语义。
// 几何拆成 x/y/w/h 平铺，而非嵌套对象，便于直接手改 boxes.json。
QJsonObject BoxConfig::toJson() const
{
    QJsonObject o;
    o["id"]        = id;
    o["title"]     = title;
    o["folder"]    = folder;
    o["managed"]   = managed;
    o["kind"]      = kind;
    // 可选字段为空就不写，体积与可读性都更好；读回时靠 fromJson 的默认值补。
    if (!pluginId.isEmpty())
        o["pluginId"] = pluginId;
    if (!widgetState.isEmpty())
        o["widgetState"] = widgetState;
    o["x"]         = geometry.x();
    o["y"]         = geometry.y();
    o["w"]         = geometry.width();
    o["h"]         = geometry.height();
    o["iconSize"]  = iconSize;
    o["viewMode"]  = viewMode;
    o["sortMode"]  = sortMode;
    o["capsule"]   = capsule;
    o["stackGrid"] = stackGrid;
    o["collapsed"]    = collapsed;
    o["alwaysOnTop"]  = alwaysOnTop;
    o["lockSize"]     = lockSize;
    o["opacity"]      = opacity;

    QJsonArray arr;
    for (const QString &s : manualOrder)
        arr.append(s);
    o["manualOrder"] = arr;

    if (!groupMembers.isEmpty()) {
        QJsonArray members;
        for (const QString &s : groupMembers)
            members.append(s);
        o["groupMembers"] = members;
    }
    if (!currentMember.isEmpty())
        o["currentMember"] = currentMember;
    o["hideHeader"] = hideHeader;
    return o;
}

// 每个字段都用「键存在则取，不存在则用结构体默认值」的方式读，因此新增字段
// 不需要写迁移代码。x/y 没有下限保护（多屏摆到负坐标是合法的），w/h 才夹。
BoxConfig BoxConfig::fromJson(const QJsonObject &o)
{
    BoxConfig c;
    c.id       = o.value("id").toString();
    c.title    = o.value("title").toString();
    c.folder   = o.value("folder").toString();
    c.managed  = o.value("managed").toBool();
    c.kind     = o.value("kind").toInt(KindFiles);
    c.pluginId = o.value("pluginId").toString();
    c.widgetState = o.value("widgetState").toObject();
    // 下限交给窗口的 minimumSize 处理：这里夹得太死，小尺寸组件就永远摆不出来
    c.geometry = QRect(o.value("x").toInt(120), o.value("y").toInt(120),
                       qMax(80, o.value("w").toInt(420)),
                       qMax(50, o.value("h").toInt(320)));
    c.iconSize    = o.value("iconSize").toInt(0);
    c.viewMode    = o.value("viewMode").toInt(-1);
    c.sortMode    = o.value("sortMode").toInt(-1);
    c.capsule     = o.value("capsule").toInt(-1); // 仍照读照写，但 effectiveCapsule() 恒为 false
    c.stackGrid   = o.value("stackGrid").toInt(0);
    c.collapsed   = o.value("collapsed").toBool(false);
    c.alwaysOnTop = o.value("alwaysOnTop").toBool(false);
    c.lockSize = o.value("lockSize").toBool(false); // 旧配置没有该键，缺省 false 即「不锁大小」
    c.opacity     = o.value("opacity").toInt(-1);
    // manualOrder 存的是文件名（不是路径），因此目录内重命名后这条记录会自然失效，
    // applySort 会把它当作「未收录项」排到已收录项之后，不会报错。
    for (const QJsonValue &v : o.value("manualOrder").toArray())
        c.manualOrder << v.toString();
    for (const QJsonValue &v : o.value("groupMembers").toArray())
        c.groupMembers << v.toString();
    c.currentMember = o.value("currentMember").toString();
    c.hideHeader = o.value("hideHeader").toBool(false);
    return c;
}

// 注意判断是 > 0 而不是 >= 0：iconSize 的 0 和 -1 都视为「跟随全局」。
int BoxConfig::effectiveIconSize() const
{
    return iconSize > 0 ? iconSize : AppSettings::get().iconSize;
}

// viewMode 的 0 是合法的「图标视图」，所以这里用 >= 0 判断，只有 -1 才回落。
int BoxConfig::effectiveViewMode() const
{
    return viewMode >= 0 ? viewMode : AppSettings::get().defaultViewMode;
}

// 同 viewMode：0 = 手动排序，是合法值，只有 -1 才回落全局。
int BoxConfig::effectiveSortMode() const
{
    return sortMode >= 0 ? sortMode : AppSettings::get().defaultSortMode;
}

// 同样用 > 0 判断，所以显式写入 opacity = 0 也会被当作「跟随全局」。
int BoxConfig::effectiveOpacity() const
{
    return opacity > 0 ? opacity : AppSettings::get().opacity;
}

bool BoxConfig::effectiveCapsule() const
{
    // 胶囊模式已移除：折叠后统一缩成标题条。
    // `capsule` 字段继续保留在配置里，只是不再影响任何行为（旧数据无需迁移）。
    return false;
}

// 只接受 3 / 5 两个合法列数，其余值（含 0 和各种脏数据）一律回落到全局设置。
int BoxConfig::effectiveStackGrid() const
{
    if (stackGrid == 3 || stackGrid == 5)
        return stackGrid;
    return AppSettings::get().stackGrid;
}
