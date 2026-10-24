// -----------------------------------------------------------------------------
// MediaVolume —— 按播放器调音量的 Core Audio 封装（头文件）。
// 职责：绕开 GSMTC（它根本不提供音量接口），改用 WASAPI 会话管理按 appId 找到
//       目标播放器的音频会话，再用 ISimpleAudioVolume 单独改它的音量。
// 约定：所有函数都可能在「没会话/没设备/COM 初始化失败」时安全失败，
//       失败用 -1（读）/ false（写）表达，不抛异常、不弹窗。
// 坑：  appId 既可能是 exe 名（cloudmusic.exe）也可能是 AUMID（Company.AppName_hash），
//       匹配逻辑全在 .cpp 的 matches() 里，调用方不要自己做字符串裁剪。
// -----------------------------------------------------------------------------
#pragma once

#include <QString>

/**
 * 调整「某个播放器」的音量（不是系统总音量）。
 *
 * 原理：走 Windows Core Audio（WASAPI）的会话管理，把当前所有音频会话按
 * 进程可执行文件名对上我们要控制的播放器，然后用 ISimpleAudioVolume 单独调它的音量。
 * 系统媒体会话（GSMTC）本身不提供音量接口，所以只能这么绕。
 *
 * 全部接口在拿不到会话时都会安全失败：volumeFor 返回 -1，setVolume 返回 false。
 *
 * 这些函数都是同步调用（内部要跟 audiosrv 做一次 RPC 往返，通常几毫秒到几十毫秒），
 * 只适合在 UI 事件里按需调用；不要放进音频回调或每帧刷新的热路径。
 *
 * 注意粒度：操作对象是「命中 appId 的第一个音频会话」，而不是播放器进程的总音量。
 * 同一播放器为多条音频流各建会话时（例如预览窗 + 主输出），只会改到其中一条。
 */
namespace MediaVolume
{

/**
 * 播放器的当前音量（0-100）；-1 表示没找到它的音频会话
 *
 * @param appId 播放器标识，可传 exe 文件名（cloudmusic.exe，大小写与扩展名不限）
 *              或 AUMID（NetEase.CloudMusic_1.0.0.0_x64__xxx）。空串必定返回 -1。
 * @return 0-100 的取整音量；找不到匹配会话、没有默认输出设备、COM 不可用时返回 -1。
 *         调用方应把 -1 当作「未知/不支持」，据此退化到 systemVolume()。
 */
int volumeFor(const QString &appId);

/**
 * 设置播放器音量（0-100），成功返回 true
 *
 * @param appId  同 volumeFor() 的标识约定。
 * @param volume 目标音量，超出 0-100 会被静默夹紧（不报错）。
 * @return 找到会话且 SetMasterVolume 成功才为 true；会话不存在、播放器未启动音频流、
 *         COM 初始化失败都返回 false（此时调用方通常退化到 setSystemVolume）。
 *         注意返回值只代表「写进去了」，不代表播放器真的按这个值发声。
 */
bool setVolume(const QString &appId, int volume);

/**
 * 系统主音量（0-100），找不到设备时返回 -1
 *
 * 取的是默认渲染设备（eRender + eConsole）的主音量标量，与具体播放器无关。
 * @return 0-100；没有默认输出设备时为 -1。
 */
int systemVolume();

/**
 * 设置系统主音量（0-100）。
 *
 * @param volume 目标音量，超出 0-100 会被静默夹紧。
 * @return 成功写入默认渲染设备时为 true；无设备或 COM 不可用为 false。
 */
bool setSystemVolume(int volume);

} // namespace MediaVolume
