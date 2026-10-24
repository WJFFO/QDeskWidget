#pragma once

#include <QPixmap>
#include <QTimer>
#include <QWidget>

// 圆形专辑封面控件：把源图预切成圆图（m_disc），paintEvent 里先铺一整块黑色圆，
// 再把圆图按 ring 厚度内缩着画上去，于是露出的黑边就是「唱片外圈」。
// 旋转只作用在圆图上，黑环在屏幕坐标系里始终不动 —— 否则整块图会跟着晃。
// 尺寸是固定尺寸（Fixed/Fixed），由外部（MusicWidget::applyMetrics）调 setDiameter 驱动。

/**
 * 圆形专辑封面：播放时会缓慢旋转（像唱片），暂停时定格。
 *
 * 旋转只影响图像本身，外圈描边是不动的 —— 这样转起来不会整块图案跟着晃。
 */
class CoverView : public QWidget
{
    Q_OBJECT

public:
    /**
     * 构造：固定尺寸策略 + 30ms 旋转定时器（先不启动，等 setSpinning + 可见才转）。
     * @param parent 父控件；本控件对鼠标透明，点击穿透给父级
     */
    explicit CoverView(QWidget *parent = nullptr);

    /**
     * 设置封面源图，并按当前直径立即重切圆图。
     * @param pixmap 原始封面（任意尺寸/比例，内部居中裁成正方形）；传空图会画一个音符占位
     */
    void setCover(const QPixmap &pixmap);
    /**
     * 设置圆直径。
     * @param diameter 目标直径（像素），内部夹到最小 20；尺寸没变且已有圆图时直接返回，
     *                 避免拖窗口时反复做平滑缩放
     */
    void setDiameter(int diameter);
    /** @return 当前直径（像素），含黑色外圈；最小 20 */
    int diameter() const { return m_d; }

    /**
     * 播放/暂停切换。
     * @param spinning true 开始转（仅当控件可见才真正启动定时器）；false 立即停表并定格
     */
    void setSpinning(bool spinning);
    /** @return 期望的旋转状态；控件不可见时即使为 true，定时器也是停的 */
    bool isSpinning() const { return m_spin; }

    /** @return m_d×m_d，与固定尺寸保持一致 */
    QSize sizeHint() const override;
    /** @return 20×20，与 setDiameter 内部的下限一致 */
    QSize minimumSizeHint() const override;

protected:
    /**
     * 绘制顺序不能反：先整圆填实心黑 → 再在内圆画封面/占位。
     * 反过来黑边会盖住封面；旋转也只包住封面那一段，黑环不参与。
     * @param e 重绘区域，实现里忽略（整块圆都要重画）
     */
    void paintEvent(QPaintEvent *e) override;
    /**
     * 显示时才可能开始转：构造期 setSpinning(true) 往往控件还没显示。
     * @param e show 事件，转发给基类
     */
    void showEvent(QShowEvent *e) override;
    /**
     * 隐藏即停表：不可见时的旋转既看不见又白耗 CPU。
     * @param e hide 事件，转发给基类；不重置 m_angle，重新显示时从原角度继续
     */
    void hideEvent(QHideEvent *e) override;

private:
    /** 用 m_src + 当前 m_d 重新生成 m_disc；源图为空时清掉圆图，交给 paintEvent 画音符占位 */
    void rebuild();
    /** 只有「要转且可见」才开定时器，其它情况停表（不可见时旋转毫无意义且白耗 CPU） */
    void refreshTimer();
    /**
     * 把任意尺寸的源图处理成 size×size 的圆图。
     * @param src  原始封面，可能非正方形，按 KeepAspectRatioByExpanding 居中裁切后再做圆
     * @param size 目标边长（像素）
     * @return 透明底 + 圆形裁切后的位图；不做黑边，黑边由 paintEvent 负责
     */
    static QPixmap circularDisc(const QPixmap &src, int size);

    QPixmap m_src;   // 原始封面，换尺寸时用它重新生成圆图，避免重采样多次后糊掉
    QPixmap m_disc;  // 已切成圆形的缓存；空 = 没有封面，画音符占位
    int m_d = 64;    // 控件直径（像素），同时决定控件固定尺寸
    qreal m_angle = 0; // 当前旋转角（度），取值 [0, 360)，到 360 就减回去防止无限增长
    bool m_spin = false; // 期望的旋转状态，实际是否在转还取决于 isVisible()
    QTimer m_timer;      // 30ms 步进，每帧 +1°，约 11 秒一圈
};
