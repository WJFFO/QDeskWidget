/**
 * @file WeatherIcons.cpp
 * 天气图标渲染：读资源里的 SVG 模板 → 按主题替换颜色令牌 → 栅格化成 QPixmap → 贴图。
 *
 * 坑：
 *  - 资源里的 SVG 已经**去掉了 mask**（QtSvg 不支持，带 mask 的那一组会被整组丢弃），
 *    所以「太阳被云挡住」的效果是「太阳先画、云后画盖上去」，不是真正的遮罩；
 *  - 颜色令牌的替换必须在**解析前**做（QSvgRenderer 只吃字节流），所以模板缓存的是
 *    替换后的 QByteArray，不是 QSvgRenderer 对象；
 *  - 栅格尺寸要乘 devicePixelRatio 再调 setDevicePixelRatio，否则高 DPI 下会被拉糊。
 */
#include "widgets/WeatherIcons.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QSvgRenderer>

namespace
{

/// 资源目录前缀，对应 CMake 里 qt_add_resources 的 PREFIX
const char *const kResPrefix = ":/weather/";

/**
 * 图标目录：下标必须在 slugForCode() 返回值的取值范围内。
 * 顺序不要改动语义（缓存键用的是下标，运行时改顺序只会命中旧的错误缓存，
 * 但源码可读性上它也是「一张表」，改动时同步 slugForCode 即可）。
 */
const char *const kSlugs[] = {
    "clear-day",              // 0 晴
    "mostly-clear-day",       // 1 晴间多云（主要晴）
    "partly-cloudy-day",      // 2 多云
    "overcast",               // 3 阴
    "fog",                    // 4 雾
    "drizzle",                // 5 毛毛雨
    "rain",                   // 6 雨
    "snow",                   // 7 雪
    "partly-cloudy-day-rain", // 8 阵雨
    "partly-cloudy-day-snow", // 9 阵雪
    "thunderstorms",          // 10 雷阵雨
    "thunderstorms-hail",     // 11 雷暴伴冰雹
};
/// kSlugs 的元素个数；缓存键里给它 4 个 bit 足够
constexpr int kSlugCount = int(sizeof(kSlugs) / sizeof(kSlugs[0]));

/**
 * 一套配色下的 4 个可变颜色。
 * 原版 Meteocons 只按「白底」设计，云朵是近白色（#e6effc），
 * 直接放到深色玻璃上反而最合适；浅色主题必须整组压暗，否则云朵会糊在白底里。
 */
struct Palette
{
    const char *cloud;  ///< 云朵主体（占比最大，最需要按主题调）
    const char *cloud2; ///< 云的次级色（阴天图标里被压在下面那一朵）
    const char *cloud3; ///< 雾/风的细线
    const char *rain;   ///< 雨滴；深色主题下原版的深蓝几乎看不见，要提亮
};

/// 深色主题：接近原版，只把雨滴提亮一档
const Palette kPaletteDark = { "#e6effc", "#94a3b8", "#e2e8f0", "#3f8efc" };
/// 浅色主题：云朵压到石板灰，雨滴回到原版深蓝
const Palette kPaletteLight = { "#c3d4e8", "#94a3b8", "#a8b6c6", "#0a5ad4" };

/**
 * 模板缓存：键是 (图标下标 | 主题)，值是**已替换好颜色**的 SVG 字节流。
 * 只缓存 12×2 = 24 条，体积可忽略；省掉的是每次栅格化都要做一遍字符串替换。
 */
QHash<quint32, QByteArray> &templateCache()
{
    static QHash<quint32, QByteArray> cache;
    return cache;
}

/**
 * 栅格结果缓存：键是 (图标下标 | 主题 | 像素边长 | DPI×100)。
 * paintEvent 里每次都要画 1 个主图标 + 最多 4 个逐日小图标，命中率极高。
 */
QHash<quint64, QPixmap> &pixmapCache()
{
    static QHash<quint64, QPixmap> cache;
    return cache;
}

/**
 * 取「已替换颜色」的 SVG 字节流。
 * @param slugIdx 图标下标（0..kSlugCount-1）
 * @param dark    是否深色主题
 * @return 字节流；资源缺失时返回空 QByteArray（调用方据此跳过绘制）
 */
QByteArray svgSource(int slugIdx, bool dark)
{
    const quint32 key = quint32(slugIdx) | (dark ? 0x80u : 0u);
    const auto it = templateCache().constFind(key);
    if (it != templateCache().constEnd())
        return it.value();

    QFile f(QString::fromLatin1(kResPrefix) + QString::fromLatin1(kSlugs[slugIdx])
            + QStringLiteral(".svg"));
    // 资源是编译期打进 exe 的，打不开说明 CMake 的 qt_add_resources 没带上这个文件
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();

    QByteArray data = f.readAll();
    const Palette &pal = dark ? kPaletteDark : kPaletteLight;
    // 令牌在 SVG 里是 fill="@CLOUD@" / stroke="@RAIN@" 这种形式，纯文本替换即可，
    // 不需要走 XML：替换发生在解析之前，替换后的内容仍是合法 SVG
    data.replace("@CLOUD@", pal.cloud);
    data.replace("@CLOUD2@", pal.cloud2);
    data.replace("@CLOUD3@", pal.cloud3);
    data.replace("@RAIN@", pal.rain);

    templateCache().insert(key, data);
    return data;
}

/**
 * 天气代码 → kSlugs 下标（0..kSlugCount-1）。
 * 区间分组沿用 WMO 定义：51-57 毛毛雨、61-67 雨、71-77 雪、80-82 阵雨、
 * 85/86 阵雪、95-99 雷暴；没列出来的代码一律落到「雨」，
 * 与天气描述 weatherText() 的分组保持一致（描述写「雨」的，图标也画雨）。
 * @param wmoCode Open-Meteo 的 weather_code
 * @return kSlugs 的下标，恒在 [0, kSlugCount)
 */
int indexForCode(int wmoCode)
{
    if (wmoCode == 0)
        return 0;
    if (wmoCode == 1)
        return 1;
    if (wmoCode == 2)
        return 2;
    if (wmoCode == 3)
        return 3;
    if (wmoCode == 45 || wmoCode == 48)
        return 4;
    if (wmoCode >= 51 && wmoCode <= 57)
        return 5;
    if (wmoCode >= 61 && wmoCode <= 67)
        return 6;
    if (wmoCode >= 71 && wmoCode <= 77)
        return 7;
    if (wmoCode >= 80 && wmoCode <= 82)
        return 8;
    if (wmoCode == 85 || wmoCode == 86)
        return 9;
    if (wmoCode == 95)
        return 10;
    if (wmoCode == 96 || wmoCode == 99)
        return 11;
    return 6; // 兜底：雨
}

/**
 * 把 SVG 栅格化成 QPixmap（内部带缓存）。
 * @param slugIdx 图标下标
 * @param side    逻辑边长（像素，未乘 DPI）
 * @param dpr     设备像素比，用于高 DPI 清晰度
 * @param dark    是否深色主题
 * @return 像素图的引用；失败时返回一个空的静态 QPixmap（调用方判 isNull 后跳过）
 */
const QPixmap &rasterized(int slugIdx, int side, qreal dpr, bool dark)
{
    // 缓存键：图标 4 bit + 主题 1 bit + 边长 12 bit + DPI×100 的 12 bit
    const quint64 key = quint64(slugIdx) | (dark ? 0x10ull : 0ull)
                        | (quint64(side) << 5) | (quint64(qRound(dpr * 100)) << 17);
    const auto it = pixmapCache().constFind(key);
    if (it != pixmapCache().constEnd())
        return it.value();

    static const QPixmap kNull;
    const QByteArray src = svgSource(slugIdx, dark);
    if (src.isEmpty())
        return kNull;

    QSvgRenderer renderer(src);
    // 解析失败（SVG 被改坏）时不要往缓存里塞空图，否则以后一直画不出来；
    // 每次 miss 都重试解析的代价只发生在出错场景
    if (!renderer.isValid())
        return kNull;

    // 用 QImage 而不是直接画到 QPixmap：QPixmap 依赖平台后端，
    // 在无 GUI 平台上构造会失败（自检/离屏截图场景会用到）
    const int px = qMax(1, qRound(side * dpr));
    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter ip(&img);
        ip.setRenderHint(QPainter::Antialiasing, true);
        // 图标 viewBox 是正方形（16,16,96,96），用正方形目标矩形不会有形变
        renderer.render(&ip, QRectF(0, 0, px, px));
    }
    QPixmap pm = QPixmap::fromImage(img);
    // 逻辑尺寸交回给 Qt：drawPixmap(QPointF, pm) 时按 side 而不是 px 铺开
    pm.setDevicePixelRatio(dpr);

