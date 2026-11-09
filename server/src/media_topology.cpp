/**
 * @file media_topology.cpp
 * @brief media controller 拓扑的纯算法实现（无 syscall）
 *
 * 与 `media_topology.hpp` 的分工：本文件只做判定与格式化，不打开任何
 * 设备。ioctl 枚举在 `v4l2_frame_source.cpp` 里，负责把内核结构体搬进
 * 这里的 `MediaTopology`。
 */

#include "media_topology.hpp"

#include <linux/media.h>

#include <sstream>

namespace weaknet {

const char* entityKindName(MediaEntityKind k) {
    switch (k) {
        case MediaEntityKind::V4lNode: return "node";
        case MediaEntityKind::Sensor:  return "sensor";
        case MediaEntityKind::Lens:    return "lens";
        case MediaEntityKind::Subdev:  return "subdev";
        case MediaEntityKind::Unknown: return "unknown";
    }
    return "unknown";
}

MediaEntityKind classifyEntity(uint32_t type) {
    // `media_entity_desc.type` 的布局是 **类别位域 + 子类位域**，不是枚举：
    //   bits 16..23 = 类别（MEDIA_ENT_F_OLD_BASE / OLD_SUBDEV_BASE / ...）
    //   bits  0..15 = 子类
    //
    // 关键事实（实测厘清，容易搞反）：`MEDIA_ENT_F_IO_V4L` /
    // `MEDIA_ENT_F_CAM_SENSOR` / `MEDIA_ENT_F_LENS` 这些看起来像「新式
    // 常量」的名字，**本身就是这个位域布局的取值**：
    //   MEDIA_ENT_F_IO_V4L              = 0x00010001 = OLD_BASE    | 0x0001
    //   MEDIA_ENT_F_CAM_SENSOR          = 0x00020001 = OLD_SUBDEV  | 0x0001
    //   MEDIA_ENT_F_LENS                = 0x00020003 = OLD_SUBDEV  | 0x0003
    //   MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN = 0x00020000 = OLD_SUBDEV  | 0x0000
    //
    // 所以**不存在「两套互不兼容的编码」**：按类别 + 子类拆位判断，天然
    // 覆盖全部这些常量。实测 vimc 与 vivid 上报的类型值逐条等于上述常量：
    //   Sensor A 0x00020001 / Debayer A 0x00020000 / Raw Capture 0 0x00010001
    //   / Lens A 0x00020003 / vivid-000-vid-cap 0x00010001
    //
    // 曾经的实现额外写了一段「再试新式编码」的等值比较，那是**永远走不到
    // 的死代码**（上面的位域判断已先行命中），会让人误以为存在另一套编码。
    const uint32_t cat = type & 0x00ff0000u;
    const uint32_t sub = type & 0x0000ffffu;

    if (cat == (MEDIA_ENT_F_OLD_BASE & 0x00ff0000u)) {
        // 节点类：子类 0x0001 是 V4L 节点，0xffff 是处理器实体
        if (sub == 0x0001u) return MediaEntityKind::V4lNode;
        return MediaEntityKind::Unknown;
    }
    if (cat == (MEDIA_ENT_F_OLD_SUBDEV_BASE & 0x00ff0000u)) {
        // subdev 类：只有 sensor 与 lens 有独立子类，其余（Debayer / Scaler
        // / CSI / ISP …）都归为通用 subdev。不能一律当成 Sensor：这会让人
        // 去查一颗根本不存在的摄像头。
        if (sub == 0x0001u) return MediaEntityKind::Sensor;
        if (sub == 0x0003u) return MediaEntityKind::Lens;
        return MediaEntityKind::Subdev;
    }

    return MediaEntityKind::Unknown;
}

bool claimNodeByDeviceNumber(MediaTopology& topo, uint32_t major, uint32_t minor) {
    for (const auto& e : topo.entities) {
        if (e.kind != MediaEntityKind::V4lNode) continue;
        if (e.major == major && e.minor == minor) {
            topo.node_id = e.id;
            topo.node_name = e.name;
            return true;
        }
    }
    return false;
}

std::string summarizeUpstream(const MediaTopology& topo) {
    if (!topo.valid || topo.entities.empty() || topo.node_id == 0) {
        return std::string();
    }

    // 建立 id → 实体指针，避免每跳都线性查找
    const auto findEntity = [&topo](uint32_t id) -> const MediaEntity* {
        for (const auto& e : topo.entities) {
            if (e.id == id) return &e;
        }
        return nullptr;
    };

    // 从本节点开始逐级上溯。深度上限防环（驱动理论上不该有环，但坏链路
    // 描述会让这里死循环，而 describe() 是被频繁调用的热路径）。
    std::ostringstream os;
    os << topo.node_name;

    uint32_t cursor = topo.node_id;
    std::string chain_kind;
    bool any_disabled = false;
    int hops = 0;
    constexpr int kMaxHops = 16;

    while (hops++ < kMaxHops) {
        // 找一条连到 cursor 的链路。多个上游同时存在时优先取 ENABLED 的，
        // 因为那才是实际生效的通路；全 DISABLED 时取第一条用于报告断链。
        const MediaLink* chosen = nullptr;
        for (const auto& l : topo.links) {
            if (l.sink_id != cursor) continue;
            if (chosen == nullptr) {
                chosen = &l;
            } else if (l.enabled && !chosen->enabled) {
                chosen = &l;
            }
        }
        if (chosen == nullptr) break;

        const MediaEntity* src = findEntity(chosen->source_id);
        if (src == nullptr) break;

        // **箭头必须指向数据流方向**：数据从上游 subdev 流向本 video 节点，
        // 所以写 `下游 <- 上游`。原先写成 `上下游 -> 下游` 会让读者
        // 把链路方向读反——在「哪一跳断了」这种判断题上读反代价很高。
        std::string seg = src->name;
        if (!chosen->enabled) {
            seg += "(DISABLED)";
            any_disabled = true;
        }
        os.str(os.str() + " <- " + seg);

        // 记录最上游的实体类型，让读者知道这是 sensor 还是别的
        chain_kind = entityKindName(src->kind);
        cursor = src->id;
    }

    if (hops <= 1) {
        // 一跳都没走成：可能拓扑里没记录链路，或节点本身是 root
        return topo.node_name + " (no upstream link)";
    }

    // 只报「有几个上游节点」与「有没有断链」，不铺开完整拓扑：
    // describe() 是单行日志，铺开会让真正的重点被淹没。
    std::ostringstream tail;
    tail << " [" << chain_kind << ", " << (hops - 1) << " hop";
    if (hops - 1 != 1) tail << "s";
    if (any_disabled) tail << ", DISABLED upstream";
    tail << "]";
    return os.str() + tail.str();
}

}  // namespace weaknet
