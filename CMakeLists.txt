cmake_minimum_required(VERSION 3.19)
project(QDeskWidget VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Svg)

qt_standard_project_setup()

qt_add_executable(QDeskWidget
    WIN32 MACOSX_BUNDLE
    main.cpp
    resources/app_icon.rc            # exe 内嵌图标（资源管理器/快捷方式/任务栏）
    src/App.h                        src/App.cpp
    src/core/Paths.h                 src/core/Paths.cpp
    src/core/AppSettings.h           src/core/AppSettings.cpp
    src/core/BoxConfig.h             src/core/BoxConfig.cpp
    src/core/BoxManager.h            src/core/BoxManager.cpp
    src/core/FileItemModel.h         src/core/FileItemModel.cpp
    src/core/ShellActions.h          src/core/ShellActions.cpp
    src/platform/Platform.h          src/platform/Platform.cpp
    src/platform/MediaVolume.h       src/platform/MediaVolume.cpp
    src/ui/FileItemDelegate.h        src/ui/FileItemDelegate.cpp
    src/ui/UiStyle.h
    src/ui/MarqueeLabel.h            src/ui/MarqueeLabel.cpp
    src/ui/CoverView.h               src/ui/CoverView.cpp
    src/ui/mrowlayout.h              src/ui/mrowlayout.cpp
    src/ui/mcolumnlayout.h           src/ui/mcolumnlayout.cpp
    src/ui/FileListView.h            src/ui/FileListView.cpp
    src/ui/StackView.h               src/ui/StackView.cpp
    src/ui/FileBoxWindow.h           src/ui/FileBoxWindow.cpp
    src/ui/NewBoxDialog.h            src/ui/NewBoxDialog.cpp
    src/ui/SettingsDialog.h          src/ui/SettingsDialog.cpp
    src/widgets/WidgetPanel.h        src/widgets/WidgetPanel.cpp
    src/widgets/IWidgetPlugin.h
    src/widgets/WidgetRegistry.h     src/widgets/WidgetRegistry.cpp
    src/widgets/WeatherWidget.h      src/widgets/WeatherWidget.cpp
    src/widgets/WeatherIcons.h       src/widgets/WeatherIcons.cpp
    src/widgets/MusicWidget.h        src/widgets/MusicWidget.cpp
    src/widgets/LyricsView.h         src/widgets/LyricsView.cpp
    src/core/lyricfetcher.h          src/core/lyricfetcher.cpp
)

target_include_directories(QDeskWidget PRIVATE src)

# ------------------------------------------------------------ 天气图标资源
# 图标来自 Meteocons（MIT，见 resources/weather/LICENSE.txt），取的是 line 变体：
# 128×128 统一网格、纯 path，没有 <use>/<symbol>。
# 入库前做过三处加工（脚本一次性完成，不参与构建）：
#   1. 删掉 SMIL 动画（QtSvg 不播放）；
#   2. 删掉 <mask> 及其引用（QtSvg 不支持，带 mask 的那一组会被整组丢弃）；
#   3. viewBox 收到 "16 16 96 96"（所有墨迹都落在这个方框内，去掉四周空白）；
#   4. 可变颜色换成 @CLOUD@ / @CLOUD2@ / @CLOUD3@ / @RAIN@ 令牌，
#      运行时按主题替换（见 WeatherIcons.cpp 的 palette）。
qt_add_resources(QDeskWidget "qdw_weather_icons"
    PREFIX "/weather"
    BASE "resources/weather"
    FILES
        resources/weather/clear-day.svg
        resources/weather/mostly-clear-day.svg
        resources/weather/partly-cloudy-day.svg
        resources/weather/overcast.svg
        resources/weather/fog.svg
        resources/weather/drizzle.svg
        resources/weather/rain.svg
        resources/weather/snow.svg
        resources/weather/partly-cloudy-day-rain.svg
        resources/weather/partly-cloudy-day-snow.svg
        resources/weather/thunderstorms.svg
        resources/weather/thunderstorms-hail.svg
)

qt_add_resources(QDeskWidget "qdw_app_icons"
    PREFIX "/app"
    BASE "resources"
    FILES resources/logo.png
)