    // 尺寸档位是连续变化的（窗口拖多细就有多少种边长），缓存必须有上限，
    // 否则长时间拖动窗口会把内存吃光。清空比 LRU 简单，重建代价只有一次栅格化
    if (pixmapCache().size() > 96)
        pixmapCache().clear();

    pixmapCache().insert(key, pm);
    return pixmapCache()[key];
}

} // namespace

// 代码到图标的映射统一走 indexForCode()，这里只是把它翻译成文件名，
// 保证「查表」只有一份实现，改分组不会漏改另一处
QString WeatherIcons::slugForCode(int wmoCode)
{
    return QString::fromLatin1(kSlugs[indexForCode(wmoCode)]);
}

void WeatherIcons::paint(QPainter &p, const QRectF &box, int wmoCode, bool dark)
{
    // 太小就别画：1~2px 的图标放不下云朵的描边，只会变成一个脏点
    if (box.width() < 8 || box.height() < 8)
        return;

    // 边长取整：图标是正方形，取宽高较小值并居中，避免非正方形区域里被拉变形
    const int side = qMax(1, int(qMin(box.width(), box.height())));
    // DPR 从画笔的目标设备取：窗口在 125%/150% 缩放下 device 会给出 1.25/1.5，
    // 少了它栅格结果会被放大插值成一圈灰边
    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const QPixmap &pm = rasterized(indexForCode(wmoCode), side, dpr, dark);
    if (pm.isNull())
        return;

    p.save();
    // 图标内部已经是矢量抗锯齿的结果，这里只需在缩放时插值（side 与 pm 逻辑尺寸相等，
    // 实际是 1:1 贴图，SmoothPixmapTransform 只在 DPR 非整数时起作用）
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawPixmap(QPointF(box.center().x() - side / 2.0, box.center().y() - side / 2.0), pm);
    p.restore();
}

void WeatherIcons::clearCache()
{
    templateCache().clear();
    pixmapCache().clear();
}
