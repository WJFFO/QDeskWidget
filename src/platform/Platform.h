// -----------------------------------------------------------------------------
// Platform —— 窗口外观的 Win32/DWM 薄封装层（头文件）。
// 职责：把「材质（亚克力/云母/纯色）、DWM 圆角、窗口层级」这些只有 Windows 才有的
//       能力收敛成一组无状态函数，调用方不必包含 windows.h。
// 约定：入参 QWidget* 为 nullptr 时一律静默返回；非 Windows 平台走降级分支
//       （bool 返回 false、int 返回 -1），调用方无需自己做平台判断。
// 坑：  DWM 常量禁止使用 SDK 的 DWMSBT_* / DWMWCP_* 名字（MSVC 会报 C2872
//       ambiguous），实现里统一用 kDwm* 私有常量；applyMaterial 每次都要先归一化背板。
// -----------------------------------------------------------------------------
#pragma once

#include <QColor> // 必须完整包含：applyMaterial 的默认实参要在此处就地构造 QColor 临时对象，前置声明不够

class QWidget; // 只用到指针，前置声明即可——平台层不该把 QWidget 的完整定义传染给每个包含者

/**
 * Windows 平台相关外观处理：亚克力 / 云母材质、窗口圆角、窗口层级。
 * 非 Windows 平台或旧系统上安全降级。
 *
 * 本命名空间内全部函数无内部状态、可重复调用、可任意顺序调用；
 * 但它们都会经 QWidget::winId() 触达原生句柄，故只允许在该 QWidget 所属的
 * GUI 线程调用（跨线程调 winId() 会踩 Qt 的线程断言，且 DWM 调用会串到错线程）。
 */
namespace Platform
{

/**
 * 窗口背板材质。
 * 注意：取值会以「整数下标」写进配置（AppSettings::material 直接存下拉框 currentIndex），
 * 因此枚举顺序即界面顺序，不要重排或插入新值，否则老配置会串味。
 */
enum class Material {
    Acrylic = 0, // 亚克力模糊（Win10 1803+ / Win11）：靠 SetWindowCompositionAttribute 的 accent 实现
    Mica = 1,    // 云母系统背板（Win11 22H2+，不可用时回退亚克力）：需要非分层窗口才生效
    Solid = 2    // 纯色半透明，不做模糊：只保留窗口自身 alpha，关闭所有 accent
};

/**
 * 应用材质。tint 只用于亚克力染色，建议低 alpha。
 *
 * 内部会先「清背板 → 设 accent」做一次归一化，所以可反复调用、可任意切换材质；
 * 副作用是同时改写 DWM 深色模式标记（按 tint 亮度决定），并可能改变扩展边框状态。
 *
 * @param w        目标顶层窗口；nullptr 时直接返回。传入前应已 show() 过，
 *                 否则 winId() 会强制创建原生窗口、提前固化窗口属性。
 * @param material 目标材质；非法值（越界强转）会走 else 分支按亚克力处理。
 * @param tint     染色，默认近透明白 (255,255,255,22)；alpha 越低越透，
 *                 其 lightnessF() 还用于判断是否开启 DWM 深色模式。
 *
 * 无返回值：系统不支持目标材质时静默降级（例如 Win10 上要 Mica 会退成亚克力），
 * 调用方无从得知实际生效的是哪种；需要确认只能用 backdropType() 事后读回。
 */
void applyMaterial(QWidget *w, Material material, const QColor &tint = QColor(255, 255, 255, 22));

/**
 * DWM 系统圆角（Win11 21H2+）。返回是否成功。
 *
 * @param w      目标窗口；nullptr 返回 false。
 * @param enable true = 圆角(DWMWCP_ROUND)，false = 直角(DWMWCP_DONOTROUND)。
 * @return 系统版本不足（build < 22000）或 DwmSetWindowAttribute 失败时为 false，
 *         此时调用方应改用 applyRoundedMask() 兜底。
 */
bool setRoundedCorners(QWidget *w, bool enable);

/**
 * 兜底圆角裁剪（旧系统 / 关闭系统圆角时使用）
 *
 * 用 QRegion 掩码硬裁，边缘无抗锯齿（会有锯齿），只作为 DWM 圆角不可用时的降级方案。
 * @param w      目标窗口；nullptr 或 radius <= 0 时直接返回（不做任何裁剪）。
 * @param radius 圆角半径（逻辑像素），<= 0 视为无效。
 */
void applyRoundedMask(QWidget *w, int radius);

/**
 * 清除 applyRoundedMask 设置的窗口掩码（恢复矩形窗口）。
 * @param w 目标窗口；nullptr 时直接返回。
 */
void clearMask(QWidget *w);

/**
 * 读取当前窗口的 DWM 背板类型（诊断用）：1=NONE 2=MAINWINDOW(Mica)，-1 表示取不到
 *
 * 仅用于打日志/诊断「材质看起来没生效」，不要拿它做业务分支判断。
 * @param w 目标窗口；nullptr 返回 -1。
 * @return 成功时返回 DWM 的 DWMSBT_* 整数值（1=NONE，2=MAINWINDOW/Mica，
 *         3=TRANSIENTWINDOW/Acrylic，4=TABBEDWINDOW）；系统 < 22621 或调用失败返回 -1。
 */
int backdropType(QWidget *w);

/**
 * 是否 Windows 11（build >= 22000）。非 Windows 恒为 false。
 * @return build >= 22000 为 true；非 Windows，或 RtlGetVersion 取不到版本（build 按 0 处理）为 false，
 *         即「不确定」也会被当成不支持，宁可少用新特性也不要画出半成品。
 */
bool isWindows11();

/**
 * 是否支持 DWM 云母背板（build >= 22621，即 Win11 22H2+）。非 Windows 恒为 false。
 * @return 只有 22621 及以上为 true；阈值比 isWindows11() 晚一个版本，别混用。
 */
bool supportsMica();

/**
 * 把窗口压到桌面层 / 提到最前
 *
 * 只改 z 序、不改窗口样式：不激活窗口（SWP_NOACTIVATE），也不改变 always-on-top 属性。
 * @param w 目标窗口；nullptr 时直接返回。
 */
void sendToDesktopLayer(QWidget *w);

/**
 * 把窗口提到同级 z 序最前（同样不激活窗口）。
 * @param w 目标窗口；nullptr 时直接返回。
 */
void bringToFront(QWidget *w);

} // namespace Platform
