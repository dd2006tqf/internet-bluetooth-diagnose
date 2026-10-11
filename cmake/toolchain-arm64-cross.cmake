# ============================================================================
# cmake/toolchain-arm64-cross.cmake
# 宿主机裸机原生交叉编译工具链（x86_64 宿主机直接编译 -> aarch64 目标板）
#
# 核心设计：
# 1. 完全脱离 Docker 容器与 QEMU 模拟，直接在宿主机 x86 CPU 上运行原生 Clang，
#    吃满全部 CPU 核心并行编译 (-j$(nproc))，全量构建从 15 分钟降至 ~30 秒。
# 2. 通过 --sysroot=/opt/sysroot-bullseye-arm64 与 --gcc-toolchain 指向从
#    Bullseye 提取的 GCC 10 / glibc 2.31 运行时树，彻底隔绝宿主机高版本 glibc 2.39，
#    产出的 ELF 最高只依赖 GLIBC_2.30 及以下，与开发板 100% 二进制兼容。
# ============================================================================

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TARGET_TRIPLE "aarch64-linux-gnu")
set(SYSROOT_DIR "/opt/sysroot-bullseye-arm64")

set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${TARGET_TRIPLE})

set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET ${TARGET_TRIPLE})

set(CMAKE_SYSROOT ${SYSROOT_DIR})
set(CMAKE_FIND_ROOT_PATH ${SYSROOT_DIR})

# 告知 generate_vmlinux.sh 正在针对 aarch64 交叉编译，强制拷贝 board-assets/vmlinux.h
set(ENV{TARGET_ARCH} "aarch64")

# 关键：--gcc-toolchain=${SYSROOT_DIR}/usr 让 Clang 使用 sysroot 内的 GCC 10 (crtbeginS.o / libstdc++)，
# 而不去捡宿主机 Ubuntu 的 GCC 13 (glibc 2.39)。
set(COMMON_CROSS_FLAGS "--target=${TARGET_TRIPLE} --sysroot=${SYSROOT_DIR} --gcc-toolchain=${SYSROOT_DIR}/usr -Wno-unused-command-line-argument")

set(CMAKE_C_FLAGS_INIT "${COMMON_CROSS_FLAGS} -isystem ${SYSROOT_DIR}/usr/local/include")
set(CMAKE_CXX_FLAGS_INIT "${COMMON_CROSS_FLAGS} -stdlib=libstdc++ -isystem ${SYSROOT_DIR}/usr/local/include")

set(COMMON_LINKER_FLAGS "${COMMON_CROSS_FLAGS} -L${SYSROOT_DIR}/usr/local/lib -L${SYSROOT_DIR}/usr/lib/aarch64-linux-gnu -L${SYSROOT_DIR}/lib/aarch64-linux-gnu -Wl,-rpath-link,${SYSROOT_DIR}/usr/local/lib -Wl,-rpath-link,${SYSROOT_DIR}/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,${SYSROOT_DIR}/lib/aarch64-linux-gnu")

set(CMAKE_EXE_LINKER_FLAGS_INIT "${COMMON_LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${COMMON_LINKER_FLAGS}")

# 资源搜索隔离策略
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Pkg-config 交叉编译环境变量注入
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR} "${SYSROOT_DIR}/usr/lib/aarch64-linux-gnu/pkgconfig:${SYSROOT_DIR}/usr/local/lib/pkgconfig:${SYSROOT_DIR}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SYSROOT_DIR}")

# 私有依赖路径映射
set(WEAKNET_OPENSSL_SYSROOT "${SYSROOT_DIR}/opt/weaknet-openssl-1.1" CACHE PATH "" FORCE)
set(WEAKNET_CURL_SYSROOT "${SYSROOT_DIR}/opt/weaknet-libcurl" CACHE PATH "" FORCE)
