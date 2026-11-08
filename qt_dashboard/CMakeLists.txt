# Qt 本地看板（实验性，独立构建）
#
# ## 为什么不接入根 CMakeLists.txt
#
# 根构建面向开发板部署产物（server/client/bpf），看板是**可选的人机界面**：
#   - 不是所有部署都装显示器
#   - 板端目前没有 Qt 开发包，只有运行时
#
# 因此做成独立 project，需要时单独 configure：
#
#   cmake -S qt_dashboard -B build-qt-dashboard
#   cmake --build build-qt-dashboard -j$(nproc)
#
# ## 数据层可脱离 Qt 构建
#
# 只想要数据层（JSON → POD）时不传 `-DWEAKN_DASHBOARD_WITH_UI=ON`，
# 此时只编译 data/ 与 tests/，不依赖任何 Qt 头文件。
# 这在没有 Qt 开发包的机器上仍能跑通解析层单测。

cmake_minimum_required(VERSION 3.16)
project(weaknet_qt_dashboard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WEAKN_DASHBOARD_WITH_UI "编译 Qt UI 层" OFF)
option(WEAKN_DASHBOARD_BUILD_TESTS "编译单元测试" ON)

# ---------------------------------------------------------------------------
# 数据层（不依赖 Qt）
# ---------------------------------------------------------------------------

add_library(weaknet_dashboard_data STATIC
    data/json_reader.cpp
    data/dashboard_data.cpp
)

