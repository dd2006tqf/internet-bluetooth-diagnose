#pragma once

/**
 * @file media_topology.hpp
 * @brief media controller 拓扑的**纯数据结构与算法**（不含任何 ioctl）
 *
 * ============================================================================
 * 为什么要从 v4l2_frame_source.cpp 里切出来
 * ============================================================================
 *
 * 拓扑解析有两半，风险完全不对等：
 *
 *   1. **ioctl 枚举**（`MEDIA_IOC_ENUM_ENTITIES` / `MEDIA_IOC_ENUM_LINKS`）：
 *      只依赖内核 ABI，写法对错一次就能发现——枚举不出来就是空的。
 *   2. **图算法与类型判定**（实体分类、按 major:minor 认领节点、逐级上溯）：
 *      逻辑分支多，错了**不会报错**，只会安静地输出一个看似合理的错误结论。
 *
 * 第 2 类正是本项目已经踩过的坑：把 `st_dev` 当成 `st_rdev` 会让节点匹配
 * 永远失败（describe() 少一段，看不出是 bug）；只按 `type & 0x00ff0000`
 * 分类会把 Sensor 判成未知。这类错误只能靠单测抓。
 *
 * 但原来的实现全部藏在 .cpp 的匿名命名空间里，gtest 一行都碰不到；
 * 而它唯一的入口 `describe()` 又要求 `start()` 成功——本机 vimc 因
 * `VIDIOC_STREAMON: Broken pipe` 根本走不到。于是「有实现、零覆盖」。
 *
 * 切成独立编译单元后，测试可以**直接构造拓扑图**验证算法，
 * 不需要任何设备：这正是纯函数该有的可测性。
 *
 * ============================================================================
 * 边界：这里不做任何 syscall
 * ============================================================================
 *
 * 本文件及对应 .cpp **不打开任何文件、不调用 ioctl**。ioctl 侧只负责把内核
 * 回报的结构体搬进 `MediaTopology`，判断逻辑全在这里。
 *
 * 因此 media 拓扑对上层是**只读诊断信息**：本模块不调用
 * `MEDIA_IOC_SETUP_LINK`，也不会因为解析失败而让采集失败。
 */

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

/// 实体的大类
///
/// 内核的 `media_entity_desc.type` 是**位域**而不是枚举：
///   bits 16..23 = 类别（`MEDIA_ENT_F_OLD_BASE` / `MEDIA_ENT_F_OLD_SUBDEV_BASE` …）
///   bits  0..15 = 子类
///
/// 容易搞反的一点：`MEDIA_ENT_F_IO_V4L` / `MEDIA_ENT_F_CAM_SENSOR` /
/// `MEDIA_ENT_F_LENS` 这些看起来像「新式常量」的名字，**本身就是这个位域
/// 布局的取值**（`MEDIA_ENT_F_IO_V4L == 0x00010001 == OLD_BASE | 0x0001`）。
/// 所以不存在「新旧两套编码」，按类别 + 子类拆位判断即可覆盖全部。
///
/// 实测依据（vimc / vivid）：
///   Sensor A 0x00020001 · Debayer A 0x00020000 · Raw Capture 0 0x00010001
///   Lens A 0x00020003 · vivid-000-vid-cap 0x00010001
enum class MediaEntityKind {
    V4lNode,    ///< 可打开的 /dev/videoN
    Sensor,     ///< 图像传感器
    Lens,       ///< 镜头（VCM）
    Subdev,     ///< 其它 subdev（ISP / Debayer / Scaler / CSI ...）
    Unknown,
};

/// 实体类别 → 稳定的短字符串（进入 describe()，属于可观测输出）
const char* entityKindName(MediaEntityKind k);

/// 把内核 `media_entity_desc.type` 映射成大类
///
/// 判定顺序：先试旧式编码，再试新式常量。反过来会让旧式驱动的
/// `0x00020001`（Sensor）先撞上新式某个常量而误判。
MediaEntityKind classifyEntity(uint32_t type);

/// 一条解析出来的实体
struct MediaEntity {
    uint32_t id{0};
    std::string name;
    MediaEntityKind kind{MediaEntityKind::Unknown};
    uint32_t major{0};   ///< 仅 V4lNode 有意义（设备节点的 major）
    uint32_t minor{0};   ///< 仅 V4lNode 有意义
};

/// 一条链路（只记录 source → sink 与是否启用）
struct MediaLink {
    uint32_t source_id{0};
    uint32_t sink_id{0};
    uint32_t sink_pad{0};
    bool enabled{false};
};

/// 一个 media 设备的拓扑快照
struct MediaTopology {
    std::string device;      ///< /dev/mediaN
    std::string driver;
    std::vector<MediaEntity> entities;
    std::vector<MediaLink> links;

    /// 本帧源打开的那个 video 节点在拓扑里的 id / 名字
    ///
    /// 为什么要存下来而不是事后按「第一个 V4L 节点」猜：一个 media 设备
    /// 常常注册多个 video 节点（实测 vimc 有 3 个：Raw Capture 0/1 与
    /// RGB/YUV Capture）。取第一个必然指错节点，然后把别人的上游链路
    /// 当成自己的——比不输出更糟，因为它看起来像个可信的结论。
    uint32_t node_id{0};
    std::string node_name;
    bool valid{false};
};

/// 按设备号在拓扑里找出对应的 V4L 节点，找到则回填 `node_id` / `node_name`
///
/// **必须传入设备节点自身的 `st_rdev`**（例如 /dev/video3 → 81:3），
/// 不能用 `st_dev`——后者是**该文件所在文件系统**的设备号，实测 /dev/video3
/// 的 `st_dev` 是 0:5，与任何实体都对不上。写错时匹配永远失败，
/// 而且失败得很安静：describe() 只是少一段拓扑。
///
/// 返回是否找到。
bool claimNodeByDeviceNumber(MediaTopology& topo, uint32_t major, uint32_t minor);

/// 从 `topo.node_id` 开始沿 sink 方向逐级上溯，格式化成人话
///
/// 真机上最有用的信息不是完整拓扑，而是**这条链路上游是否全通**：
/// 任何一个 ENABLED 缺失都意味着采集会拿到 0 帧，而 V4L2 层只会报 EINVAL。
///
/// 逐级上溯而不是只看一跳：实际链路上 video 节点上面通常还有 ISP / Scaler /
/// CSI 等多级 subdev，只看直接上游会漏掉中间断链。实测 vimc：
/// `RGB/YUV Capture ← Scaler ← Debayer A ← Sensor A`，其中
/// `Debayer B → Scaler` 与 `RGB/YUV Input → Scaler` 都是 DISABLED，
/// 这两条只有列出全部上游才看得见。
///
/// 输出形态（单行，供 describe() 用）：
///   "RGB/YUV Capture <- Scaler <- Debayer A <- Sensor A [sensor, 3 hops]"
///   "XXX (no upstream link)"            —— 一跳都没走成
///   ""                                  —— 拓扑或节点无效
///
/// 多上游时优先取 ENABLED 的那条（那才是实际生效的通路）；全 DISABLED
/// 时取第一条并把 `(DISABLED)` 标在链路上，用于报告断链位置。
std::string summarizeUpstream(const MediaTopology& topo);

}  // namespace weaknet
