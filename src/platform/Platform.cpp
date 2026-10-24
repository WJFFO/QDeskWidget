// -----------------------------------------------------------------------------
// Platform —— 窗口外观的 Win32/DWM 实现（Platform.h 的实现文件）。
// 职责：亚克力 accent、DWM 背板/圆角/深色模式、窗口 z 序，全部在此落地。
// 约定：非 Windows 平台每个函数都保留一份 #else 降级返回，头文件声明不区分平台。
// 坑1：SetWindowCompositionAttribute 是未公开导出，只能 GetProcAddress 动态取，没有导入库。
// 坑2：DWM 属性常量一律用 kDwm* 私有名（SDK 已有 DWMSBT_* / DWMWCP_* 同名枚举，重定义会 C2872）。
// 坑3：applyMaterial 每次切换都必须先把背板复位成 NONE 再施加目标材质，否则会残留上次状态。
// -----------------------------------------------------------------------------
#include "platform/Platform.h"

#include <QOperatingSystemVersion>
#include <QPainterPath>
#include <QWidget>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  include <winternl.h>
#endif

namespace
{
// 内部链接：本 TU 里的枚举、常量与辅助函数都不进入导出符号表，
// 因此可以放心用「看起来是全局名」的短名字（AccentState、matches 等）而不怕与其他 TU 撞车。
#ifdef Q_OS_WIN

// ---- SetWindowCompositionAttribute（亚克力） ----
// 该 API 未公开，下列枚举/结构体均照抄社区逆向结论（Win10 1803 起稳定），
// 不要按「看起来更合理」的方式重排字段，ABI 必须逐字节对上。

/**
 * accent 状态机取值；界面只用到 DISABLED 与 ACRYLICBLURBEHIND 两个。
 *
 * 数字是 SetWindowCompositionAttribute 自己的协议编号，会原样塞进 AccentPolicy
 * 交给系统，因此不能为了「整洁」重排或跳号，否则系统按旧编号解读会得到别的效果。
 */
enum AccentState {
    ACCENT_DISABLED = 0,                  // 关闭：不叠加任何合成效果，窗口外观完全由自绘内容决定
    ACCENT_ENABLE_GRADIENT = 1,           // 纯色渐变填充：不透明，会把下层窗口完全挡住
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2, // 半透明渐变：能透出下层，但不做模糊（Win10 早期观感）
    ACCENT_ENABLE_BLURBEHIND = 3,         // 旧版高斯模糊：Win10 1803 前的实现，性能差且易与 DWM 抢合成
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,  // 亚克力（模糊 + 噪点 + 染色），本项目 Material::Acrylic 用的就是它
    ACCENT_ENABLE_HOSTBACKDROP = 5        // 宿主背板：交给 XAML 宿主决定，普通 Win32 窗口用不到
};

/** SetWindowCompositionAttribute 的负载结构，gradientColor 为 0xAABBGGRR */
struct AccentPolicy
{
    int accentState;    // AccentState 取值；决定系统怎么合成这块窗口
    int accentFlags;    // 行为开关位（如是否绘制边框），本项目恒为 0，保持系统默认
    DWORD gradientColor; // 染色，0xAABBGGRR；仅当 accentState != ACCENT_DISABLED 时被系统读取
    int animationId;    // 过渡动画编号，0 = 不播动画（切材质时不做淡入，避免残影）
};

/** WCA_ACCENT_POLICY = 19：该属性号是未公开 API 的既有协议值，写死不可换 */
enum WindowCompositionAttrib { WCA_ACCENT_POLICY = 19 };

/** SetWindowCompositionAttribute 的通用入参：属性号 + 负载指针 + 负载字节数 */
struct WindowCompositionAttribData
{
    DWORD attrib;  // 属性号，本文件只用 WCA_ACCENT_POLICY(19)
    PVOID data;    // 指向具体负载（这里是 AccentPolicy），调用期间必须保持有效
    SIZE_T size;   // 负载字节数；系统按它决定拷贝多少，填错会读到栈上脏数据
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI *)(HWND, WindowCompositionAttribData *);

/**
 * 惰性解析并缓存 user32!SetWindowCompositionAttribute。
 *
 * 该符号不在 user32.lib 导入表里，只能运行时 GetProcAddress；结果缓存在函数内 static，
 * 由 C++11 魔法静态保证只解析一次且线程安全。
 * @return 解析成功返回函数指针；拿到 user32 或符号失败返回 nullptr（调用方须判空后跳过，
 *         因为 Win7 等旧系统上这个出口根本不存在）。
 */
SetWindowCompositionAttributeFn compositionFn()
{
    static SetWindowCompositionAttributeFn fn = []() -> SetWindowCompositionAttributeFn
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32)
            return nullptr;
        return reinterpret_cast<SetWindowCompositionAttributeFn>(
            GetProcAddress(user32, "SetWindowCompositionAttribute"));
    }();
    return fn;
}

