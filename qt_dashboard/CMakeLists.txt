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

    set(CMAKE_AUTOMOC ON)

    add_library(weaknet_dashboard_ui STATIC
        ui/line_chart_widget.cpp
        ui/data_fetch_thread.cpp
        ui/main_window.cpp
    )

    target_include_directories(weaknet_dashboard_ui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(weaknet_dashboard_ui PUBLIC
        weaknet_dashboard_source
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
    else()
        message(WARNING "未找到 GTest，跳过数据层单元测试")
    endif()
endif()
