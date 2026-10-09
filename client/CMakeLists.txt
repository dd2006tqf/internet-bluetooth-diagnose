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
set_target_properties(weaknet PROPERTIES
    OUTPUT_NAME weaknet
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

add_executable(test_network_quality test_network_quality.cpp)
target_link_libraries(test_network_quality PRIVATE weaknet PkgConfig::DBUS1 glog pthread)
set_target_properties(test_network_quality PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/client/bin
    EXCLUDE_FROM_ALL TRUE)

add_test(NAME test_lifecycle_api COMMAND test_lifecycle_api)