// ---- DWM 属性 ----
// 注意：这里的常量一律加 kDwm 前缀。新版 Windows SDK 的 dwmapi.h 已经把
// DWMWCP_* / DWMSBT_* 定义成全局枚举成员，若在此重复定义成同名常量，
// MSVC 会报 C2872「ambiguous」（MinGW 的旧头文件里没有，才不会暴露）。
// 所以这里不复用 SDK 名字，只用等价的本地常量。

/** DWMWA_USE_IMMERSIVE_DARK_MODE：BOOL，控制标题栏/系统边框走深色
 *  20 是正式版编号（更早的预览版曾用 19）；老系统上该属性不存在，写入只会静默失败。 */
constexpr DWORD kDwmAttrUseImmersiveDarkMode = 20;
/** DWMWA_BORDER_COLOR：COLORREF，窗口外圈 1px 边框色；仅 22000+ 有效 */
constexpr DWORD kDwmAttrBorderColor = 34;
/** DWMWA_CAPTION_COLOR：COLORREF，标题栏底色；仅 22000+ 有效 */
constexpr DWORD kDwmAttrCaptionColor = 35;
/** DWMWA_TEXT_COLOR：COLORREF，标题栏文字色；仅 22000+ 有效 */
constexpr DWORD kDwmAttrTextColor = 36;
/** DWMWA_WINDOW_CORNER_PREFERENCE：int，取 kDwmwcp* */
constexpr DWORD kDwmAttrWindowCornerPreference = 33;
/** DWMWA_SYSTEMBACKDROP_TYPE：int，取 kDwmsbt*；仅 build 22621+ 有意义 */
constexpr DWORD kDwmAttrSystemBackdropType = 38;

constexpr int kDwmwcpDoNotRound = 1; // 直角；0(DEFAULT) 会让系统自行决定，不能拿来表达「关」
constexpr int kDwmwcpRound = 2;      // 系统圆角（半径由系统主题决定，不可自定义）

constexpr int kDwmsbtNone = 1;         // 无背板：切材质的必经「归一化」目标
constexpr int kDwmsbtMainWindow = 2;   // Mica（云母），要求非分层窗口
constexpr int kDwmsbtTransientWindow = 3; // Acrylic（系统级亚克力背板），本项目未使用
constexpr int kDwmsbtTabbedWindow = 4;    // 标签页式云母，本项目未使用

/** 系统版本（主版本号 + build 号）。
 *  只关心 build：22000 = Win11 21H2（圆角），22621 = Win11 22H2（云母背板）。 */
struct WinBuild
{
    DWORD major = 0; // 主版本号，Win10/11 均为 10
    DWORD build = 0; // build 号，判断能力的唯一依据；解析失败时为 0（等价于「什么都不支持」）
};

/**
 * 取系统真实 build 号（进程内缓存一次）。
 *
 * 必须走 ntdll!RtlGetVersion 而不是 GetVersionEx：后者在无兼容性清单时会谎报成
 * Win8.1（6.2），导致 Win11 上云母/圆角全部误判为不支持。
 * @return 解析失败（拿不到 ntdll 或符号）时返回 {0,0}，调用方的 build 比较自然全部不成立。
 */
WinBuild winBuild()
{
    static const WinBuild b = []
    {
        WinBuild r;
        using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
            return r;
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!fn)
            return r;
        RTL_OSVERSIONINFOW vi = {}; // 必须清零：RtlGetVersion 只填它认识的字段，其余依赖结构体初值
        // dwOSVersionInfoSize 是入参而非出参，不填或填错时 RtlGetVersion 直接返回错误状态，
        // 于是 build 恒为 0、所有能力判断都不成立（表现为 Win11 上也没有云母和圆角）
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (fn(&vi) == 0) {
            r.major = vi.dwMajorVersion;
            r.build = vi.dwBuildNumber;
        }
        return r;
    }();
    return b;
}

