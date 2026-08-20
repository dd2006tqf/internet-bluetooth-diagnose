cmake_minimum_required(VERSION 3.16)
project(weaknet VERSION 1.0.0 LANGUAGES C CXX)

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
