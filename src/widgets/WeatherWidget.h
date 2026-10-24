/**
 * @file WeatherWidget.h
 * 天气小组件：走 Open-Meteo 公开接口（免密钥），先地理编码拿经纬度，再取实况 + 未来几天。
 * 结果按城市缓存到数据目录下的 weather_cache.json，断网时回落到缓存并标注「离线缓存」。
 *
 * 关键约定：
 *  - 全内容由 paintEvent 自绘（图标也是矢量画的，不用 QLabel/emoji），所以没有布局开销，
 *    但也意味着所有排版计算都得自己来，见 metrics() 的三档版式；
 *  - 网络全异步：refresh() 只负责发请求，数据回来后在 onXxxDone→applyPayload 里改成员再 update()；
 *  - 定时器按 AppSettings::widgetRefreshMin 分钟触发，0 表示不自动刷新。
 */
#pragma once

#include "widgets/WidgetPanel.h"

#include <QJsonObject>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/**
 * 天气组件：Open-Meteo 免密钥接口，先地理编码再取实况 + 未来三天。
 * 结果会缓存到数据目录，断网时显示上次数据。
 *
 * 整个面板由 paintEvent 直接绘制（图标是矢量画的），因此可以按可用宽高分档排版：
 * 极小尺寸只留「图标 + 温度」，高一些才补描述、体感/湿度、逐日预报、更新时间；
 * 宽扁尺寸改成左右两栏。字体与图标同步缩放，任何尺寸都不会截断或互相遮挡。
 *
 * 内部状态分三层：原始接口数据（m_code/m_tempValue/...）、请求状态（m_busy/m_hasCoord）、
 * 版式（不存成员，每次 paint 由 metrics() 现算），所以改版式不需要额外的失效逻辑。
 *
 * 时序：构造函数末尾只用 QTimer::singleShot(0) 排一次 refresh，把首次请求推迟到事件循环
 * 首轮 —— 宿主 new 完面板才调 restoreState()，太早发请求会拿全局默认城市白跑一次。
 */
class WeatherWidget : public WidgetPanel
{
    Q_OBJECT

public:
    /**
     * 构造：建网络管理器与刷新定时器、读城市设置与磁盘缓存，最后异步发起一次请求。
     * @param parent 父控件，交给 WidgetPanel/QWidget
     */
    explicit WeatherWidget(QWidget *parent = nullptr);

    /**
     * 标题栏文字。
     * @return 有城市名时是「天气 · 城市」，否则只写「天气」；返回空串会让窗口回落到
     *         BoxConfig.title，所以这里始终给一个非空值
     */
    QString panelTitle() const override;

    /**
     * 旧式数字角标。
     * @return 有数据时返回摄氏温度整数值，无数据返回 -1（哨兵 = 不显示）
     * @note 窗口实际只读 badgeText()，这里保留是为了满足基类接口并给只认数字的
     *       标题栏实现留后路，二者取值必须一致（都从 m_tempValue 来）。
     */
    int badgeCount() const override;

    /**
     * 标题栏角标文字。
     * @return 如「20°」；m_descText 为空（还没有拿到任何实况数据）时返回空串表示不显示
     */
    QString badgeText() const override;

    /**
     * 发起一次拉取。
     * @note 有经纬度直连预报接口（一次往返），否则先地理编码（两次往返，成功后由
     *       onGeocodeDone() 再调回来）；m_busy 为 true 期间直接忽略本次调用而非排队。
     */
    void refresh() override;

    /**
     * 往右键菜单追加「更换城市…」「立即刷新」。
     * @param menu 宿主提供的菜单，非空；只 addAction，不要 popup 或接管所有权
     */
    void buildMenu(QMenu *menu) override;

    /**
     * 持久化城市名与经纬度，避免下次启动重新地理编码。
     * @param o 宿主给的 widgetState 副本，写入 city / lat / lon 三个键
     */
    void saveState(QJsonObject &o) const override;

