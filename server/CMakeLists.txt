# ---- 服务端源文件（自动发现） ----
file(GLOB SERVER_SOURCES CONFIGURE_DEPENDS src/*.cpp src/utils/*.cpp)
list(FILTER SERVER_SOURCES EXCLUDE REGEX ".*\\.bpf\\.c$")
list(FILTER SERVER_SOURCES EXCLUDE REGEX ".*history_query_tool\\.cpp$")
list(FILTER SERVER_SOURCES EXCLUDE REGEX ".*main\\.cpp$")

# ---- 公共代码库（客户端和服务端共享 serializer + logger + json_escape） ----
add_library(weaknet_common STATIC src/serializer.cpp src/logger.cpp src/utils/json_escape.cpp)
target_include_directories(weaknet_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(weaknet_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- 服务端静态库（供测试链接） ----
add_library(server_lib STATIC ${SERVER_SOURCES})
target_include_directories(server_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(PkgConfig REQUIRED)
# QEMU ARM64 模拟下 cmake try_compile 会 segfault，手动设置 pthread
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
pkg_check_modules(DBUS1 REQUIRED IMPORTED_TARGET dbus-1)
find_library(SQLITE3_LIB sqlite3 REQUIRED)
target_link_libraries(server_lib PUBLIC
    PkgConfig::DBUS1 glog bpf pthread ${SQLITE3_LIB})

# ---- 服务端可执行文件 ----
add_executable(weaknet-dbus-server src/main.cpp src/server.cpp)
target_link_libraries(weaknet-dbus-server PRIVATE server_lib)

# ---- 历史数据查询工具 ----
add_executable(history_query_tool src/history_query_tool.cpp)
target_link_libraries(history_query_tool PRIVATE server_lib)

# ---- eBPF 程序编译（自定义命令） ----
find_program(CLANG clang REQUIRED)

# eBPF 产物输出到 CMake 构建目录（避免与旧 Makefile 的 server/build/ 冲突）
set(BPF_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/ebpf")
file(MAKE_DIRECTORY ${BPF_BUILD_DIR})

set(VMLINUX_H "${BPF_BUILD_DIR}/vmlinux.h")

add_custom_command(
    OUTPUT ${VMLINUX_H}
    COMMAND ${CMAKE_SOURCE_DIR}/cmake/generate_vmlinux.sh ${VMLINUX_H} ${CMAKE_SOURCE_DIR}/board-assets/vmlinux.h
    COMMENT "Generating vmlinux.h"
)

# eBPF 程序列表（src/ 目录下）
set(BPF_PROGRAMS
    flow_rate
    tcp_retransmit
    dns_monitor
    wifi_packet_loss
    http_latency
)

foreach(BPF_PROG ${BPF_PROGRAMS})
    add_custom_command(
        OUTPUT ${BPF_BUILD_DIR}/${BPF_PROG}.bpf.o
        COMMAND ${CLANG} -g -O2 -target bpf
            -I${BPF_BUILD_DIR}
            -c ${CMAKE_CURRENT_SOURCE_DIR}/src/${BPF_PROG}.bpf.c
            -o ${BPF_BUILD_DIR}/${BPF_PROG}.bpf.o
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/src/${BPF_PROG}.bpf.c
            ${VMLINUX_H}
        COMMENT "Compiling eBPF: ${BPF_PROG}.bpf.c"
    )
endforeach()

# a2dp_media 在子目录 src/bpf/
add_custom_command(
    OUTPUT ${BPF_BUILD_DIR}/a2dp_media.bpf.o
    COMMAND ${CLANG} -g -O2 -target bpf
        -I${BPF_BUILD_DIR}
        -c ${CMAKE_CURRENT_SOURCE_DIR}/src/bpf/a2dp_media.bpf.c
        -o ${BPF_BUILD_DIR}/a2dp_media.bpf.o
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bpf/a2dp_media.bpf.c
        ${VMLINUX_H}
    COMMENT "Compiling eBPF: a2dp_media.bpf.c"
)

# 汇总 eBPF 目标（默认构建，但不阻塞服务端二进制）
add_custom_target(ebpf
    DEPENDS
        ${BPF_BUILD_DIR}/flow_rate.bpf.o
        ${BPF_BUILD_DIR}/tcp_retransmit.bpf.o
        ${BPF_BUILD_DIR}/dns_monitor.bpf.o
        ${BPF_BUILD_DIR}/wifi_packet_loss.bpf.o
        ${BPF_BUILD_DIR}/http_latency.bpf.o
        ${BPF_BUILD_DIR}/a2dp_media.bpf.o
)
# eBPF 依赖：在 ARM64 容器内编译时链接，x86 开发机可跳过
option(BUILD_EBPF "Build eBPF programs (requires ARM64 or native BTF)" ON)
if(BUILD_EBPF)
    add_dependencies(weaknet-dbus-server ebpf)
endif()

# ---- 测试 ----
add_subdirectory(test)
