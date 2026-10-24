#pragma once

#include <QString>

class QPainter;
class QRectF;

/**
 * @file WeatherIcons.h
 * 天气图标渲染：把 resources/weather/*.svg（Meteocons line 变体，MIT）按需栅格化后贴到画笔上。
 *
 * 为什么不直接在 paintEvent 里逐笔手绘：
 *   - 手绘那套（旧实现）线条比例靠人调，14px 与 60px 两档永远有一档不像；
 *   - SVG 是设计稿原文，任意尺寸都按同一份几何缩放，视觉一致。
 *
 * 关键约束：
 *   - 图标颜色分「浅色主题 / 深色主题」两套：原版云朵是 #e6effc（近白），
 *     在浅色玻璃上等于隐形，所以 SVG 里把可变颜色写成 @CLOUD@ 之类的令牌，
 *     加载时按主题替换（见 palette()）；
 *   - 栅格化必须带 devicePixelRatio，否则 125% 缩放下图标会糊一圈；
 *   - 结果进内存缓存（按 图标+边长+缩放+主题 做键），paintEvent 里只做查表 + drawPixmap。
 */
class WeatherIcons
{
public:
    /**
     * 在指定矩形里画一个天气图标。
     * @param p      目标画笔；内部会 save/restore，不改调用方的画笔/画刷状态
     * @param box    图标外接矩形；取宽高较小值作为正方形边长，在矩形里居中绘制
     * @param wmoCode WMO 4677 天气代码（Open-Meteo 的 weather_code），决定画哪一张图
     * @param dark   是否为深色主题，决定云朵/雨雪用哪套配色
     */
    static void paint(QPainter &p, const QRectF &box, int wmoCode, bool dark);

    /**
     * 清空栅格缓存。主题切换或 DPI 变化后调用可以让缓存重建；
     * 不调用也不会画错（缓存键里带主题与 DPI），只是会多留一些旧图。
     */
    static void clearCache();

    /**
     * 天气代码 → 资源里那张 SVG 的文件名（不含目录与扩展名）。
     * @param wmoCode WMO 天气代码；表里没有的代码统一落到「雨」
     * @return 12 个固定 slug 之一，不会返回空串
     */
    static QString slugForCode(int wmoCode);
};