    /**
     * 恢复城市。
     * @param o 上次 saveState() 写出的对象，可能为空（首次创建）或缺字段
     * @note 配置里的城市与当前不同才切换；相同则保持构造结果（构造时取全局
     *       AppSettings::weatherCity 作默认），避免无谓重绘。
     */
    void restoreState(const QJsonObject &o) override;

    /**
     * 最紧凑版式下限：一行放得下图标 + 温度。
     * @return 内容区 142x64（比基类的 150x80 更窄，因为不需要描述行）；
     *         不含标题栏高度，窗口会自己叠加
     */
    QSize minimumSizeHint() const override { return QSize(142, 64); }

    /**
     * 放大上限：再大就只是留白了。
     *
     * 内容量是固定的：头部（图标 + 温度 + 描述 + 体感）约 200px，逐日预报固定 **4 天**
     * （Open-Meteo 请求里 `forecast_days=4`，与界面 4 个槽位一一对应），一行约 48px。
     * 字号在 metrics() 里也已经夹顶（温度 54px、图标 60px，约在内容区高 190 处封顶），
     * 所以超过 480×360 之后窗口只会把内容泡在大片空白里。
     * @return 内容区 480x360；不含标题栏高度，窗口会自己叠加
     * @note 返回无效 QSize（基类默认）表示不限制，这里刻意给死，避免被拖成一整屏的板子。
     */
    QSize maximumSizeHint() const override { return QSize(480, 360); }

    /**
     * 标题栏隐藏时要把城市名画到内容区右上角。
     * @param hidden true = 标题栏已隐藏，内容区上下留白改成 12px（矮窗仍优先取 7px）
     *                并在右上角补画一个城市小标签
     * @note 只在真正变化时重绘，因为宿主每次应用配置都会调一次。
     */
    void setHeaderHidden(bool hidden) override;

protected:
    /**
     * 全部界面：城市标签 / 图标 / 温度 / 描述 / 详情 / 逐日预报 / 更新时间，按 metrics() 分档绘制。
     * @param e 重绘区域，故意忽略 —— 整块重画，不做局部更新（否则换版式会残留旧像素）
     */
    void paintEvent(QPaintEvent *e) override;

private:
    /// 逐日预报的一行。4 个槽位固定存在，缺数据时 valid=false 由绘制侧跳过
    struct Day
    {
        QString label;      // 显示文字：「今天」「明天」「后天」或「周X」；数据缺失时为空
        int code = 0;       // WMO 天气代码，决定图标形状
        int hi = 0;         // 当日最高气温，摄氏度，已四舍五入
        int lo = 0;         // 当日最低气温，摄氏度，已四舍五入
        bool valid = false; // false = 该位置没有拿到数据（接口返回不足 4 天或字段缺失）
    };

    /**
     * 由当前宽高推导出的排版参数。
     * 所有尺寸都是像素（不是磅），每次 paintEvent 现算，不缓存 —— 因此改窗口大小
     * 自然就换版式，不需要额外的状态失效处理。
     *
     * 不变量：compact 与 wide 互斥（wide 的成立条件里带了 !compact），竖排是两者都不成立
     * 时的兜底；三个分支各自 return，绘制时不会互相覆盖。
     */
    struct Metrics
    {
        int marginX = 14;    // 内容区左右留白，像素；窄窗会缩到 10
        int marginY = 10;    // 内容区上下留白，像素；矮窗 7，隐藏标题栏时 12
        int glyph = 44;      // 主天气图标边长，像素；绘制时按可用空间在 14..60 之间缩放
        int tempPx = 38;     // 温度字号，像素；范围 18..54
        int descPx = 13;     // 天气描述字号，像素；范围 9..15
        int detailPx = 11;   // 「体感/湿度/风」详情字号，像素；范围 9..12
        int dayPx = 11;      // 逐日预报字号，像素；范围 9..12
        int updatedPx = 10;  // 更新时间与城市小标签字号，像素；范围 8..11
        bool wide = false;   // 左右两栏（宽且不太高）
        bool compact = false; // 单行（图标 + 温度 + 描述）；优先级高于 wide
    };

