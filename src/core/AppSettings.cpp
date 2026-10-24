#include "core/AppSettings.h"
#include "core/Paths.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

// AppSettings 的实现：settings.json 的读写 + 明暗两套颜色令牌。
// 约定：load() 负责把任何外部输入（旧版本文件、手改的脏值）收敛到合法区间，
// 因此其余代码可以直接信任字段取值，不必到处再夹一次。

// 函数内静态对象 + 一次性标志：首次 get() 时同步读盘，之后直接返回同一实例。
// 注意 loaded 标志的读写在 C++ 层面不是原子的，本类约定只在 GUI 线程使用。
AppSettings &AppSettings::get()
{
    static AppSettings s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        s.load();
    }
    return s;
}

// 每个字段都走 toInt(当前值) / toBool(当前值) 的「键缺失就保留默认」写法，
// 这样缺字段的旧配置能逐一回落，不需要写逐版本迁移代码。
// 顺序很重要：先读原始值 → 再跑 acrylic→material 迁移 → 最后统一 qBound。
void AppSettings::load()
{
    QFile f(Paths::settingsFile());
    // 读不到就直接拿空对象走一遍下面的默认值回落，省掉一套「文件不存在」的分支。
    const bool readable = f.open(QIODevice::ReadOnly);
    const QJsonObject o = readable ? QJsonDocument::fromJson(f.readAll()).object()
                                   : QJsonObject();

    theme            = o.value("theme").toInt(theme);
    material         = o.value("material").toInt(material);
    systemCorners    = o.value("systemCorners").toBool(systemCorners);
    opacity          = o.value("opacity").toInt(opacity);
    iconSize         = o.value("iconSize").toInt(iconSize);
    nameLines        = o.value("nameLines").toInt(nameLines);
    defaultViewMode  = o.value("defaultViewMode").toInt(defaultViewMode);
    defaultSortMode  = o.value("defaultSortMode").toInt(defaultSortMode);
    confirmDelete    = o.value("confirmDelete").toBool(confirmDelete);
    snapToGrid       = o.value("snapToGrid").toBool(snapToGrid);
    gridStep         = o.value("gridStep").toInt(gridStep);
    showHidden       = o.value("showHidden").toBool(showHidden);
    capsuleCollapsed = o.value("capsuleCollapsed").toBool(capsuleCollapsed);
    stackGrid        = o.value("stackGrid").toInt(stackGrid);
    weatherCity      = o.value("weatherCity").toString(weatherCity);
    musicAppId       = o.value("musicAppId").toString(musicAppId);
    widgetRefreshMin = o.value("widgetRefreshMin").toInt(widgetRefreshMin);

    // 旧版本只有 bool acrylic，迁移成 material
    if (o.contains("acrylic") && !o.contains("material"))
        material = o.value("acrylic").toBool(true) ? Acrylic : Solid;

    // 统一收敛：越界的脏值/被手改的值都夹回合法区间，后续代码不必再防。
    theme         = qBound(0, theme, 1);
    material      = qBound(0, material, 2);
    opacity       = qBound(10, opacity, 100); // 下限 10：全透明窗口会既看不见也点不到；设置对话框的滑块量程是 20-100
    iconSize      = qBound(32, iconSize, 160); // 32-160；设置对话框滑块只到 144，上限放宽是为了容错手改的配置
    nameLines     = qBound(1, nameLines, 2);
    gridStep      = qBound(2, gridStep, 64);
    stackGrid     = (stackGrid == 5) ? 5 : 3; // 只支持 3 列 / 5 列两档，其余一律按 3
    defaultViewMode = qBound(0, defaultViewMode, 2);
    widgetRefreshMin = qBound(0, widgetRefreshMin, 60); // 0 = 关闭自动刷新
    if (weatherCity.trimmed().isEmpty())
        weatherCity = QStringLiteral("上海"); // 空城市会让天气组件无法请求，给个可用默认值
}

// 全量写回，字段顺序与 load() 对应，便于肉眼 diff。
// 先 ensureDir 再开 QSaveFile：data 目录不存在时 QSaveFile 会直接开失败。
void AppSettings::save()
{
    QJsonObject o;
    o["theme"]            = theme;
    o["material"]         = material;
    o["systemCorners"]    = systemCorners;
    o["opacity"]          = opacity;
    o["iconSize"]         = iconSize;
    o["nameLines"]        = nameLines;
    o["defaultViewMode"]  = defaultViewMode;
    o["defaultSortMode"]  = defaultSortMode;
    o["confirmDelete"]    = confirmDelete;
    o["snapToGrid"]       = snapToGrid;
    o["gridStep"]         = gridStep;
    o["showHidden"]       = showHidden;
    o["capsuleCollapsed"] = capsuleCollapsed;
    o["stackGrid"]        = stackGrid;
    o["weatherCity"]      = weatherCity;
    o["musicAppId"]       = musicAppId;
    o["widgetRefreshMin"] = widgetRefreshMin;

    Paths::ensureDir(Paths::dataDir());
    QSaveFile f(Paths::settingsFile());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.commit();
}

// 下面几个令牌全部由 theme 二选一出值，调用处不需要自己判断明暗；改配色只改这里。
QColor AppSettings::accent() const
{
    return isDark() ? QColor(0x4C, 0xC2, 0xFF) : QColor(0x00, 0x67, 0xC0);
}

QColor AppSettings::textColor() const
{
    return isDark() ? QColor(0xF2, 0xF3, 0xF5) : QColor(0x1B, 0x1B, 0x1F);
}

QColor AppSettings::subTextColor() const
{
    return isDark() ? QColor(0xA6, 0xA9, 0xB0) : QColor(0x6A, 0x6E, 0x78);
}

// 把 0-100 的 opacity 映射成 0-235 的 alpha：上限取 235 而非 255，始终留一点透光。
QColor AppSettings::glassTint() const
{
    const int a = qRound(opacity / 100.0 * 235.0);
    return isDark() ? QColor(24, 25, 30, a) : QColor(248, 249, 252, a);
}

// 描边 / 悬停 / 菜单底色都带 alpha，叠在玻璃材质上才有层次；数值都是 0-255 的 A。
QColor AppSettings::borderColor() const
{
    return isDark() ? QColor(255, 255, 255, 34) : QColor(15, 18, 24, 28);
}

QColor AppSettings::hoverColor() const
{
    return isDark() ? QColor(255, 255, 255, 22) : QColor(15, 18, 24, 16);
}

QColor AppSettings::menuBg() const
{
    return isDark() ? QColor(38, 39, 45, 248) : QColor(252, 252, 254, 250);
}

// px 是像素字号（setPixelSize），与 setPointSize 的磅值不同，不受 DPI 缩放二次影响。
// PreferFullHinting 走全像素对齐，中文小字号在 Windows 上更锐利。
QFont AppSettings::uiFont(int px, int weight) const
{
    QFont f(QStringLiteral("Microsoft YaHei UI"));
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    f.setHintingPreference(QFont::PreferFullHinting);
    return f;
}
