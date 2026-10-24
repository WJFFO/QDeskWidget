#pragma once

#include <QColor>
#include <QFont>
#include <QVector>
#include <QWidget>

// 自绘歌词视图：内部维护「句 → 视觉行」的两级结构。QTextLayout 按控件宽度把每句折成
// 若干行（m_rows），m_lineFirstRow 记录每句的起始行号，滚动单位是「行」而不是「句」。
// 陷阱：m_times 必须升序（调用方依赖 parseLrc 的排序），否则 currentLineFor 的线性扫描会错；
// 宽度/字号变化必须 rebuild 后立刻 updateTarget(false)，否则滚动位置会停在旧行号上。

class QVariantAnimation;
class QTimer;

/**
 * 歌词视图（自绘）。
 *
 * 与「一行 QLabel + 省略号」不同，这里会：
 *   - 按当前宽度把每句歌词**自动折行**，长句不会只显示半截；
 *   - 显示当前句以及上下文多行（当前句在偏上位置，下面的句子用来「预告」）；
 *   - 当前句加粗高亮，其它行按距离淡出；
 *   - 换句时用短动画平滑滚动，而不是瞬间跳。
 *
 * 大档额外开放「时间列 + 点击跳转 + 滚轮翻阅」（setSeekable），交互对齐网易云歌词页。
 */
class LyricsView : public QWidget
{
    Q_OBJECT

public:
    /**
     * 构造：装好滚动动画（220ms OutCubic）与初始字号/配色，此时还没有歌词。
     * @param parent 父控件；本控件对鼠标完全透明，点击会穿透给父级
     * 注意动画对象在构造期就建好，因为 setTextPixelSize 内部会调 updateTarget 用到它。
     */
    explicit LyricsView(QWidget *parent = nullptr);

    /**
     * 设置带时间轴的歌词。
     * @param lines (毫秒, 该句文本) 列表，**必须按时间升序**（调用方依赖 parseLrc 的排序），
     *              否则 currentLineFor 的线性扫描会定位错；空列表等同于没有歌词
     * 副作用：清掉纯文本状态、重建行结构、把 m_currentLine 复位成 -1 等下一次 setPosition。
     */
    void setLyrics(const QVector<QPair<qint64, QString>> &lines);
    /**
     * 设置没有时间轴的纯文本歌词：不做高亮、不滚动，按 \n 分行后折行静态显示。
     * @param text 全文；空行会被丢弃（无句概念，空行没有意义）
     */
    void setPlainText(const QString &text);
    /** 清空内容并复位滚动/当前句；不清 placeholder（那是外部设置的提示语） */
    void clear();

    /**
     * 一个「视觉行」的像素高度（字号 + 行距）。
     * 布局方用 height() / rowHeight() 反推当前能显示几行；
     * 想让歌词区正好是整数行时，也用 rowHeight() 的整数倍设固定高度。
     * @return 行高，恒 >= 1
     */
    int rowHeight() const { return m_rowH; }

    /**
     * 通知当前播放位置，用于决定高亮哪一句。
     * @param ms 播放进度（毫秒），可小于 0 或超过总时长，内部按行首时间做线性查找并夹到 [0, 最后一句]
     * 只在「句号变化」时才触发滚动动画，同一句内的连续调用基本无开销（这是每 300ms 被调用的热点路径）。
     */
    void setPosition(qint64 ms);

    /**
     * 设置正文字号。
     * @param px 像素字号，内部夹到 >= 9；上下文行用 px-2（夹到 >= 8）
     * 字号不变时直接返回（避免整首重新折行）；变化则重建行结构并立即重算滚动目标。
     */
    void setTextPixelSize(int px);
    /**
     * 设置两种文字颜色。
     * @param current 当前句颜色（加粗那句）
     * @param other   上下文基准色，实际绘制时会按离当前行的距离再叠 alpha 衰减
     */
    void setColors(const QColor &current, const QColor &other);
    /** 没有歌词时显示的提示文字 */
    void setPlaceholder(const QString &text);

    /**
     * 设置每行文字的水平对齐。
     * @param a 只有 Qt::AlignLeft / Qt::AlignHCenter / Qt::AlignRight 有意义，其它位被忽略
     *
     * 小档没有歌名做参照，居中最好看；大档的歌名/歌手是左对齐的，
     * 歌词再居中就会让首行左边缘缩进去，和歌名不在一条竖线上，所以大档用左对齐。
     */
    void setTextAlignment(Qt::Alignment a);