/**
 * 设置窗口的 accent（亚克力/渐变/关闭）。
 *
 * @param hwnd  目标窗口句柄，需为顶层窗口。
 * @param state AccentState 之一；传 ACCENT_DISABLED 时不会写 gradientColor。
 * @param tint  染色，仅在非 DISABLED 时生效。
 *
 * 注意 color 的打包顺序是 0xAABBGGRR（ABGR），与 Qt 的 ARGB 相反，
 * 少写一个分量就会得到完全不同的颜色，别照抄 QColor::rgb()。
 * 失败静默：系统没有该导出（Win7）时函数直接返回，窗口保持原样。
 */
void setAccent(HWND hwnd, int state, const QColor &tint)
{
    auto fn = compositionFn();
    if (!fn)
        return;

    AccentPolicy policy = {}; // 全零起步：accentFlags/animationId 依赖默认 0，手填容易漏字段
    policy.accentState = state;
    policy.accentFlags = 0;
    if (state != ACCENT_DISABLED) {
        // GradientColor 为 0xAABBGGRR
        policy.gradientColor = (DWORD(tint.alpha()) << 24)
                               | (DWORD(tint.blue()) << 16)
                               | (DWORD(tint.green()) << 8)
                               | DWORD(tint.red());
    }
    // policy 在栈上，直接把地址交给系统；fn 是同步调用，返回后即可随作用域销毁
    WindowCompositionAttribData data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
    fn(hwnd, &data);
}

/**
 * 设置 DWM 系统背板类型。
 *
 * @param hwnd 目标窗口句柄。
 * @param type kDwmsbt* 之一（1=NONE 2=MAINWINDOW/Mica 3=TRANSIENTWINDOW 4=TABBEDWINDOW）。
 * @return build < 22621 或 DWM 拒绝时返回 false。
 *
 * 低于 22621 必须先返回 false 而不是硬调：老系统上这个属性号不存在，
 * DwmSetWindowAttribute 会返回 E_INVALIDARG，某些版本还会顺带把窗口边框画乱。
 */
bool setBackdropType(HWND hwnd, int type)
{
    if (winBuild().build < 22621)
        return false;
    const HRESULT hr = DwmSetWindowAttribute(hwnd, kDwmAttrSystemBackdropType,
                                             &type, sizeof(type));
    return SUCCEEDED(hr);
}

/**
 * 扩展/收回 DWM 客户区边框。
 *
 * Mica 要求窗口「没有不透明系统边框」，所以背板生效时必须把整圈 MARGINS 设为 -1
 * （-1 表示整块客户区都算扩展帧）；关背板时要收回 0，否则会留下 1px 的系统描边，
 * 和自绘圆角对不上。
 * @param hwnd   目标窗口句柄。
 * @param extend true = 扩展到整窗（-1），false = 收回（0）。
 */
void extendFrame(HWND hwnd, bool extend)
{
    MARGINS m = { extend ? -1 : 0, extend ? -1 : 0, extend ? -1 : 0, extend ? -1 : 0 };
    DwmExtendFrameIntoClientArea(hwnd, &m);
}

/**
 * QColor → Win32 COLORREF。
 * @param c 源颜色；alpha 被忽略（DWM 的标题栏/边框属性都不吃 alpha）
 * @return 0x00BBGGRR 形式的 COLORREF —— 字节序与 RGB 相反，不能用 qRgb() 直接喂
 */
COLORREF toColorRef(const QColor &c)
{
    return COLORREF((c.red() & 0xFF) | ((c.green() & 0xFF) << 8) | ((c.blue() & 0xFF) << 16));
}

/** Qt 的 WA_TranslucentBackground 会把窗口变成 WS_EX_LAYERED */
bool isLayered(HWND hwnd)
{    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
}

#endif
} // namespace

