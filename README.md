# QDeskWidget

![QDeskWidget](docs/images/hero.png)

QDeskWidget 是一款基于 **Qt 6 / C++** 的 Windows 桌面整理工具，把桌面图标收进一个个真实的文件夹盒子。

- 亚克力 / 云母 / 纯色三种材质，Win11 走 DWM 系统圆角，无边框，不抢任务栏
- 每个整理盒背后是**一个真实文件夹**（可托管新建，也可关联已有文件夹），桌面文件不被搬走
- 从资源管理器 / 微信 / 浏览器拖进来，拖出去，盒内拖动排序，拖到文件夹图标上即移入
- 图标网格 / 列表 / **堆叠**三种视图，手动 / 名称 / 时间 / 类型 / 大小五种排序
- **天气 / 音乐** 两个内置小组件（音乐支持封面/歌词/进度/循环/音量），**任何盒子都能组合到一个窗口**（`Ctrl+Tab` 切成员）
- 组件**版式随尺寸分档**：窗口大小的改变会切换到不同版式（而非把控件硬缩小），文字与图标不会被裁切或遮挡
- 组件可**隐藏标题栏**，让内容铺满整窗（隐藏后在空白处直接拖动移动、右键出菜单）
- 所有盒子 / 组件都能**锁定大小**，锁定后拖边框不再改变宽高
- **手动堆叠**：选几个文件右键「叠成一堆」，盒子里只显示一张叠卡 + 数量角标，双击展开
- **插件机制**：自己写一个 DLL 就能加组件（附可直接编译的示例）
- 标题栏拖动移动、双击改名、一键折叠成标题条
- 托盘常驻、单实例、布局与状态全部本地持久化

---

## 构建与运行

依赖：**Qt 6.5+**（Core / Gui / Widgets / Network）、CMake 3.19+、**C++20** 编译器
（C++20 是 C++/WinRT 协程的要求，音乐组件读取系统媒体会话需要它）。

```powershell
# MinGW 示例（本机验证通过的组合：Qt 6.11.0 mingw_64 + MinGW 13.1）
cmake -S . -B build/mingw -G Ninja `
      -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.11.0/mingw_64 `
      -DCMAKE_CXX_COMPILER=D:/Qt/Tools/mingw1310_64/bin/g++.exe
cmake --build build/mingw
./build/mingw/QDeskWidget.exe
```

也可以直接用 Qt Creator 打开 `CMakeLists.txt`，选择对应 Kit 构建运行。

- **MSVC 说明**：`CMakeLists.txt` 已为 MSVC 自动加 `/utf-8 /Zc:__cplusplus /permissive-`。
  第一条必须有 —— 源码是 UTF-8，不加会被按 GBK 读，界面中文变乱码并刷 C4819。
  MinGW 与 MSVC（MSVC 19.51 + Qt 6.11.0 msvc2022_64）两套均已验证零错误零警告。
- 音乐组件只走系统媒体会话（GSMTC），**不再内置本地播放器**，因此不依赖 Qt Multimedia。
- **Windows 发布打包**：运行 `package-msvc.bat`（加 `clean` 全量重编），产出
  `dist\QDeskWidget\` 免安装自包含包 —— Release exe、Qt 运行库与 VC/UCRT 运行库
  （用户无需装 vc_redist）全部平铺在包根目录，`qt.conf` 指向同级的 `plugins\`，
  整个文件夹拷到别的机器即可运行。数据目录 `<exe 同目录>\QDeskWidget\` 随包走，
  便携形态。


## 插件：自己写组件

插件就是一个 DLL，实现 `src/widgets/IWidgetPlugin.h` 里的 `IWidgetPlugin`：

```cpp
class MyPlugin : public QObject, public IWidgetPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QDeskWidgetPlugin_iid FILE "metadata.json")
    Q_INTERFACES(IWidgetPlugin)