    /**
     * 紧凑模式：只画**当前这一句**（长句仍会折行），整体垂直居中、不做上下文滚动。
     * 用于窗口很矮、右侧只够放一行歌词的场景。
     * @param on true 进入紧凑模式（滚动强制归零）；false 恢复上下文多行
     */
    void setCompact(bool on);
    /** @return 当前是否紧凑模式（只画当前句） */
    bool isCompact() const { return m_compact; }

    /** 是否有可绘制内容（已折好行）；用于外部判断「该显示歌词还是显示歌名占位」 */
    bool hasLyrics() const { return !m_rows.isEmpty(); }

    /** 最低 64×24：再窄折行会退化成每字一行，再矮连一句完整的都放不下 */
    QSize minimumSizeHint() const override { return QSize(64, 24); }

    /**
     * 时间列是否显示（**右侧**那一列 mm:ss）。
     * 默认隐藏：只在「鼠标悬停 + 滚轮翻阅」时临时出现，鼠标移开或停手 3s 后自动收起，
     * 免得平时一直在歌词右边挂一串时间。
     * @param on true 显示并重新折行（正文要给时间列让出宽度）
     *
     * 显隐会改变折行宽度，所以内部要 rebuild —— 但**不重算滚动目标**：
     * 用户此刻正在翻阅，把他滚到的位置拽回「当前句居中」就等于白翻了。
     */
    void setTimeColumnVisible(bool on);
    /** @return 时间列当前是否可见 */
    bool isTimeColumnVisible() const { return m_timeVisible; }

    /**
     * 打开「时间列 + 点击跳转 + 滚轮翻阅」（大档用，交互对齐网易云歌词页）。
     * @param on true：每行右侧可按需显示该句时间、鼠标可点可滚；false：纯展示，不吃鼠标事件
     *
     * 打开后控件会把鼠标标记成可交互（`qdwInteractive` 动态属性），
     * 这样「隐藏标题栏」的拖动事件过滤器不会把点击歌词当成拖窗口。
     */
    void setSeekable(bool on);
    /** @return 是否处于可交互（时间列 + 点击 seek + 滚轮）模式 */
    bool isSeekable() const { return m_seekable; }

signals:
    /**
     * 用户点了某一行歌词，请求从这句开头开始播。
     * @param ms 该句的起始时间（毫秒）；纯文本模式不会发出
     */
    void seekRequested(qint64 ms);

protected:
    /**
     * 自绘入口：无内容画提示语；紧凑模式只画当前句；普通模式画可见行并按距离淡出。
     * @param e 重绘区域。实现里忽略它，直接按控件完整矩形绘制（行是按滚动位置整体平移的，
     *          局部重绘反而要先算行号范围，收益不明显）
     */
    void paintEvent(QPaintEvent *e) override;
    /**
     * 宽度变化 = 全部重新折行，行号索引整体失效，必须 rebuild 后立刻重算滚动目标。
     * @param e resize 事件，仅用于转发给基类
     */
    void resizeEvent(QResizeEvent *e) override;
    /**
     * 悬停高亮：只有可交互模式下才跟踪鼠标，命中有效行时把光标改成手型。
     * @param e 鼠标移动事件，坐标单位是控件像素
     */
    void mouseMoveEvent(QMouseEvent *e) override;
    /**
     * 左键点击某一句 → emit seekRequested(该句起始时间)。
     * @param e 鼠标释放事件；没落在有效行上时交回基类，不影响窗口拖动
     */
    void mouseReleaseEvent(QMouseEvent *e) override;
    /** 鼠标离开时清掉高亮行，否则那一行会一直亮着 */
    void leaveEvent(QEvent *e) override;
    /**
     * 滚轮手动翻阅歌词（可交互模式）。
     * @param e 滚轮事件；一格的滚动量固定为一行，滚完 2.5s 会自动恢复跟随当前句
     * 未开交互时把事件交回基类，交给窗口层处理（窗口可能用它切换组合成员）。
     */
    void wheelEvent(QWheelEvent *e) override;

private:
    /** 折行后的一个视觉行；一句歌词可能对应多个 Row */
    struct Row
    {
        QString text;  // 该视觉行的文本（已按宽度折好，绘制时不再二次排版）
        int line = -1; // 属于第几句歌词（-1 表示纯文本模式）
    };

