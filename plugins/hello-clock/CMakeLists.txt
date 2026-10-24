cmake_minimum_required(VERSION 3.19)

# 示例组件插件：编译成一个 DLL，放进「数据目录/plugins」即可被主程序加载。
#
# 只依赖 Qt 的 Core/Gui/Widgets，以及主程序的 src/widgets/IWidgetPlugin.h（纯头文件接口）。
# 你可以直接复制这个目录作为自己组件的起点。

project(HelloClockPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)

add_library(HelloClockPlugin MODULE
    HelloClockPlugin.cpp
)

target_include_directories(HelloClockPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(HelloClockPlugin PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if (MSVC)
    target_compile_options(HelloClockPlugin PRIVATE /utf-8 /Zc:__cplusplus /permissive-)
    # 与主程序共用同一个「头文件戳」：IWidgetPlugin.h 改了插件也要重编
    if (QDW_HEADER_STAMP)
        set_source_files_properties(HelloClockPlugin.cpp
            PROPERTIES OBJECT_DEPENDS ${QDW_HEADER_STAMP})
    endif()
endif()

set_target_properties(HelloClockPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    PREFIX ""
)

# 构建后自动拷到数据目录（exe 同级的 QDeskWidget/plugins），启动程序就能看到「示例时钟（插件）」
# 走脚本而不是直接 copy：程序开着时 DLL 被占用，失败只警告、不中断构建。
if (WIN32)
    add_custom_command(TARGET HelloClockPlugin POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DPLUGIN_SRC=$<TARGET_FILE:HelloClockPlugin>
                -DPLUGIN_DIR=$<TARGET_FILE_DIR:QDeskWidget>/QDeskWidget/plugins
                -P ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/install_plugin.cmake
        COMMENT "复制示例插件到 <exe>/QDeskWidget/plugins"
    )
endif()