public:
    QString pluginId() const override { return "my-widget"; }
    QString displayName() const override { return "我的组件"; }
    QWidget *createWidget(QWidget *parent) override { return new MyWidget(parent); }
    QString widgetTitle() const override { return "我的"; }
};
```

- 接口还提供 `description()`、`badgeCount()`（标题栏计数）、`refreshWidget()`、
  `saveWidgetState()` / `restoreWidgetState()`（状态会存进 `boxes.json` 的 `widgetState`）。
- 宿主会把 `createWidget()` 返回的控件铺满内容区，组件自己管刷新定时器和内部布局。
- 编译产物放进 **`<exe 同目录>\QDeskWidget\plugins\`**（打包形态下整个 `QDeskWidget\`
  数据目录随 exe 走），然后 设置 → 组件 →「重新扫描插件」，再在下拉框里选中它、点「添加」。
- `plugins/hello-clock/` 是完整可编译的示例（一个走秒的时钟），CMake 里默认一起构建并自动拷到上述目录，
  复制整个目录改个 id 就能当自己的组件起点。

📖 **完整的插件编写文档见 [docs/PLUGIN_GUIDE.md](docs/PLUGIN_GUIDE.md)**：接口逐个说明、
控件编写规则、状态持久化、CMake 模板、安装加载与排查清单，直接照着写即可。

---

## 交互一览

| 操作 | 效果 |
| --- | --- |
| 拖动标题栏 | 移动盒子（拖动过程 1:1 跟手，松手后对齐 16px 网格） |
| 双击标题 | 就地改名 |
| 折叠状态下点标题栏 | 展开 |
| 标题栏 `▾` | 折叠成标题条（180ms 卷帘动画，展开同理） |
| 标题栏 `⋯`、标题栏右键 | 盒子菜单（改名、查看方式、排序、图标大小、折叠样式、组合、置顶、删除…） |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | 组合内切换成员 |
| 音乐组件「音量」按钮 | 弹出音量滑条（滚轮 ±5%） |
| 音乐组件「循环」按钮 | 顺序 / 列表 / 单曲 循环 |
| 隐藏标题栏后左键拖动 | 移动窗口（按钮/滑条上除外；`Alt` + 拖动可在任意位置移动，右键出菜单） |
| 标题栏滚轮 | 组合内切换成员 |
| 堆叠视图点击卡片 | 弹出 3×3 / 5×5 网格窗口（`Esc` 或点窗口外关闭） |
| 双击 `.pile` 堆 | 用同一个弹出窗口展开这一堆 |
| 选中后右键「叠成一堆…」 | 把选中的项目打成一个堆 |
| 双击文件 / 文件夹 | 用默认程序打开 / 打开文件夹 |
| 从资源管理器拖入 | 默认**复制**进盒子；按住 `Shift` 改为移动 |
| 选中后拖出 | 拖到资源管理器（走 Windows 默认移动/复制判定） |
| 盒内拖动 | 排序，自动切换为「手动」排序 |
| 拖到文件夹图标上 | 移入该文件夹（按住 `Ctrl` 为复制） |
| `F2` / `Delete` / `Enter` | 重命名 / 移到回收站 / 打开 |
| `Ctrl` + 滚轮 | 调整图标大小（32px ~ 160px） |
| 单击托盘图标 / 双击托盘图标 | 显示隐藏全部盒子 / 打开设置 |
| 再次启动程序 | 唤起已有实例并显示全部盒子，本进程退出 |

---

## 数据存放

```
<exe 同目录>\QDeskWidget\        数据随 exe 走（便携形态；旧版 %APPDATA% 数据首次启动自动迁移）
├── settings.json          全局外观 / 行为设置（含天气城市、优先播放器）
├── boxes.json             盒子布局、排序、组件状态、组合成员
├── weather_cache.json     天气组件缓存
├── plugins\               组件插件 DLL
└── storage\<boxId>\       托管盒子的真实文件夹（.pile 堆也在这里面）
```

- 托管文件夹里的文件就是普通文件，资源管理器一样能打开、改名、删除。
- 删除盒子**不会**删除托管文件夹里的文件。
- 组件的私有状态（天气城市、音乐音量、插件数据）存在各自 `BoxConfig.widgetState` 里。

---

## 目录结构

```
main.cpp                     入口：字体、数据目录、单实例、开发自检参数
src/App.*                    应用控制器：托盘、窗口生命周期、组合重建、持久化
src/core/
  Paths.*                    数据目录常量
  AppSettings.*              全局设置 + 主题色令牌
  BoxConfig.*                盒子配置（kind / 组件状态 / 组合成员）与 JSON 序列化
  BoxManager.*               盒子集合的增删存取（文件盒 / 组件）
  FileItemModel.*            文件夹内容模型（图标缓存、排序、目录监听、堆识别）
  ShellActions.*             打开 / 定位 / 回收站 / 复制 / 移动 / 重命名
  lyricfetcher.*             网易云 LRC 歌词抓取（带缓存）
  globalmediasessionlistener.*  系统全局媒体会话（C++/WinRT，仅 MSVC）
