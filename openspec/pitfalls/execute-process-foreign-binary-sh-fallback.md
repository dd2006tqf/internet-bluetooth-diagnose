# `execute_process` 探测非本机架构的可执行文件，会通过 /bin/sh 回退在 cwd 里生成垃圾文件

> 发现于：2026-11-14
> 位置：`qt_dashboard/CMakeLists.txt` 的「交叉编译换宿主机 moc/rcc/uic」段（已改）
> 影响：每次 cmake configure 在仓库根目录生成一个名字是 ELF 内部字节、内容为空的垃圾文件；名字含非法 UTF-8，`rm s5*` 之类的 glob 删不掉

## 现象

仓库根目录反复出现一个 0 字节文件，`git status` 里显示为一串乱码：

```text
?? "s5\242\271\352\203\222\315ay\270P\4\20\1GNU\3\7\1\1\1\3\v\370D\3\31\10\23\16\4\1\22H\3\22..."
```

它的原始字节是：

```python
b's5\xa2\xb9\xea\x83\x92\xcday\xb8P\x04\x10\x01GNU\x03\x07\x01\x01\x01\x03\x0b\xf8D\x03...'
```

两个关键特征：

1. **每次 `./tools/ci.sh` 都会重新生成**（删掉后跑一次构建又出现）
2. **`rm -f s5*` 删不掉**——文件名不是合法 UTF-8，shell glob 匹配不上，只能用 Python 按 bytes 删：
   ```python
   for n in os.listdir(b'.'):
       if b'GNU' in n and n.startswith(b's5'):
           os.unlink(n)
   ```

## 排查过程（三次走错路）

| 尝试 | 结果 | 为什么没找到 |
| --- | --- | --- |
| `strace -e trace=file` 跟 ci.sh | 无命中 | 用错判据：先按 inode 找，但文件每次重建 inode 会变；后来又误以为「没删掉」 |
| 只跑主构建 `cmake --build build-arm64-native` | 不产生 | 正确——问题不在主构建，但当时还没排除 |
| 用 `file -b` 输出怀疑 | 无控制字符 | 是被自己加的那行日志带偏了 |

**转折点**：改用「分阶段二分」，发现只有 **Qt 看板 configure** 会生成它，然后用 strace 抓 `openat(..., O_CREAT)` 且带 `-y`（显示 fd 路径），一次命中。

## 成因

strace 抓到完整链路：

```text
execve("/opt/sysroot-bullseye-arm64/usr/lib/qt5/bin/rcc", [..., "-v"], ...)     = -1 ENOEXEC
execve("/bin/sh", ["/bin/sh", "/opt/.../qt5/bin/rcc", "-v"], ...)              = 0
openat(AT_FDCWD, "/etc/ld.so.preload", ...)                                    = 4
openat(AT_FDCWD, "s5\242\271\352\203\222\315ay\270P\4\20\1GNU\3\7...",
       O_WRONLY|O_CREAT|O_TRUNC, 0666)                                         = 4
```

三步解释：

1. **`execve` 返回 `ENOEXEC`**：sysroot 里的 `rcc` 是 **aarch64 二进制**，在 x86_64 宿主机上无法执行。
2. **内核/CMake 回退到 `/bin/sh`**：POSIX 约定 `execve` 遇到 `ENOEXEC` 时用 shell 解释该文件（这是为了让无 shebang 的脚本也能跑）。于是 `sh` 把 **aarch64 ELF 当成 shell 脚本**逐行执行。
3. **ELF 字节被当成重定向目标**：aarch64 机器码在 sh 眼里是一堆随机 token，其中恰好有 `>文件名` 的形状，于是 `O_CREAT` 出一个以 ELF 内部字符串片段命名的文件。

**原始实现**（`qt_dashboard/CMakeLists.txt` 改前）：

```cmake
execute_process(
    COMMAND "${__qt_tool_location}" -v
    RESULT_VARIABLE __qt_tool_rc
    OUTPUT_QUIET ERROR_QUIET)
if(NOT __qt_tool_rc EQUAL 0)
    # 目标架构的二进制跑不了，换成宿主机上的同名工具
    find_program(...)
endif()
```

看起来最严谨——「先探测能不能执行，跑不了才替换」，这样原生构建就不会被误干预。**但探测这个动作本身就是有害的**。

## 修法

```cmake
# 判据用 CMAKE_CROSSCOMPILING（构建期已知事实），不靠运行期试探
if(CMAKE_CROSSCOMPILING)
    foreach(__tool moc rcc uic)
        if(TARGET Qt${QT_VERSION_MAJOR}::${__tool})
            find_program(__qt_host_${__tool}
                NAMES ${__tool}
                PATHS /usr/lib/qt5/bin /usr/lib/qt6/libexec
                      /usr/lib/x86_64-linux-gnu/qt5/bin
                NO_DEFAULT_PATH)
            ...
            set_target_properties(Qt${QT_VERSION_MAJOR}::${__tool}
                PROPERTIES IMPORTED_LOCATION "${__qt_host_${__tool}}")
        endif()
    endforeach()
endif()
```

**交叉编译时，目标架构的 moc/rcc/uic 永远不可能执行**——这不是需要探测的事，是构建配置的已知事实。探测没有提供任何信息，只留下副作用。

验证：修完后 `cmake configure` 与完整 `./tools/ci.sh --local-only` 均不再生成该文件。

## 边界

1. **只在「目标架构 ≠ 宿主机架构」时触发**，且只在那个二进制**没有 shebang**（ELF 都是）时。原生构建探测能成功，不会走到 `sh` 回退。
2. **`OUTPUT_QUIET ERROR_QUIET` 挡不住它**：垃圾文件是 `sh` 子进程的**副作用**，不是被捕获的输出。任何以 `execute_process` 探测异构二进制的地方都有这个风险（例如探测交叉工具链、探测 sysroot 里的 python）。
3. **文件名含非法 UTF-8**，常规 shell 工具链（`rm`、`find`、`git clean`）可能都处理不了，必须用 `os.unlink(bytes_path)`。

## 教训

**「先探测再决定」比「按已知事实直接决定」多一次副作用机会，而探测往往什么信息都没多给。**

在交叉编译这个语境下，`CMAKE_CROSSCOMPILING` 已经是答案；再去问「这个 aarch64 二进制能不能在 x86 上跑」，问的是一件已知为否的事，代价是在工作区里撒一个删不掉的垃圾文件。

配套：`openspec/tasks/2026-11-14-video-frame-egress-and-panel.md`（同日的面板/帧出口任务）。