namespace Platform
{

// 施加材质：核心是「先归一化、再施加」，保证任意材质间来回切换都不残留上次的 DWM 状态。
// 唯一的分叉点是「能不能吃原生 Mica」——分层窗口永远吃不到，只能退化成 accent 模糊。
void applyMaterial(QWidget *w, Material material, const QColor &tint)
{
    if (!w)
        return;
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(w->winId());

    // 关键：每次切换材质都要先把窗口恢复成「无 DWM 背板」的基线，再施加目标材质。
    // 否则会残留上一次的背板状态 —— 典型症状就是从「纯色」切到「云母」时，
    // DWM 的 Mica 背板压在分层窗口上，整块变成不透明；而从亚克力切过去时，
    // 因为强调色（accent）一直在生效，背板被盖住，看起来还是透明的。
    // 所以这里不再「只设不清」，而是统一按 清背板 → 设强调色 的顺序来。
    const bool layered = isLayered(hwnd); // 窗口是否 WS_EX_LAYERED；决定能不能吃到原生 Mica（分层窗口永远不行）

    if (material == Material::Mica && !layered && winBuild().build >= 22621) {
        // 只有非分层窗口才能真正吃到 DWM 原生云母背板。
        // 三步顺序不可交换：撤 accent 必须最先（残留的模糊层会顶在背板前面，肉眼看到的是模糊而非云母），
        // 扩帧必须早于设背板（DWM 只在扩展帧覆盖的区域内绘制背板，顺序反了就是「设了但看不见」）。
        setAccent(hwnd, ACCENT_DISABLED, tint);
        extendFrame(hwnd, true);
        setBackdropType(hwnd, kDwmsbtMainWindow);
    } else {
        // 分层窗口：DWM 背板不参与合成，且会和 SetWindowCompositionAttribute 打架，
        // 一律关掉背板，改用强调色实现模糊。云母则用更薄的染色近似壁纸取色。
        // 铁律：分层窗口（WA_TranslucentBackground = WS_EX_LAYERED）永远不打 Mica 背板标记——
        // 此时系统只认分层 alpha 作为合成依据，再打标只会让整块变成不透明。
        // 这行 setBackdropType(NONE) 同时承担「归一化」职责：先落回无背板基线，再决定下面施加什么。
        setBackdropType(hwnd, kDwmsbtNone);
        extendFrame(hwnd, false);
        if (material == Material::Solid)
            setAccent(hwnd, ACCENT_DISABLED, tint);
        else
            setAccent(hwnd, ACCENT_ENABLE_ACRYLICBLURBEHIND, tint);
    }

    // 深色模式按 tint 亮度推定（lightnessF < 0.5 视为深色）：决定系统标题栏/边框走深色还是浅色。
    // 这一步与材质分支无关，Solid 之后也要执行——只要走了 applyMaterial 就同步刷新一次。
    // 返回值故意不检查：老系统没有该属性号，失败也只影响标题栏配色，不该打断整条材质流程。
    const BOOL dark = tint.lightnessF() < 0.5 ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kDwmAttrUseImmersiveDarkMode, &dark, sizeof(dark));

    // 标题栏 / 外圈边框的显式配色（Win11 22000+）。
    // 只靠 USE_IMMERSIVE_DARK_MODE 在**分层窗口**上实测不可靠：设置面板的客户区已经是深色玻璃，
    // 标题栏却仍是浅灰，中间还夹一圈白边，割裂得很明显。这里把三处颜色直接钉死。
    // 无边框的整理盒没有标题栏，这几个属性对它是空操作，所以放在公共路径上无损。
    if (winBuild().build >= 22000) {
        const COLORREF cap = toColorRef(dark ? QColor(32, 34, 42) : QColor(247, 248, 251));
        const COLORREF txt = toColorRef(dark ? QColor(236, 239, 246) : QColor(28, 30, 36));
        const COLORREF brd = toColorRef(dark ? QColor(56, 59, 70) : QColor(221, 224, 230));
        DwmSetWindowAttribute(hwnd, kDwmAttrCaptionColor, &cap, sizeof(cap));
        DwmSetWindowAttribute(hwnd, kDwmAttrTextColor, &txt, sizeof(txt));
        DwmSetWindowAttribute(hwnd, kDwmAttrBorderColor, &brd, sizeof(brd));
    }
#else
    // 非 Windows 平台没有 accent / 背板概念，参数只为保持签名一致，这里显式消掉未使用告警。
    Q_UNUSED(material)
    Q_UNUSED(tint)
#endif
}

