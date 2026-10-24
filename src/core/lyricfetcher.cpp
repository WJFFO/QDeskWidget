// -----------------------------------------------------------------------------
// lyricfetcher —— 网易云 LRC 歌词抓取实现（LyricFetcher 的实现文件）。
// 职责：两步 HTTP（搜索 → 取词）的串接、结果打分选曲、缓存与去重。
// 约定：所有回调都通过 reply 上的 "title"/"artist" 属性回传原始请求参数，
//       不从 reply 的 URL 反推，避免二次请求时参数串味。
// 坑：  析构顺序 + abort 顺序都要对（见 ~LyricFetcher），否则在飞请求的 finished
//       会打到已析构对象上；两步请求共用同一个 m_nam，因此中途析构会同时打断两条链。
// -----------------------------------------------------------------------------
#include "lyricfetcher.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

namespace {
/**
 * 生成缓存 key。
 *
 * @param title  歌名（原样，不做大小写/空白归一化 —— 归一化会让"同名不同版本"互相污染）。
 * @param artist 歌手，可为空。
 * @return title + '\x1' + artist。用控制字符 \x1 作分隔，避免
 *         ("AB","C") 与 ("A","BC") 拼出同一个 key。
 */
QString cacheKey(const QString &title, const QString &artist)
{
    return title + QChar(0x1) + artist;
}
}

// 构造函数不做事：m_nam 是成员对象，无需手动 new，也就没有"忘记 delete"的风险。
LyricFetcher::LyricFetcher(QObject *parent)
    : QObject(parent)
{
}

// 析构的两步顺序不能换：先断信号，再 abort。
// abort() 会同步 emit finished；若先 abort 后 disconnect，回调会在析构中途执行。
LyricFetcher::~LyricFetcher()
{
    // 析构期间不要再有任何网络回调回到本对象：
    // 先断开管理器与所有在飞 reply 的信号，再 abort（此时 abort 已不会 emit 到我们）。
    disconnect(&m_nam, nullptr, this, nullptr);

    // findChildren 拿到的是 m_nam 的所有 reply 子对象；逐个再断一次是因为
    // reply 自身的 finished 是直接连到 this 的 lambda，不受上面那行 disconnect 影响。
    const QList<QNetworkReply *> replies = m_nam.findChildren<QNetworkReply *>();
    for (QNetworkReply *reply : replies)
    {
        if (!reply)
            continue;
        reply->disconnect(this);
        reply->abort();
    }
}

