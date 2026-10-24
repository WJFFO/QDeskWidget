# 把插件 DLL 复制到用户数据目录。
#
# 为什么不用 add_custom_command 直接 copy_if_different：
# QDeskWidget 正在运行时 HelloClockPlugin.dll 是被加载的，复制必然失败，
# 而 add_custom_command 里任何一条命令失败都会让整个构建失败 ——
# 改个无关代码却报 "Error copying file"，很容易误导。
#
# 这里改成用 execute_process，失败只打 warning。
#
# 用法：
#   cmake -DPLUGIN_SRC=<dll> -DPLUGIN_DIR=<目标目录> -P install_plugin.cmake

if(NOT DEFINED PLUGIN_SRC OR NOT DEFINED PLUGIN_DIR)
    message(WARNING "install_plugin.cmake: 缺少 PLUGIN_SRC / PLUGIN_DIR")
    return()
endif()

if(NOT EXISTS "${PLUGIN_SRC}")
    return()
endif()

file(MAKE_DIRECTORY "${PLUGIN_DIR}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PLUGIN_SRC}" "${PLUGIN_DIR}"
    RESULT_VARIABLE _result
    OUTPUT_QUIET
    ERROR_QUIET
)

if(NOT _result EQUAL 0)
    get_filename_component(_name "${PLUGIN_SRC}" NAME)
    message(WARNING
        "插件 ${_name} 没能复制到 ${PLUGIN_DIR} —— QDeskWidget 可能正在运行。\n"
        "关闭程序后重新构建即可；当前主程序仍能用，只是插件库里还是旧版本。")
endif()
