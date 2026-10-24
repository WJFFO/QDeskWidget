// -----------------------------------------------------------------------------
// lyricfetcher —— 网易云 LRC 歌词抓取器的头文件。
// 职责：按「歌名 + 歌手」搜歌取 id，再拉该曲歌词，结果通过 lyricReady 异步回传。
// 约定：同一「歌名+歌手」只抓一次。第一段（搜索）失败时只把 key 从 m_inflight 移除，
//       不写缓存，下次还会重试；第二段（取词）无论成功与否都写缓存（空词也写），
//       避免对「确实没有歌词」的歌反复发两次请求。
// 坑：  m_nam 必须声明在 m_cache / m_inflight 之后 —— 成员按声明逆序析构，
//       这样 m_nam 先析构；否则 ~QNetworkAccessManager 取消在飞请求时 emit finished，
//       回调会写已经销毁的容器。
// -----------------------------------------------------------------------------
#ifndef LYRICFETCHER_H
#define LYRICFETCHER_H

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QHash>

/**
 * 通过网易云公开 API 抓取 LRC 歌词。
 *
 * 流程（与 qtest 的 C# 桥接一致）：
 *   1. 按"歌手 歌名"搜索歌曲（music.163.com/api/search/get）
 *   2. 匹配最佳曲目 id
 *   3. 取该曲目的歌词（music.163.com/api/song/lyric）
 *
 * 带缓存：同一"歌名+歌手"只抓一次。
 */
class LyricFetcher : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造
     * @param parent Qt 父对象；通常的主控件，随之销毁。
     * 构造不做任何网络 IO，m_nam 是成员对象，随本类一起构造/析构。
     */
    explicit LyricFetcher(QObject *parent = nullptr);

    /**
     * @brief 析构
     * 先断开 m_nam 与所有在飞 reply 到本对象的信号，再 abort，保证析构期间
     * 不会有 finished 回调打到半销毁的 this 上（详见 .cpp 里的顺序说明）。
     */
    ~LyricFetcher() override;

    /**
     * 请求某首歌的歌词（异步，结果通过 lyricReady 信号返回）
     *
     * @param title  歌名；trimmed 后为空则直接 emit lyricReady(title, artist, 空串) 并返回，
     *               不发起网络请求。
     * @param artist 歌手；可为空（此时只按歌名搜、只按歌名打分）。
     *
     * 时序：命中缓存 → 同一次事件循环内同步 emit；未命中 → 返回后异步 emit（可能两次
     * 网络往返之后）。相同 title+artist 的并发请求会被合并，只有一次 emit
     * （后来的调用直接 return，不会补发）。
     */
    void fetch(const QString &title, const QString &artist);

signals:
    /**
     * 歌词获取完成
     * @param title  原样回传请求时的歌名
     * @param artist 原样回传请求时的歌手
     * @param text   LRC 全文（含 [mm:ss.xx] 时间标签）；未找到/网络失败/解析失败时为空串
     */
    void lyricReady(const QString &title, const QString &artist, const QString &text);

private:
    QHash<QString, QString> m_cache; // key = title + '\x1' + artist；value 为 LRC 全文，空串代表「确认无词」。
                                     // 不做容量上限/淘汰：条目数等于本次运行听过的曲目数，量级可忽略，
                                     // 换来的是反复切歌时不必重复发网络请求。
    QHash<QString, bool> m_inflight; // 在飞集合：value 恒为 true，只用来 contains / value(key, false) 判重

    // 放最后：成员按声明逆序析构，m_nam 必须先于上面两个容器析构。
    // 否则 ~QNetworkAccessManager 取消在飞请求时会 emit finished，
    // 回调里再去写已经销毁的 m_cache / m_inflight。
    QNetworkAccessManager m_nam;
};

#endif // LYRICFETCHER_H
