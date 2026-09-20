# ---- 产物不携带构建机专用路径 ----

# ---- 服务端源文件（自动发现） ----
file(GLOB SERVER_SOURCES CONFIGURE_DEPENDS src/*.cpp src/utils/*.cpp src/assurance/*.cpp src/metrics/*.cpp)
list(FILTER SERVER_SOURCES EXCLUDE REGEX ".*\\.bpf\\.c$")
list(FILTER SERVER_SOURCES EXCLUDE REGEX ".*history_query_tool\\.cpp$")
list(FILTER SERVER_SOURCES EXCLUDE REGEX ".*main\\.cpp$")
list(FILTER SERVER_SOURCES EXCLUDE REGEX ".*server\\.cpp$")

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
pkg_check_modules(NL80211 QUIET IMPORTED_TARGET libnl-3.0 libnl-genl-3.0)
find_library(SQLITE3_LIB sqlite3 REQUIRED)
target_link_libraries(server_lib PUBLIC
    PkgConfig::DBUS1 glog bpf pthread ${SQLITE3_LIB}
    $<$<BOOL:${NL80211_FOUND}>:PkgConfig::NL80211>)

# ---- W1: OpenSSL（TLS 探测能力）----
#
# 双路径探测，按优先级：
#   1. 私有 sysroot ${WEAKNET_OPENSSL_SYSROOT}（ARM64 构建容器的主路径）
#      libssl-dev 不经 APT 安装（bullseye-security 索引/pool 不一致导致
#      恒 404；bullseye/main 的 deb11u1 又与镜像已装 libssl1.1 deb11u8
#      版本冲突），改由 scripts/setup_openssl_sysroot.sh 以固定 .deb +
#      SHA256 校验 + dpkg-deb -x 解压得到。
#   2. 系统 OpenSSL（x86 开发机，实测为 3.5.5）
#
# 两个 backend 的 API 差异（1.1 vs 3.x）由源码内
# #if OPENSSL_VERSION_NUMBER 适配，CMake 只负责定位与链接。
#
# 链接范围 PRIVATE：公共头不暴露任何 OpenSSL 类型，不把 TLS 依赖
# 传播给 server_lib 的消费者。
#
# 找不到时不 fail：TLS 能力按项目语义降级为 NO_CAPABILITY
# （对应 SleResult.scope = NO_CAPABILITY、reason = no_tls_probe_capability），
# 而不是让整个构建失败。
set(WEAKNET_OPENSSL_SYSROOT "/opt/weaknet-openssl-1.1"
    CACHE PATH "OpenSSL 1.1 私有 sysroot（ARM64 构建容器）")

# 1) 私有 sysroot：NO_DEFAULT_PATH 确保不会误拾系统库
# 默认 FALSE：仅 sysroot 后端会置 TRUE（决定是否剥掉构建期 RUNPATH）
set(WEAKNET_TLS_IS_SYSROOT FALSE)
find_path(WEAKNET_OPENSSL_SYSROOT_INCLUDE_DIR openssl/ssl.h
    PATHS "${WEAKNET_OPENSSL_SYSROOT}/usr/include"
    NO_DEFAULT_PATH)
find_library(WEAKNET_OPENSSL_SYSROOT_SSL_LIB NAMES ssl
    PATHS "${WEAKNET_OPENSSL_SYSROOT}/usr/lib/aarch64-linux-gnu"
    NO_DEFAULT_PATH)
find_library(WEAKNET_OPENSSL_SYSROOT_CRYPTO_LIB NAMES crypto
    PATHS "${WEAKNET_OPENSSL_SYSROOT}/usr/lib/aarch64-linux-gnu"
    NO_DEFAULT_PATH)

if(WEAKNET_OPENSSL_SYSROOT_INCLUDE_DIR
   AND WEAKNET_OPENSSL_SYSROOT_SSL_LIB
   AND WEAKNET_OPENSSL_SYSROOT_CRYPTO_LIB)
    set(WEAKNET_TLS_FOUND TRUE)
    set(WEAKNET_TLS_IS_SYSROOT TRUE)
    set(WEAKNET_TLS_BACKEND "sysroot:${WEAKNET_OPENSSL_SYSROOT}")
    # Debian multiarch 布局：opensslconf.h 在 aarch64-linux-gnu/ 子目录下，
    # 只加 usr/include 会报 "openssl/opensslconf.h: No such file".
    set(WEAKNET_TLS_INCLUDE_DIRS
        "${WEAKNET_OPENSSL_SYSROOT_INCLUDE_DIR}"
        "${WEAKNET_OPENSSL_SYSROOT}/usr/include/aarch64-linux-gnu")
    set(WEAKNET_TLS_LIBRARIES
        "${WEAKNET_OPENSSL_SYSROOT_SSL_LIB}"
        "${WEAKNET_OPENSSL_SYSROOT_CRYPTO_LIB}")
else()
    # 2) 回退系统 OpenSSL。
    # 开发机（x86）若安装有特定版本的 OpenSSL（如 /usr/local 下 1.1），
    # 保持 find_package(OpenSSL) 的标准查找行为。
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        set(WEAKNET_TLS_FOUND TRUE)
        set(WEAKNET_TLS_BACKEND "system:${OPENSSL_VERSION}")
        set(WEAKNET_TLS_INCLUDE_DIRS "${OPENSSL_INCLUDE_DIR}")
        set(WEAKNET_TLS_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
    else()
        set(WEAKNET_TLS_FOUND FALSE)
        set(WEAKNET_TLS_IS_SYSROOT FALSE)
    endif()
endif()

if(WEAKNET_TLS_FOUND)
    message(STATUS "weaknet TLS: enabled (${WEAKNET_TLS_BACKEND})")
    target_include_directories(server_lib PRIVATE ${WEAKNET_TLS_INCLUDE_DIRS})
    target_link_libraries(server_lib PRIVATE ${WEAKNET_TLS_LIBRARIES})
    target_compile_definitions(server_lib PRIVATE WEAKNET_HAVE_TLS=1)
else()
    message(STATUS "weaknet TLS: not found; HTTPS probe capability will be NONE")
endif()

# ---- 边缘遥测上报：libcurl ----
#
# 与 OpenSSL 同样的双路径 + 私有 sysroot 纪律：
#   1. 私有 sysroot ${WEAKNET_CURL_SYSROOT}（ARM64 构建容器的主路径）
#      容器里 libcurl4 运行时已装（7.74.0-1.3+deb11u13），但**没有开发头文件**
#      与 libcurl.so 链接名；由 scripts/setup_libcurl_sysroot.sh 以固定 .deb +
#      SHA256 校验解压得到，不 apt install、不改镜像。
#   2. 系统 curl（x86 开发机，实测 8.12.1 + /usr/include/curl/curl.h）。
#
# 找不到时不 fail：上报能力按项目语义降级为"未启用"
# （exporter start() 直接返回 false 并写明原因），而不是让整个构建失败——
# 与 TLS 能力的处理一致。
set(WEAKNET_CURL_SYSROOT "/opt/weaknet-libcurl"
    CACHE PATH "libcurl 私有 sysroot（ARM64 构建容器）")

set(WEAKNET_CURL_FOUND FALSE)
set(WEAKNET_CURL_IS_SYSROOT FALSE)

find_path(WEAKNET_CURL_SYSROOT_INCLUDE_DIR curl/curl.h
    PATHS "${WEAKNET_CURL_SYSROOT}/usr/include/aarch64-linux-gnu"
    NO_DEFAULT_PATH)
find_library(WEAKNET_CURL_SYSROOT_LIB NAMES curl
    PATHS "${WEAKNET_CURL_SYSROOT}/usr/lib/aarch64-linux-gnu"
    NO_DEFAULT_PATH)

if(WEAKNET_CURL_SYSROOT_INCLUDE_DIR AND WEAKNET_CURL_SYSROOT_LIB)
    set(WEAKNET_CURL_FOUND TRUE)
    set(WEAKNET_CURL_IS_SYSROOT TRUE)
    set(WEAKNET_CURL_BACKEND "sysroot:${WEAKNET_CURL_SYSROOT}")
    # Debian multiarch 布局：curl 头文件在 usr/include/aarch64-linux-gnu/curl/ 下，
    # 同时加两条路径（curl.h 内部 include 的解析依赖 usr/include）。
    set(WEAKNET_CURL_INCLUDE_DIRS
        "${WEAKNET_CURL_SYSROOT_INCLUDE_DIR}"
        "${WEAKNET_CURL_SYSROOT}/usr/include/aarch64-linux-gnu")
    set(WEAKNET_CURL_LIBRARIES "${WEAKNET_CURL_SYSROOT_LIB}")
else()
    find_package(CURL QUIET)
    if(CURL_FOUND)
        set(WEAKNET_CURL_FOUND TRUE)
        set(WEAKNET_CURL_BACKEND "system:${CURL_VERSION_STRING}")
        set(WEAKNET_CURL_INCLUDE_DIRS "${CURL_INCLUDE_DIRS}")
        set(WEAKNET_CURL_LIBRARIES CURL::libcurl)
    endif()
endif()

if(WEAKNET_CURL_FOUND)
    message(STATUS "weaknet edge export: enabled (libcurl ${WEAKNET_CURL_BACKEND})")
    target_include_directories(server_lib PRIVATE ${WEAKNET_CURL_INCLUDE_DIRS})
    target_link_libraries(server_lib PRIVATE ${WEAKNET_CURL_LIBRARIES})
    target_compile_definitions(server_lib PRIVATE WEAKNET_HAVE_CURL=1)
else()
    message(STATUS
        "weaknet edge export: libcurl not found; edge telemetry upload will be disabled")
endif()

# 只有**私有 sysroot** 后端需要剥掉构建期 RPATH。
#
# 背景：sysroot 位于 /opt/weaknet-openssl-1.1/...，不在 linker 默认搜索路径内，
# 于是 CMake 给链接它的目标加了 RUNPATH=<sysroot>/usr/lib/aarch64-linux-gnu。
# 该路径是构建期产物，**在开发板上不存在**；把它固化进交付产物会误导依赖排查
# （ldd 显示这条路径，实际运行走的是系统库 —— 因为 SONAME 就是 libssl.so.1.1，
# 板端 /usr/lib/aarch64-linux-gnu/libssl.so.1.1 已具备，运行不依赖 sysroot）。
#
# 系统后端（x86 开发机）**不能剥**：那里的 libssl.so.1.1 只在 /usr/local/lib64，
# 既不在 ld.so.conf 也没被 ldconfig 收录，运行完全依赖 OpenSSL::SSL 导入目标
# 携带的 RPATH=/usr/local/lib64。剥掉会直接导致
# "error while loading shared libraries: libssl.so.1.1"（已实测）。
#
# 注意 SKIP_BUILD_RPATH 是**链接期**属性，必须设在最终可执行目标上。
# 设在 server_lib（静态库）上无效 —— 静态库不参与链接，属性不会传播，
# 实测 ARM64 产物仍带 RUNPATH。
function(weaknet_strip_build_rpath target)
    if(WEAKNET_TLS_IS_SYSROOT)
        set_target_properties(${target} PROPERTIES SKIP_BUILD_RPATH TRUE)
    endif()
endfunction()

# ---- 服务端可执行文件 ----
add_executable(weaknet-dbus-server src/main.cpp src/server.cpp)
target_link_libraries(weaknet-dbus-server PRIVATE server_lib)
weaknet_strip_build_rpath(weaknet-dbus-server)

# ---- 历史数据查询工具 ----
add_executable(history_query_tool src/history_query_tool.cpp)
target_link_libraries(history_query_tool PRIVATE server_lib)
weaknet_strip_build_rpath(history_query_tool)

# ---- eBPF 程序编译（自定义命令） ----
find_program(CLANG clang REQUIRED)

# eBPF 产物输出到当前架构的 CMake 构建目录；部署时映射到 server/build/
set(BPF_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/build")
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
    tcp_conn_stats
    skb_drop
    tcp_connect
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
        ${BPF_BUILD_DIR}/tcp_conn_stats.bpf.o
        ${BPF_BUILD_DIR}/skb_drop.bpf.o
        ${BPF_BUILD_DIR}/a2dp_media.bpf.o
        ${BPF_BUILD_DIR}/tcp_connect.bpf.o
)
# eBPF 依赖：在 ARM64 容器内编译时链接，x86 开发机可跳过
option(BUILD_EBPF "Build eBPF programs (requires ARM64 or native BTF)" ON)
if(BUILD_EBPF)
    add_dependencies(weaknet-dbus-server ebpf)
endif()

# ---- 测试 ----
add_subdirectory(test)
