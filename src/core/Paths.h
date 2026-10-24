#pragma once

#include <QString>

/**
 * @file Paths.h
 * 全部数据路径的唯一出口：目录布局集中在这里，其他文件不要自己拼 %APPDATA%。
 *
 * 关键约定：除 ensureDir() 外，其余函数只是字符串拼接，不保证目录 / 文件真的存在，
 * 也不会创建任何东西；调用方要用之前必须自己 ensureDir() 或确认文件可打开。
 * 返回值都统一用 '/' 作分隔符，可直接用于 QDir / QFile 与日志输出。
 */

/**
 * 应用级路径常量。
 * 数据目录：<exe 所在目录>/QDeskWidget   （便携形态，数据随 exe 走）
 *    - settings.json   全局外观 / 行为设置
 *    - boxes.json      整理盒布局与状态
 *    - storage/<id>/   托管文件存储根（每个整理盒一个真实文件夹）
 *    - plugins/        用户安装的组件插件
 *
 * 说明：路径由 QCoreApplication::applicationDirPath() 决定，即放 exe 的目录。
 * 首次运行时会把旧的 %APPDATA%/QDeskWidget 整体迁移过来（见 Paths.cpp）。
 */
namespace Paths
{
/// 数据根目录（不带结尾分隔符）；结果在首次调用时算好并缓存
QString dataDir();
/// 全局设置文件路径 settings.json（不保证已存在）
QString settingsFile();
/// 盒子布局文件路径 boxes.json（不保证已存在）
QString boxesFile();
/// 托管文件存储根目录 storage/（不保证已存在）
QString storageRoot();
/**
 * 某个盒子的托管目录 storage/<boxId>。
 * @param boxId  盒子 id（8 位十六进制）；直接拼进路径，不做合法性校验，调用方负责来源可信
 * @return         目录路径；只做拼接，不会创建
 */
QString boxStorageDir(const QString &boxId);

/**
 * 确保目录存在（递归创建，已存在也算成功）。
 * @param dir  待创建目录；空字符串直接判失败
 * @return     true = 目录可用；false = 入参为空或创建失败（无权限 / 磁盘错误等）
 */
bool ensureDir(const QString &dir);
} // namespace Paths