    /**
     * 按当前 width()/height() 推导版式与各级字号。
     * @return 本次绘制要用的参数；构造期间尺寸未定，不要在构造函数里调用
     */
    Metrics metrics() const;

    /**
     * 预留的拉取入口：当前实现里既没有定义也没有调用，实际请求逻辑在 refresh() 中。
     * 保留声明只为不改动既有接口，新增逻辑请直接放进 refresh()。
     */
    void fetch();

    /**
     * 地理编码回调：从结果里取第一个城市的经纬度，然后重新走一次 refresh()。
     * @param reply 已完成的响应；调用方在这之前已 deleteLater()，本函数内仍可安全读取
     */
    void onGeocodeDone(QNetworkReply *reply);

    /**
     * 预报回调：成功则写界面并落盘缓存，失败只改底部状态文字（不清掉旧数据）。
     * @param reply 已完成的响应；同样由调用方 deleteLater()，本函数内可读
     */
    void onForecastDone(QNetworkReply *reply);

    /**
     * 把接口 JSON（或缓存里的 payload）解析进成员变量并刷新界面。
     * @param root      原始接口 JSON；缓存路径下调用方会额外塞入 _fetchedAt（秒级时间戳）
     * @param fromCache true = 来自磁盘缓存，更新时间显示成「离线缓存 · 时间」而不是「更新于 时间」
     */
    void applyPayload(const QJsonObject &root, bool fromCache);

    /**
     * 记录一条错误提示，只改底部状态行：已有数据时提示「显示上次数据」，没数据时提示可右键重试。
     * @param text 面向用户的短句，如「网络不可用」
     */
    void showError(const QString &text);

    /**
     * 切换城市。
     * @param city            新城市名（中文或英文，直接拼进地理编码请求）
     * @param coordinateKnown true = m_lat/m_lon 已随之有效，下次 refresh() 跳过地理编码
     */
    void setCity(const QString &city, bool coordinateKnown);

    /// 缓存文件绝对路径：<数据目录>/weather_cache.json（不保证存在）
    QString cacheFile() const;
    /// 读缓存并直接套用到界面；缓存里的城市与当前城市不一致时整份丢弃
    void loadCache();
    /**
     * 写缓存（含城市、经纬度、抓取时间）。
     * @param payload 接口原始 JSON，原样存到 "payload" 键下
     */
    void writeCache(const QJsonObject &payload);

    QNetworkAccessManager *m_net = nullptr; // 网络管理器，parent 是本控件，随控件销毁
    QTimer *m_timer = nullptr;              // 自动刷新定时器；widgetRefreshMin == 0 时不启动（对象仍会创建，只是不设间隔也不 connect）

    QString m_cityName;       // 当前城市名；空表示还没选城市（此时标题只显示「天气」）
    double m_lat = 0;         // 纬度，WGS84 十进制度；与 m_lon 同时为 0 视为「未知」
    double m_lon = 0;         // 经度，WGS84 十进制度
    bool m_hasCoord = false;  // true = 经纬度可信，refresh() 可跳过地理编码这一步
    bool m_busy = false;      // true = 已有请求在飞，refresh() 直接返回，防止重复拉取
    bool m_headerHidden = false; // 标题栏是否隐藏；隐藏后要在内容区右上角补画城市名

    int m_code = 0;         // WMO 天气代码（0 晴 … 95+ 雷暴），决定图标与中文描述
    int m_tempValue = 0;    // 当前气温，摄氏度，已四舍五入成整数（显示与角标都用它）
    int m_feels = 0;        // 体感温度，摄氏度，已四舍五入
    int m_humidity = 0;     // 相对湿度，百分比 0-100，取整
    int m_wind = 0;         // 风速，km/h，已四舍五入（接口给的是 km/h）
    QString m_descText;     // 天气中文描述（如「多云」）；空串同时被当作「还没有数据」的标志
    QString m_updatedText;  // 底部状态行：「正在获取…」「更新于 14:20」「离线缓存 · 09-01 08:00」等
    QVector<Day> m_days;    // 逐日预报，固定 4 项，[0] 是今天；按需绘制，缺失项 valid=false
};