# ---------------------------------------------------------------- 头文件依赖
# 中文版 MSVC 的 /showIncludes 输出是「注意: 包含文件:」，而 CMake 只认
# 「Note: including file:」。结果是 NMake 工程里**头文件依赖全部丢失**
# （compiler_depend.internal 里每个 .obj 只登记了 .cpp 自己），改头文件不会
# 触发重编，新旧代码混在一个二进制里。
#
# 这在「基类新增虚函数」时是致命的：派生类的虚表由旧头文件生成，比调用方
# 预期的少一项，运行期 call [vtable+0x1A8] 取到 0，直接跳到空地址
# （c0000005，栈顶 0x0）。2026-10-24 就是这么崩的：WidgetPanel.h 加了
# maximumSizeHint()，FileBoxWindow.cpp 重编了而 WeatherWidget.cpp 没有。
#
# 这里不依赖编译器输出：所有源文件都挂一个「头文件戳」，任意头文件变动
# 就整体重编一次。宁可慢一点，也不能让二进制不自洽。
if (MSVC)
    file(GLOB_RECURSE QDW_ALL_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*.h
    )
    set(QDW_HEADER_STAMP ${CMAKE_BINARY_DIR}/qdw_headers.stamp)
    add_custom_command(
        OUTPUT  ${QDW_HEADER_STAMP}
        COMMAND ${CMAKE_COMMAND} -E touch ${QDW_HEADER_STAMP}
        DEPENDS ${QDW_ALL_HEADERS}
        COMMENT "头文件有变动，整体重编（保证虚表布局一致）"
        VERBATIM
    )
    add_custom_target(qdw_headers ALL DEPENDS ${QDW_HEADER_STAMP})
    get_target_property(QDW_OWN_SOURCES QDeskWidget SOURCES)
    set_source_files_properties(${QDW_OWN_SOURCES}
        PROPERTIES OBJECT_DEPENDS ${QDW_HEADER_STAMP})
endif()

# MSVC 专项：
#  /utf-8        源码是 UTF-8（含中文界面字符串），不加会被按 GBK 读成乱码
#  /Zc:__cplusplus + /permissive-   Qt 6 头文件硬性要求
#  /utf-8 同时消掉 C4819 警告
if (MSVC)
    target_compile_options(QDeskWidget PRIVATE
        /utf-8
        /Zc:__cplusplus
        /permissive-
    )
endif()

target_link_libraries(QDeskWidget
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Svg
)

# 系统全局媒体会话（GSMTC）：C++/WinRT 只有在 MSVC 下可用
if (MSVC)
    target_sources(QDeskWidget PRIVATE
        src/core/globalmediasessionlistener.h
        src/core/globalmediasessionlistener.cpp
    )
    target_compile_definitions(QDeskWidget PRIVATE QDW_HAS_GSMTC)
    target_compile_options(QDeskWidget PRIVATE /bigobj)
    target_link_libraries(QDeskWidget PRIVATE windowsapp)
    message(STATUS "启用系统媒体会话（GSMTC）：音乐组件可读取/控制其它播放器")
else()
    message(STATUS "非 MSVC：不编译 GSMTC，音乐组件会提示「系统媒体会话不可用」")
endif()

# 示例插件（演示用户自写组件）
option(QDW_BUILD_EXAMPLE_PLUGIN "构建 plugins/hello-clock 示例插件" ON)
if (QDW_BUILD_EXAMPLE_PLUGIN)
    add_subdirectory(plugins/hello-clock)
endif()

if (WIN32)
    target_link_libraries(QDeskWidget PRIVATE
        shell32 ole32 dwmapi shlwapi user32 uuid
    )
endif()

install(TARGETS QDeskWidget
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Only deploy what the app actually loads.
#   --no-translations        -- no QTranslator is installed; UI strings are hard-coded zh_CN
#   --no-opengl-sw           -- QPainter raster painting, no QOpenGL/QQuick anywhere
#   --no-system-d3d-compiler -- D3DCompiler is only pulled in by the Qt Quick RHI backends
# NOTE: pass all three through DEPLOY_TOOL_OPTIONS instead of using the NO_TRANSLATIONS
# keyword -- Qt's macro appends that keyword as a bare string and the tool options as a
# list, so combining the two emits a malformed ";DEPLOY_TOOL_OPTIONS;..." argument.
# windeployqt has no switch for dxcompiler.dll / dxil.dll (still deployed unconditionally),
# so those -- plus the SDK's api-ms-win-* apiset stubs -- are removed by the TRIM block in
# package-msvc.bat.
qt_generate_deploy_app_script(
    TARGET QDeskWidget
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    DEPLOY_TOOL_OPTIONS
        --no-translations
        --no-opengl-sw
        --no-system-d3d-compiler
)
install(SCRIPT ${deploy_script})