src/platform/Platform.*      Win32 材质（亚克力 / 云母 / 纯色）、DWM 圆角、背板诊断
src/platform/MediaVolume.*   按播放器调音量（Core Audio 会话音量）
src/ui/
  MarqueeLabel.*             单行文字：放不下时横向滚动
  CoverView.*                圆形封面：播放时旋转
  mrowlayout.*               横向布局容器（用户提供）
  mcolumnlayout.*            纵向布局容器（用户提供）
  UiStyle.h                  对话框 / 面板统一样式 + 表单行助手
  FileBoxWindow.*            盒子窗口 + 标题栏 + 折叠 + 组合切换 + 堆叠菜单
  FileListView.*             内容视图：拖放、内联改名、快捷键
  FileItemDelegate.*         图标网格 / 列表绘制（含堆卡与角标）
  StackView.*                堆叠卡片 + 3×3 / 5×5 弹出展开窗口
  NewBoxDialog.*             新建整理盒
  SettingsDialog.*           设置窗口
src/widgets/
  WidgetPanel.h/.cpp         组件面板基类
  IWidgetPlugin.h            插件接口（第三方 DLL 只需 include 这个头）
  WidgetRegistry.*           内置组件注册 + 插件扫描 / 创建 / 适配
  WeatherWidget.*            天气组件
  WeatherIcons.*             天气图标：SVG 资源 → 按主题染色 → 栅格化缓存 → 贴图
  MusicWidget.*              音乐组件（GSMTC 控制器 + 循环 + 音量 + 分档版式）
  LyricsView.*               歌词视图（自动折行、上下文多行、平滑滚动、矮版式下只显示当前句）
plugins/hello-clock/         示例插件（可直接复制改成自己的组件）
resources/weather/           天气图标 SVG（Meteocons line 变体，MIT，见同目录 LICENSE.txt）
```

---

## 许可

本项目以 **MIT License** 发布，全文见仓库根目录的 [`LICENSE`](LICENSE)（Copyright © 2026 whnbd）。

**关于与 DeskBox 的关系**：本项目的定位是**独立实现**。它依照
[DeskBox](https://github.com/Tianyu199509/DeskBox)（C# / WinUI 3）所呈现的功能形态，
用 **Qt 6 / C++20** 从零实现，**未使用原项目的任何源代码** —— 两者技术栈、代码库与构建体系完全独立，
不存在代码层面的衍生关系。DeskBox 仅作为功能与体验的参照对象；本项目与原作者无隶属关系，亦未获其背书。

天气图标来自 [Meteocons](https://github.com/basmilius/weather-icons)（Copyright © 2020-present Bas Milius，MIT），
许可证原文见 `resources/weather/LICENSE.txt`，随程序一起分发即可。
