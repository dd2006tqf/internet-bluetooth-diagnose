# 客户端动态库
add_library(weaknet SHARED client.cpp)
target_include_directories(weaknet PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/server/include)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS1 REQUIRED IMPORTED_TARGET dbus-1)

# 客户端链接弱网公共代码库（serializer + logger）
target_link_libraries(weaknet PRIVATE
    weaknet_common
    PkgConfig::DBUS1 glog pthread)
# ---- 对外 ABI 边界 ----
#
# 1) version script 收敛导出符号（I1）。
#    此前用默认可见性：实测 421 个动态符号里只有 39 个是 `weaknet_` 前缀，
#    剩下 382 个是 C++ 模板实例 / std:: 弱定义 / RTTI —— 内部类名被固化进
#    ABI，改个实现细节就可能在不相关的下游产生符号缺失。白名单见
#    client/weaknet.map（只放行 weaknet_*）。
#
#    这里**不**加 `CXX_VISIBILITY_PRESET hidden`：-fvisibility=hidden 会在
#    编译期把符号降成 hidden，而 version script 只能把 global 降成 local、
#    无法把 hidden 提升回来 —— 两者同时用会让 C API 一起消失（空导出表）。
#
# 2) SOVERSION：`weaknet_client.h` 是对外 C 契约，历史上移除过 8 个导出符号
#    （见 docs/架构设计-模块与代码结构.md），属破坏性变更，因此按主版本号
#    定 SO name：产物为 libweaknet.so.<major>.<minor>.<patch> + 两条软链。
#
#    **消费方注意**：链接期用的是 `libweaknet.so`，但运行期 DT_NEEDED 记的是
#    `libweaknet.so.<major>`。任何部署路径都必须带上真实文件与 SO name 软链，
#    只拷 `libweaknet.so` 会在运行时报 "cannot open shared object file"。
set_target_properties(weaknet PROPERTIES
    OUTPUT_NAME weaknet
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/weaknet.map"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/client/lib)

# 客户端测试程序
add_executable(test_client_bin test_client.cpp)
target_link_libraries(test_client_bin PRIVATE weaknet PkgConfig::DBUS1 glog pthread)
set_target_properties(test_client_bin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/client/bin)

add_executable(test_lifecycle_api test_lifecycle_api.cpp)
target_link_libraries(test_lifecycle_api PRIVATE weaknet PkgConfig::DBUS1 glog pthread)

add_executable(weaknet_cli weaknet_cli.cpp)
target_link_libraries(weaknet_cli PRIVATE weaknet PkgConfig::DBUS1 glog pthread)
set_target_properties(weaknet_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/client/bin)

# 系统健康看门狗守护进程（2026-11-14）
#
# 刻意不链接 weaknet 库与 libdbus：它只需要 stat/文件读/子进程，
# 而 D-Bus 判据走 busctl 子进程（2 秒一次、实测 25ms，可接受）。
# 这样它在最小镜像里也能单独跑起来，不引入 libdbus 的加载依赖。
#
# 注意它**不是**普通客户端工具：打开 /dev/watchdog 后不喂狗就会
# 触发硬件超时复位整机。源码在 tools/ 下（运维工具定位），但产物要随
# 发布包部署到板端，因此在这里构建。
add_executable(weaknet_health_guard
    ${CMAKE_SOURCE_DIR}/tools/weaknet-health-guard.cpp)
target_compile_features(weaknet_health_guard PRIVATE cxx_std_17)
set_target_properties(weaknet_health_guard PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/client/bin)

# 示例/演示程序：EXCLUDE_FROM_ALL——只在显式 --target 时构建。
# 它们不发布到 dist-arm64、不被 ctest/板端测试消费，全量编译（尤其
# ARM64 QEMU 模拟）没必要为它们付时间。
add_executable(example_usage example_usage.cpp)
target_link_libraries(example_usage PRIVATE weaknet PkgConfig::DBUS1 glog pthread)
set_target_properties(example_usage PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/client/bin
    EXCLUDE_FROM_ALL TRUE)

add_executable(ping_example ping_example.cpp)
target_link_libraries(ping_example PRIVATE weaknet PkgConfig::DBUS1 glog pthread)
set_target_properties(ping_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/client/bin
    EXCLUDE_FROM_ALL TRUE)

# test_network_quality 已删除（2026-11-01）：该程序唯一用途是演示
# NetworkQualityChanged 信号的订阅/轮询，而服务端自 2026-10-19 起停发
# 全部 D-Bus 信号，客户端订阅族于 2026-11-01 一并删除。留着它只能编译失败。

add_test(NAME test_lifecycle_api COMMAND test_lifecycle_api)