target_include_directories(weaknet_dashboard_data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# 图表几何层（不依赖 Qt）
#
# 轴范围计算与坐标映射是纯算术，从 LineChartWidget 里拆出来单独成 target。
# 这样「RTT=150 应该画在轴中间」这类判断可以直接断言，
# 不必先渲染成 QImage 再去数像素间接反推。
#
# 代价是这个 target 只有 chart_geometry.cpp 一个文件，看起来"为一个文件建库"。
# 收益是它可以在没有 Qt 开发包的机器上编译并跑测试——本机就是这种情况
# （只有 qtbase5-dev-tools 即 moc，没有 qtbase5-dev 本体）。
# ---------------------------------------------------------------------------

add_library(weaknet_dashboard_geometry STATIC
    ui/chart_geometry.cpp
)

target_include_directories(weaknet_dashboard_geometry PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(weaknet_dashboard_geometry PUBLIC
    weaknet_dashboard_data
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(weaknet_dashboard_geometry PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# 取数层（依赖 libweaknet 的 C API，但不依赖 Qt）
#
# 单独一个 target：数据层可以完全没有 D-Bus 依赖地单测，
# 取数层则必须链接 libweaknet。
# ---------------------------------------------------------------------------

add_library(weaknet_dashboard_source STATIC
    data/data_source.cpp
)

target_include_directories(weaknet_dashboard_source PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../client
)

target_link_libraries(weaknet_dashboard_source PUBLIC
    weaknet_dashboard_data
)

# libweaknet 由上层项目构建。这里不硬编码路径，允许调用方通过
# -DWEAKNET_CLIENT_LIB=<path> 覆盖，便于在板端直接链接已安装的库。
if(WEAKNET_CLIENT_LIB)
    target_link_libraries(weaknet_dashboard_source PUBLIC ${WEAKNET_CLIENT_LIB})
    message(STATUS "链接 libweaknet: ${WEAKNET_CLIENT_LIB}")
else()
    message(WARNING
        "未指定 WEAKNET_CLIENT_LIB，取数层将无法链接。"
        "构建带 UI 的目标时请传 -DWEAKNET_CLIENT_LIB=/path/to/libweaknet.so")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(weaknet_dashboard_data PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# UI 层（依赖 Qt）
# ---------------------------------------------------------------------------

if(WEAKN_DASHBOARD_WITH_UI)
    # 同时接受 Qt6 和 Qt5，优先 Qt6。
    # 代码里用 QT_VERSION_MAJOR 区分，不硬编码 Qt5:: / Qt6::。
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

    message(STATUS "Qt 看板使用 Qt${QT_VERSION_MAJOR}")

    # -----------------------------------------------------------------------
    # 交叉编译：moc/rcc/uic 必须用**宿主机**版本
    #
    # 从 sysroot 里 find_package 进来的 Qt 会把 `Qt5::moc` 的
    # IMPORTED_LOCATION 指向 `${_qt5Core_install_prefix}/lib/qt5/bin/moc`，
    # 那是 **aarch64 二进制**，在 x86 宿主机上跑起来只会得到
    # `ELF ...: not found` / `Syntax error: word unexpected`。
    #
    # `-DQT_MOC_EXECUTABLE=...` 没用：`Qt5CoreConfigExtras.cmake:43` 在
    # find_package 期间就把这个变量重新赋成了 `Qt5::moc` 这个 target 名，
    # 命令行传进去的值被覆盖。只能在这里、find_package 之后重设
    # IMPORTED_LOCATION。
    #
    # 判据用「能不能在本机执行」而不是「路径里有没有 sysroot」：
    # 原生构建（宿主机自己就是目标架构）时这两条路径本来就可执行，不该干预。
    # -----------------------------------------------------------------------
    if(CMAKE_CROSSCOMPILING)
        foreach(__tool moc rcc uic)
            if(TARGET Qt${QT_VERSION_MAJOR}::${__tool})
                get_target_property(__qt_tool_location
                    Qt${QT_VERSION_MAJOR}::${__tool} IMPORTED_LOCATION)
                if(__qt_tool_location)
                    execute_process(
                        COMMAND "${__qt_tool_location}" -v
                        RESULT_VARIABLE __qt_tool_rc
                        OUTPUT_QUIET ERROR_QUIET)
                    if(NOT __qt_tool_rc EQUAL 0)
                        # 目标架构的二进制跑不了，换成宿主机上的同名工具
                        find_program(__qt_host_${__tool}
                            NAMES ${__tool}
                            PATHS /usr/lib/qt5/bin /usr/lib/qt6/libexec
                                  /usr/lib/x86_64-linux-gnu/qt5/bin
                            NO_DEFAULT_PATH)
                        if(__qt_host_${__tool})
                            message(STATUS
                                "交叉编译：${__tool} 使用宿主机版本 "
                                "${__qt_host_${__tool}}（sysroot 里的是目标架构）")
                            set_target_properties(Qt${QT_VERSION_MAJOR}::${__tool}
                                PROPERTIES IMPORTED_LOCATION
                                "${__qt_host_${__tool}}")
                        else()
                            message(WARNING
                                "交叉编译但找不到宿主机版 ${__tool}。"
                                "Automoc 会在构建时因无法执行 sysroot 里的 "
                                "目标架构二进制而失败。"
                                "请安装：apt install qtbase5-dev-tools")
                        endif()
                    endif()
                endif()
            endif()
        endforeach()
    endif()

    set(CMAKE_AUTOMOC ON)

    add_library(weaknet_dashboard_ui STATIC
        ui/theme.cpp
        ui/line_chart_widget.cpp
        ui/data_fetch_thread.cpp
        ui/main_window.cpp
    )

    target_include_directories(weaknet_dashboard_ui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(weaknet_dashboard_ui PUBLIC
        weaknet_dashboard_source
        weaknet_dashboard_geometry
        Qt${QT_VERSION_MAJOR}::Widgets
    )

    # 可执行文件
    add_executable(weaknet_dashboard main.cpp)
    target_link_libraries(weaknet_dashboard PRIVATE
        weaknet_dashboard_ui
        Qt${QT_VERSION_MAJOR}::Widgets
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(weaknet_dashboard_ui PRIVATE -Wall -Wextra)
        target_compile_options(weaknet_dashboard PRIVATE -Wall -Wextra)
    endif()
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------

if(WEAKN_DASHBOARD_BUILD_TESTS)
    include(CTest)
    find_package(GTest QUIET)

    if(GTest_FOUND)
        add_executable(test_dashboard_data tests/test_json_reader.cpp)
        target_link_libraries(test_dashboard_data PRIVATE
            weaknet_dashboard_data
            GTest::gtest
            GTest::gtest_main
        )
        add_test(NAME dashboard_data COMMAND test_dashboard_data)

        # 几何层：不需要 Qt，因此不受 WEAKN_DASHBOARD_WITH_UI 约束。
        # 这是本机（无 qtbase5-dev）唯一能验证的 UI 相关逻辑。
        add_executable(test_chart_geometry tests/test_chart_geometry.cpp)
        target_link_libraries(test_chart_geometry PRIVATE
            weaknet_dashboard_geometry
            weaknet_dashboard_data
            GTest::gtest
            GTest::gtest_main
        )
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(test_chart_geometry PRIVATE -Wall -Wextra)
        endif()
        add_test(NAME chart_geometry COMMAND test_chart_geometry)

        # UI 层像素测试：必须有 Qt，否则 LineChartWidget 根本不存在。
        # 这里不要求 libweaknet —— paint() 只依赖数据结构与 Qt，不碰 D-Bus。
        if(WEAKN_DASHBOARD_WITH_UI)
            add_executable(test_line_chart_widget tests/test_line_chart_widget.cpp)
            target_link_libraries(test_line_chart_widget PRIVATE
                weaknet_dashboard_ui
                weaknet_dashboard_data
                Qt${QT_VERSION_MAJOR}::Widgets
                GTest::gtest
                GTest::gtest_main
            )
            if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                target_compile_options(test_line_chart_widget PRIVATE -Wall -Wextra)
            endif()
            add_test(NAME line_chart_widget COMMAND test_line_chart_widget)

            # 主题测试：验证全局 QSS 真的落到控件上，以及圆点跟随状态着色。
            # 与像素测试同样要求 offscreen 平台。
            add_executable(test_theme tests/test_theme.cpp)
            target_link_libraries(test_theme PRIVATE
                weaknet_dashboard_ui
                weaknet_dashboard_data
                Qt${QT_VERSION_MAJOR}::Widgets
                GTest::gtest
                GTest::gtest_main
            )
            if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                target_compile_options(test_theme PRIVATE -Wall -Wextra)
            endif()
            add_test(NAME theme COMMAND test_theme)

            # 展示态控制的交互测试：验证「切指标不重置窗口」这类语义。
            # 必须真跑 QWidget（下拉框、按钮点击），因此依赖 UI 层。
            # 不启动 fetcher：Windows 构造后不会自动取数，
            # 切换动作只改状态与触发一次 requestRefresh()，不发 D-Bus 请求。
            add_executable(test_main_window tests/test_main_window.cpp)
            target_link_libraries(test_main_window PRIVATE
                weaknet_dashboard_ui
                weaknet_dashboard_data
                Qt${QT_VERSION_MAJOR}::Widgets
                GTest::gtest
                GTest::gtest_main
            )
            if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                target_compile_options(test_main_window PRIVATE -Wall -Wextra)
            endif()
            add_test(NAME main_window COMMAND test_main_window)

            # 像素测试用 offscreen 平台，不需要显示器。
            # 不设 QT_QPA_PLATFORM 时，无 X11/Wayland 的机器上 Qt 会直接
            # abort（"could not connect to display"），测试就变成"环境挂了"
            # 而不是"代码错了"。
            set_tests_properties(line_chart_widget PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            )
            set_tests_properties(theme PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            )
            set_tests_properties(main_window PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            )
        endif()
    else()
        message(WARNING "未找到 GTest，跳过数据层单元测试")
    endif()
endif()
