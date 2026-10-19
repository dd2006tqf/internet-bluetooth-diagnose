cmake_minimum_required(VERSION 3.16)

# 版本单一来源：仓库根 VERSION 文件（一行 semver，如 "1.0.0"）。
# OTA 发布打包与 --version 输出都读这里；缺失或非法时回退 0.0.0。
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WEAKNET_VERSION_FILE LIMIT_COUNT 1)
string(STRIP "${WEAKNET_VERSION_FILE}" WEAKNET_VERSION_FILE)
if(NOT WEAKNET_VERSION_FILE MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(WEAKNET_VERSION_FILE "0.0.0")
endif()

project(weaknet VERSION ${WEAKNET_VERSION_FILE} LANGUAGES C CXX)

# CTest 集成（必须在 add_subdirectory 之前）
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ccache 自动检测
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# 通用编译选项（匹配现有 Makefile: -O2 -Wall -Wextra -Wpedantic）
add_compile_options(-Wall -Wextra -Wpedantic)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 子目录
add_subdirectory(server)
add_subdirectory(client)
