# tools/ 目录说明

本目录是**运维与诊断工具**，不属于产品运行时。产品运行时（服务端 / 客户端库 /
eBPF / 内核模块）在 `server/`、`client/`、`kmod/`。

## 板端一次性探针（`board_*.cpp`）

这些程序**不在任何构建目标里**，也不会进 `dist-arm64`。它们要手工交叉编译后
推到开发板运行，用来回答一个具体的、无法在 x86 上回答的问题。

不把它们加进 CMake 是刻意的：它们依赖板端专有库（`libvencoder.so` /
cedarc）、依赖真实摄像头或真实网络，在 x86 上必然编不过或跑不通；加进构建图
只会让每次全量编译为一个用不到的目标付出失败风险。

| 探针 | 回答什么问题 | 记录 |
| ---- | ------------ | ---- |
| `board_encoder_check.cpp` | cedar 硬件编码器在板上能否真正出码流 | `openspec/tasks/` 两条记录 |
| `board_video_clip_e2e.cpp` | 合成 NV12 → cedar → 录制器 → 上传，云端是否接受（验传输契约，不验画质） | `openspec/tasks/2026-11-14-video-clip-e2e-verification.md` |
| `board_capture_pixel_sink_check.cpp` | 「DQBUF 不出帧」与「pixel_sink 没接到」哪一条成立 | 见下 |

### `board_capture_pixel_sink_check.cpp` 的结论

它用来分离两条自相矛盾的板端现象：归因日志稳定输出 `frames_bad=yes`
（说明 `snapshotAndReset()` 返回了有效帧），而 `video frame hub configured`
在整段 journal 里 0 次。

结论已闭环：**帧确实从 V4L2 出得来，只是首次 `open()` 会静默零帧**——开机后
第一次打开的采集必须重开一次才有帧。这条边界已固化在
`openspec/pitfalls/capture-first-open-zero-frames.md` 与
`openspec/pitfalls/camera-zero-frame-open-count-not-sufficient.md`。

探针本身保留在仓库里，因为「零帧」有几种形态（复位后彻底静默最隐蔽），
再次排障时这是最快的分流手段。**它是一次性诊断工具，不承担回归验证职责。**

## 部署与发布

| 脚本 | 用途 |
| ---- | ---- |
| `ci.sh` | 唯一部署入口：编译 → 打包 `dist-arm64` → 部署 → 板端测试 |
| `release.sh` | 官方固件：编译 → 打包 → ECDSA 签名 → `artifacts/` |
| `kmod_cross_build.sh` | 交叉编译 `kmod/*.c` 到 `dist-arm64/kmod/` |
| `weaknet-local-update.sh` | 板端 OTA：adopt / apply / rollback / package |

> **三条打包路径（`ci.sh`、`release.sh`、`.github/workflows/release.yml`）的
> dist 组装必须保持一致。** 同一版本号经不同路径产出的固件内容不同，是发布
> 可信度缺陷——历史上就发生过：发布链路缺 `weaknet_health_guard` 与
> `kmod/*.ko`，而部署链路有。