// 诊断用：把 DWM 里真实生效的背板类型读回来，用于确认 applyMaterial 是否被系统接受。
// 背板类型没有 getter 接口，只能同一个属性号走 DwmGetWindowAttribute；旧系统上属性号不存在时
// 该调用返回 E_INVALIDARG，此时 value 保持预置的 -1，正好对应「读不到」而不是「无背板」。
int backdropType(QWidget *w)
{
    if (!w)
        return -1;
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    int value = -1; // 预置 -1：系统不支持该属性时会原样返回，调用方据此区分「NONE」和「读不到」
    const HRESULT hr = DwmGetWindowAttribute(hwnd, kDwmAttrSystemBackdropType,
                                             &value, sizeof(value));
    return SUCCEEDED(hr) ? value : -1;
#else
    return -1;
#endif
}

// 系统圆角。22000 以下没有这个属性，直接判失败让调用方走 mask 兜底。
bool setRoundedCorners(QWidget *w, bool enable)
{
    if (!w)
        return false;
#ifdef Q_OS_WIN
    if (winBuild().build < 22000)
        return false;
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    const int pref = enable ? kDwmwcpRound : kDwmwcpDoNotRound;
    // 只传 int 本身（sizeof(int)）：WINDOW_CORNER_PREFERENCE 在 ABI 上就是 4 字节整数，
    // 传成枚举类型或结构体会被 DWM 判成 E_INVALIDARG，圆角静默不生效。
    const HRESULT hr = DwmSetWindowAttribute(hwnd, kDwmAttrWindowCornerPreference,
                                             &pref, sizeof(pref));
    return SUCCEEDED(hr);
#else
    Q_UNUSED(enable)
    return false;
#endif
}

// 兜底圆角：把 QPainterPath 转成 QRegion 掩码。掩码只有全有/全无两种像素，
// 所以边缘一定有锯齿；只在 DWM 圆角不可用时用，不要拿它替代系统圆角。
// radius 单位是逻辑像素（与 QWidget::rect() 同一坐标系），Qt 内部会按 devicePixelRatio
// 换算成物理像素再裁，所以高 DPI 缩放下不需要调用方自己乘缩放系数。
void applyRoundedMask(QWidget *w, int radius)
{
    if (!w || radius <= 0)
        return;
    QPainterPath path; // QRegion 没有圆角构造，只能先走绘制路径再离散成区域
    path.addRoundedRect(QRectF(w->rect()), radius, radius);
    // toFillPolygon() 把曲线打成多边形，再 toPolygon() 取整——精度损失就体现在这里的锯齿上
    w->setMask(QRegion(path.toFillPolygon().toPolygon()));
}

// 清空掩码：Qt 内部会把空区域还原成整个窗口矩形，无需自己算 rect。
void clearMask(QWidget *w)
{
    if (!w)
        return;
    w->clearMask();
}

// 22000 = Win11 21H2：圆角属性从这里开始存在（更早的系统即使调了也只会失败）。
bool isWindows11()
{
#ifdef Q_OS_WIN
    return winBuild().build >= 22000;
#else
    return false;
#endif
}

// 22621 = Win11 22H2：DWMWA_SYSTEMBACKDROP_TYPE 从这一版才可用，比圆角（22000）晚一个版本。
bool supportsMica()
{
#ifdef Q_OS_WIN
    return winBuild().build >= 22621;
#else
    return false;
#endif
}

// 压到 z 序最底。SWP_NOACTIVATE 保证不抢焦点；窗口若带 always-on-top 属性则不受影响
// （那是独立样式位，SetWindowPos 的 hWndInsertAfter 只在同层级内调整）。
void sendToDesktopLayer(QWidget *w)
{
    if (!w)
        return;
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    Q_UNUSED(w)
#endif
}

// 提到同级最前，同样不激活：桌面整理盒点一下才该获得焦点，程序性前置不该打断用户输入。
void bringToFront(QWidget *w)
{
    if (!w)
        return;
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    Q_UNUSED(w)
#endif
}

} // namespace Platform
