# `install` 不创建父目录：在 `set -e` 脚本里新增一行，会让整个打包静默中断

> 发现于：2026-11-15
> 位置：`tools/ci.sh` 打包段（`mkdir -p dist-arm64/...` 与 `install` 目标路径）
> 影响：新增一个随包文件后，打包在第 N 行失败，后续所有产物与部署全部不执行
>
> 时效性说明（2026-11-16）：下面举例用的 `weaknet-wdt-takeover.sh` 已随看门狗方案
> 改为直接喂 `/dev/watchdog0` 而删除，`ci.sh` 里那一行也没了。**踩坑的教训与文件无关**——
> 「`install` 要求父目录已存在，且 `set -e` 会让它静默中止整段打包」在任何新增
> 随包文件时都成立。

## 现象

给发布包新增一个文件（看门狗接管脚本）时，只加了一行：

```bash
install -m 0755 "${ROOT}/tools/weaknet-wdt-takeover.sh" dist-arm64/tools/weaknet-wdt-takeover.sh
```

而 `dist-arm64/tools/` 从来不存在 —— 上面那行 `mkdir -p` 只列了
`server/bin`、`client/bin`、`lib` 等目录。`install` 不会创建父目录：

```
$ install -m 0755 src.sh out/tools/dst.sh
install: cannot create regular file 'out/tools/dst.sh': No such file or directory
rc=1
```

脚本开头是 `set -euo pipefail`，于是打包**在这一行直接中断**。
它后面的东西一个都不会执行：升级脚本入包、内核模块交叉编译、
`dist-arm64` 文件计数、部署、板端切换。

## 成因

`install` 的语义是「复制并设置权限」，它要求目标**父目录已存在**。
这一点与 `cp` 相同，与 `mkdir -p` 造成的直觉不同。写的人脑子里想的是
「往 dist 里放个文件」，而目录清单在函数上方几十行处，两处必须同步
但没有任何机制强制同步 —— 它们只是先后相邻的两段文本。

危险的放大器是 `set -e`：它让错误**立刻中止**（这是对的），
但如果没有把整段输出接到日志尾部，很容易只看到「编译失败」，
而真正的原因埋在中间某行 `install: cannot create` 里。

## 修法

在 `mkdir -p` 列表里补齐：

```bash
mkdir -p dist-arm64/server/bin dist-arm64/server/build dist-arm64/server/test \
         dist-arm64/client/bin dist-arm64/client/lib dist-arm64/lib dist-arm64/tools
```

同一处还有第二个必须核对的点：**新目录会不会跟着发布包走完整个链路**。
本仓库的链路是 `dist-arm64/` → rsync 到 `.deploy-staging/` → 板端 `adopt`
搬进 `releases/<ver>/`。`adopt` 用 `find . -type f` 全量搬运，所以
`tools/` 会自动跟进；而权限归一化 `chmod u=rwX,go=rX` 里的 `X` 保留
已有执行位，可执行文件不会丢 `+x`。这两点必须逐一确认过，
否则「文件进了包」但「板端不可执行」，问题会推迟到第一次调用时才暴露。

## 边界

- 只在 `install` 目标**带目录前缀**时才会踩到；`install x dist-arm64/`（以 `/` 结尾）
  是把 `dist-arm64` 当目录，同样要求它存在。
- 反过来，若把路径写成 `install -D`（GNU coreutils 支持 `-D` 自动建父目录），
  可以免去维护目录清单。本仓库没用 `-D`，因为在 ARM64 sysroot 交叉环境里
  曾遇到老版 coreutils 不支持的情况，且 `-D` 会掩盖「目录漏配」这类错配。
- 这个坑不能靠「再跑一次就好」绕过：`set -e` 下就是恒失败，每次都停在同一行。

## 教训

> 在一份「先集中建目录、后分散放文件」的脚本里，**新增文件时必须同时确认两处**：
> 父目录在不在建目录的清单里，以及这个新路径能不能走完整条发布链路。
> 只加 `install` 一行的改动，看起来很局部，实际跨了两个必须同步的位置。

顺带的观察：这个错误在代码审查里很容易漏，因为 diff 只有一行 `install`，
看起来完全正常。**它只能靠实际跑一遍打包、或逐行核对目标路径发现** ——
换句话说，`bash -n` 语法检查通过、单元测试全绿、代码编译零告警，
这三件事都不会拦住它。
