# 客户端动态库
add_library(weaknet SHARED client.cpp)
target_include_directories(weaknet PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/server/include)

find_package(PkgConfig REQUIRED)
# 复用服务端的 pthread 配置
pkg_check_modules(DBUS1 REQUIRED IMPORTED_TARGET dbus-1)

# 客户端库依赖服务端源码（serializer、logger）
target_sources(weaknet PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src/serializer.cpp
    ${CMAKE_SOURCE_DIR}/server/src/logger.cpp)
target_link_libraries(weaknet PRIVATE
    PkgConfig::DBUS1 glog pthread)
set_target_properties(weaknet PROPERTIES
    OUTPUT_NAME weaknet
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# 客户端测试程序
add_executable(test_client_bin test_client.cpp)
target_link_libraries(test_client_bin PRIVATE weaknet PkgConfig::DBUS1 glog pthread)
set_target_properties(test_client_bin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