// 抓歌词主流程：空标题/命中缓存/在飞 三种短路都在最前面，保证下游只处理真正的请求。
//
// @param title  见头文件；这里只用 trimmed 判空，实际请求仍用原始串（保留空格不影响搜索）。
// @param artist 可为空，拼关键词时为空会多出一个前导空格，被 trimmed 吃掉。
void LyricFetcher::fetch(const QString &title, const QString &artist)
{
    if (title.trimmed().isEmpty())
    {
        emit lyricReady(title, artist, QString());
        return;
    }

    const QString key = cacheKey(title, artist);

    // 命中缓存
    if (m_cache.contains(key))
    {
        emit lyricReady(title, artist, m_cache.value(key));
        return;
    }
    // 已在请求中
    // 静默丢弃而不是排队补发：同一首歌词在飞期间再请求一次不会有新信息
    // （典型来源是 A→B→A 快速切歌），补发只会让歌词面板多闪一次。
    if (m_inflight.value(key, false))
        return;
    // 未给请求设超时（QNetworkRequest 的 transferTimeout 默认 0 = 不超时）：
    // 极端情况下 finished 迟迟不来，这个 key 会一直留在 m_inflight，
    // 同曲的后续请求都被静默丢弃，直到那条请求真正结束。
    m_inflight.insert(key, true);

    // 1. 搜索歌曲
    // 关键词按「歌手 + 空格 + 歌名」拼（与 C# 桥接版一致）；artist 为空时只会多出一个
    // 前导空格，由末尾的 trimmed() 吃掉，不会把关键词拼成 " 歌名"。
    const QString keyword = (artist + QLatin1Char(' ') + title).trimmed();
    QUrl url(QStringLiteral("https://music.163.com/api/search/get/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("s"), keyword);
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("1"));       // 1 = 单曲
    query.addQueryItem(QStringLiteral("offset"), QStringLiteral("0"));     // 从第一条开始
    query.addQueryItem(QStringLiteral("total"), QStringLiteral("true"));   // 需要总数（接口默认可能不返回）
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));     // 取前 10 条候选，足够覆盖翻唱/同名
    url.setQuery(query);

    // 这两个头是照网页端请求补的：该接口对来源（Referer）校验较严，
    // 用默认空 UA 的裸请求容易拿到空结果或被限流，所以两次请求都要带上。
    QNetworkRequest req(url);
    req.setRawHeader("Referer", "https://music.163.com/");
    req.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36");

    QNetworkReply *reply = m_nam.get(req);
    // 把请求参数挂在 reply 上：第二个请求（取词）需要原封不动地把 title/artist 带回，
    // 而它的 URL 里只有歌曲 id，反推不出来。
    reply->setProperty("title", title);
    reply->setProperty("artist", artist);
    // 用 this 作 context：本对象析构时连接自动断开，回调不会再进来
    // （析构里还会主动 disconnect + abort 兜底，两道保险）。
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]()
            {
                const QString t = reply->property("title").toString();
                const QString a = reply->property("artist").toString();
                // 先 deleteLater 再读数据：readAll() 之后 reply 的内容仍在内存里，
                // deleteLater 只是排队删除，不会影响本帧后续访问。
                reply->deleteLater();

                // 以下每个失败分支都必须 m_inflight.remove，否则这首歌会被永久卡在「请求中」，
                // 之后所有 fetch 都被开头那句直接 return 掉。
                if (reply->error() != QNetworkReply::NoError)
                {
                    m_inflight.remove(cacheKey(t, a));
                    emit lyricReady(t, a, QString());
                    return;
                }

                const QByteArray data = reply->readAll(); // 一次性读完：搜索结果是普通小 JSON，没有流式解析的需求
                const QJsonDocument doc = QJsonDocument::fromJson(data);
                if (!doc.isObject())
                {
                    // 返回非 JSON（常见于被限流时返回 HTML 错误页）
                    m_inflight.remove(cacheKey(t, a));
                    emit lyricReady(t, a, QString());
                    return;
                }

                const QJsonObject root = doc.object();
                const QJsonObject result = root.value(QStringLiteral("result")).toObject();
                // 字段缺失（搜不到/接口改版/被限流）时这里是空数组，最终会走到 bestId == 0 的分支
                const QJsonArray songs = result.value(QStringLiteral("songs")).toArray();

                // 打分挑最佳曲目：没有候选时保持 0，后面据此判"没找到"。
                qint64 bestId = 0;
                int bestScore = -1; // -1 保证第一首（哪怕得 0 分）也会被选中，作为兜底
                for (const QJsonValue &v : songs)
                {
                    const QJsonObject song = v.toObject();
                    const qint64 id = song.value(QStringLiteral("id")).toVariant().toLongLong();
                    const QString name = song.value(QStringLiteral("name")).toString();
                    QStringList artistNames;
                    const QJsonArray arr = song.value(QStringLiteral("artists")).toArray();
                    for (const QJsonValue &av : arr)
                        artistNames << av.toObject().value(QStringLiteral("name")).toString();
                    const QString artists = artistNames.join(QLatin1Char('/'));

                    // 打分权重：歌名全等 3 > 歌手命中 2 > 歌名互相包含 1。
                    // 歌名全等权重最高是因为翻唱/伴奏同名极多，歌名对了歌词基本就对了；
                    // 歌手只做加权不做一票否决——很多歌在平台上挂的是"群星"或合作歌手拼接。
                    int score = 0;
                    if (name == t)
                        score += 3;
                    else if (!name.isEmpty() && (name.contains(t) || t.contains(name)))
                        score += 1;
                    if (!a.isEmpty() && artists.contains(a))
                        score += 2;

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestId = id;
                    }
                }

                if (bestId == 0)
                {
                    // 0 是"没有有效 id"的哨兵：接口正常但没搜到任何歌曲
                    m_inflight.remove(cacheKey(t, a));
                    emit lyricReady(t, a, QString());
                    return;
                }

                // 2. 取歌词
                // lv/kv/tv 三个参数分别对应"原文/翻译/罗马音"歌词；-1 表示不返回。
                // 这里只要原文，翻译由上层自己按需忽略。
                QUrl lyricUrl(QStringLiteral("https://music.163.com/api/song/lyric"));
                QUrlQuery lq;
                lq.addQueryItem(QStringLiteral("id"), QString::number(bestId));
                lq.addQueryItem(QStringLiteral("lv"), QStringLiteral("-1"));
                lq.addQueryItem(QStringLiteral("kv"), QStringLiteral("-1"));
                lq.addQueryItem(QStringLiteral("tv"), QStringLiteral("-1"));
                lyricUrl.setQuery(lq);

                QNetworkRequest lreq(lyricUrl);
                lreq.setRawHeader("Referer", "https://music.163.com/");
                lreq.setRawHeader("User-Agent",
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/120.0 Safari/537.36");

                QNetworkReply *lreply = m_nam.get(lreq);
                lreply->setProperty("title", t);
                lreply->setProperty("artist", a);
                connect(lreply, &QNetworkReply::finished, this,
                        [this, lreply]()
                        {
                            const QString t2 = lreply->property("title").toString();
                            const QString a2 = lreply->property("artist").toString();
                            lreply->deleteLater();

                            QString text;
                            // 只判 error()：Qt 会把 HTTP 4xx/5xx 折算成对应的 reply error，
                            // 这里不必再看状态码；解析失败时下面的 value() 链自然落成空串。
                            if (lreply->error() == QNetworkReply::NoError)
                            {
                                // 结构：{ "lrc": { "lyric": "[00:00.00]..." }, "tlyric": {...} }
                                // 纯音乐/无版权时 lrc 字段可能缺失或 lyric 为空，这里统一落成空串。
                                const QJsonDocument d =
                                    QJsonDocument::fromJson(lreply->readAll());
                                text = d.object()
                                           .value(QStringLiteral("lrc"))
                                           .toObject()
                                           .value(QStringLiteral("lyric"))
                                           .toString();
                            }

                            // 无论成功失败都写缓存：空串也缓存，避免对"确实没有歌词"的歌
                            // 每次换曲都重新发两次请求。
                            m_cache.insert(cacheKey(t2, a2), text);
                            m_inflight.remove(cacheKey(t2, a2));
                            emit lyricReady(t2, a2, text);
                        });
            });
}