    /** 按当前宽度把所有句子重新折行，重建 m_rows 与 m_lineFirstRow（不改滚动位置） */
    void rebuild();
    /**
     * 计算并（可选）动画滚动到「当前句居中」的目标行。
     * @param animate true 用 220ms 动画过渡；false 直接跳到位（换歌、改尺寸时用，避免整屏滑动）
     * 紧凑模式下会把滚动强制归零。
     */
    void updateTarget(bool animate);
    /** 线性扫描（一首歌几百句，二分反而更啰嗦）：返回 ms 时刻应高亮的句下标；无时间轴返回 -1，早于第一句返回 0 */
    int currentLineFor(qint64 ms) const;

    /**
     * 绘制时使用的顶部偏移（像素）。
     * @return m_scrollRow × 行高，再叠加「歌词不足一屏时整体垂直居中」的修正
     *
     * 绘制与命中测试必须用同一个值，否则点击位置会和画出来的行对不上。
     */
    qreal scrollTop() const;
    /**
     * 把控件内的 y 像素换算成视觉行下标（scrollTop 的逆运算）。
     * @param y 控件坐标（像素）
     * @return 命中的行下标；落在空白处或越界返回 -1
     */
    int rowAtY(int y) const;
    /** @return 毫秒时间戳的 mm:ss 文本，用于时间列 */
    static QString timeText(qint64 ms);

    QVector<Row> m_rows;          // 全部视觉行，绘制与滚动的基本单位
    QVector<int> m_lineFirstRow;  // 每句歌词的首个视觉行下标；纯文本模式为空
    QVector<qint64> m_times;      // 每句起始时间（毫秒），必须升序
    QVector<QString> m_texts;     // 每句原文（未折行），紧凑模式取整句做省略时用
    bool m_plain = false;         // true = 无时间轴纯文本，不做高亮/滚动

    QString m_placeholder;        // 无内容时画在中间偏左的提示语
    QString m_plainText;          // 纯文本模式的原始内容（按 \n 再折行）

    int m_currentLine = -1;       // 当前句下标；-1 表示未定位（刚设歌词、还没收到播放位置）
    qreal m_scrollRow = 0;        // 当前滚动位置，单位「行」，可为小数（动画插值结果）
    qreal m_targetRow = 0;        // 最近一次算出的目标滚动位置（单位「行」）；目前只写不读，实际滚动由 m_scrollRow 承担
    int m_rowH = 18;              // 单行高度 = 粗体行高 + 3，行距不另外设置
    int m_textPx = -1;            // 已应用的字号；-1 表示还没设过（保证首次调用必定生效）
    bool m_compact = false;       // 只画当前句的模式
    Qt::Alignment m_align = Qt::AlignHCenter; // 每行文字的水平对齐；默认居中（小档）

    bool m_seekable = false;      // 是否开放时间列 / 点击 seek / 滚轮翻阅（大档）
    bool m_timeVisible = false;   // 右侧时间列当前是否显示（按需出现，默认收起）
    int m_hoverRow = -1;          // 鼠标悬停的视觉行下标；-1 表示没有
    bool m_userScrolling = false; // 用户用滚轮翻阅中：期间不跟随当前句，避免视图被拽回去
    QFont m_fontTime;             // 时间列字体（比正文小一号）
    int m_gutter = 0;             // 时间列占用的总宽度（= 时间宽 + 间距，px）；0 表示不显示
    int m_timeGap = 0;            // 时间文字与歌词正文之间的间距（px），m_gutter 的一部分
    QTimer *m_resumeTimer = nullptr;   // 滚轮停手 2.5s 后恢复跟随当前句
    QTimer *m_timeHideTimer = nullptr; // 时间列「最后一滚」之后 3s 自动收起

    QFont m_font;                 // 占位文字与常规字号基准
    QFont m_fontBold;             // 当前句
    QFont m_fontSmall;            // 唱过 / 没唱到的上下文，小一号
    QColor m_curColor;            // 当前句颜色
    QColor m_otherColor;          // 上下文基准色（绘制时再叠 alpha）

    QVariantAnimation *m_anim = nullptr; // 滚动动画；构造期先建好，setTextPixelSize 会用到
};
