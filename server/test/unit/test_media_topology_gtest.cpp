/**
 * @file test_media_topology_gtest.cpp
 * @brief media controller 拓扑纯算法的契约测试（**无设备依赖**）
 *
 * ============================================================================
 * 这份测试存在的理由
 * ============================================================================
 *
 * 拓扑解析原本整个藏在 v4l2_frame_source.cpp 的匿名命名空间里，唯一入口
 * `describe()` 又要求 `start()` 成功。结果是：实机验证只能靠「打开一个真
 * 摄像头然后肉眼看日志」，而逻辑错了**不会报错**——只会安静地输出一个
 * 看起来合理的错误结论。
 *
 * 实际已经踩过的三个坑（都被本文件里的用例锁住）：
 *   1. 节点匹配误用 `st_dev`（文件系统设备号）而非 `st_rdev`（设备节点
 *      自身），匹配**永远失败**且无任何症状。
 *   2. 只按 `type & 0x00ff0000` 分类，把 Sensor / Debayer / Scaler 全判成
 *      同一类（实测 vimc 会把 Sensor 判成未知）。
 *   3. 按「第一个 V4L 节点」认领上游，而 vimc 注册了 3 个 video 节点，
 *      于是 reports 了别人家的链路。
 *
 * 本文件用**合成拓扑**复现这三类形状，不需要内核 media 设备、不需要 root、
 * 不需要摄像头。CI 上必须全绿。
 */

#include <gtest/gtest.h>

#include <linux/media.h>

#include <set>
#include <string>

#include "media_topology.hpp"

using namespace weaknet;

namespace {

/// 构造一个实体
MediaEntity ent(uint32_t id, const std::string& name, MediaEntityKind kind,
                uint32_t major = 0, uint32_t minor = 0) {
    MediaEntity e;
    e.id = id;
    e.name = name;
    e.kind = kind;
    e.major = major;
    e.minor = minor;
    return e;
}

/// 构造一条链路
MediaLink link(uint32_t src, uint32_t sink, bool enabled, uint32_t sink_pad = 0) {
    MediaLink l;
    l.source_id = src;
    l.sink_id = sink;
    l.sink_pad = sink_pad;
    l.enabled = enabled;
    return l;
}

/// 复刻实测的 vimc 拓扑形状：
///
///   Sensor A(1) --ENABLED--> Debayer A(3) --ENABLED--> Scaler(5) --ENABLED--> RGB/YUV Capture(7)
///   Sensor B(2) --ENABLED--> Debayer B(4) --DISABLED--> Scaler(5)      （第二条上游，断开）
///   Raw Capture 0(8)  挂在 Debayer A 的另一个 pad 上
///   Raw Capture 1(9)  挂在 Debayer B 上
///
/// 节点 7 才是本帧源打开的节点；节点 8/9 是**别人的**节点。
/// 这正是「按第一个 V4L 节点猜」会指错的那种形状。
MediaTopology makeVimcLike() {
    MediaTopology t;
    t.device = "/dev/media1";
    t.driver = "vimc";
    t.entities = {
        ent(1, "Sensor A", MediaEntityKind::Sensor),
        ent(2, "Sensor B", MediaEntityKind::Sensor),
        ent(3, "Debayer A", MediaEntityKind::Subdev),
        ent(4, "Debayer B", MediaEntityKind::Subdev),
        ent(5, "Scaler", MediaEntityKind::Subdev),
        ent(7, "RGB/YUV Capture", MediaEntityKind::V4lNode, 81, 3),
        ent(8, "Raw Capture 0", MediaEntityKind::V4lNode, 81, 0),
        ent(9, "Raw Capture 1", MediaEntityKind::V4lNode, 81, 1),
    };
    t.links = {
        link(1, 3, true),    // Sensor A → Debayer A
        link(2, 4, true),    // Sensor B → Debayer B
        // **顺序刻意为难「取第一条」的实现**：把 DISABLED 的那条排在前面。
        // 实测 vimc 的真实链路顺序不保证 ENABLED 在前，实现必须主动挑
        // ENABLED 的。若这里把 ENABLED 排前面，"不优先选" 的变异就抓不到。
        link(4, 5, false),   // Debayer B → Scaler（DISABLED，排在前）
        link(3, 5, true),    // Debayer A → Scaler（ENABLED，排在后）
        link(3, 8, true),    // Debayer A → Raw Capture 0
        link(4, 9, true),    // Debayer B → Raw Capture 1
        link(5, 7, true),    // Scaler → RGB/YUV Capture
    };
    t.valid = true;
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 实体类型分类
//
// **这一段曾经写错过，值得留个记录**：我一开始以为内核有「旧式位域」与
// 「新式常量」两套互不兼容的编码，于是写了两轮判定。实测发现
// `MEDIA_ENT_F_IO_V4L` 等常量的值**本身就等于旧式位域的组合**
// （0x00010001 = OLD_BASE|0x0001），第二轮判定永远走不到。
// 现在只做一次位域拆解，用例覆盖真实驱动上报的每一个值。
// ---------------------------------------------------------------------------

TEST(MediaTopology, ClassifiesRealValuesObservedFromDrivers) {
    // 这些值全部来自实测：`/dev/media1`（vimc）与 `/dev/media0`（vivid）
    // 的 MEDIA_IOC_ENUM_ENTITIES 原始输出。用真值而非常量名，是为了让
    // 「内核头文件换了取值」这种变化立刻暴露。
    struct Case {
        uint32_t type;
        MediaEntityKind want;
        const char* what;
    };
    const Case cases[] = {
        {0x00010001u, MediaEntityKind::V4lNode, "Raw Capture 0 / vivid-000-vid-cap"},
        {0x00020001u, MediaEntityKind::Sensor,  "Sensor A / Sensor B / RGB-YUV Input"},
        {0x00020000u, MediaEntityKind::Subdev,  "Debayer A / Debayer B / Scaler"},
        {0x00020003u, MediaEntityKind::Lens,    "Lens A / Lens B"},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(classifyEntity(c.type), c.want)
            << "type=0x" << std::hex << c.type << " (" << c.what << ")";
    }
}

TEST(MediaTopology, MediaEntityConstantsShareTheLegacyBitLayout) {
    // 锁住「不存在两套编码」这个事实。若哪天有人又加回一段「新式常量」
    // 判定，这条断言会提醒他那是死代码——因为这些常量本来就在位域里。
    EXPECT_EQ(static_cast<uint32_t>(MEDIA_ENT_F_IO_V4L),
              (static_cast<uint32_t>(MEDIA_ENT_F_OLD_BASE) & 0x00ff0000u) | 0x0001u);
    EXPECT_EQ(static_cast<uint32_t>(MEDIA_ENT_F_CAM_SENSOR),
              (static_cast<uint32_t>(MEDIA_ENT_F_OLD_SUBDEV_BASE) & 0x00ff0000u) | 0x0001u);
    EXPECT_EQ(static_cast<uint32_t>(MEDIA_ENT_F_LENS),
              (static_cast<uint32_t>(MEDIA_ENT_F_OLD_SUBDEV_BASE) & 0x00ff0000u) | 0x0003u);
    EXPECT_EQ(static_cast<uint32_t>(MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN),
              (static_cast<uint32_t>(MEDIA_ENT_F_OLD_SUBDEV_BASE) & 0x00ff0000u) | 0x0000u);

    // 用常量名调用也必须得到同样的分类
    EXPECT_EQ(classifyEntity(MEDIA_ENT_F_IO_V4L), MediaEntityKind::V4lNode);
    EXPECT_EQ(classifyEntity(MEDIA_ENT_F_CAM_SENSOR), MediaEntityKind::Sensor);
    EXPECT_EQ(classifyEntity(MEDIA_ENT_F_LENS), MediaEntityKind::Lens);
    EXPECT_EQ(classifyEntity(MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN),
              MediaEntityKind::Subdev);
}

TEST(MediaTopology, SubdevWithoutKnownSubtypeIsSubdevNotSensor) {
    // 这是最关键的一条分类语义：Debayer / Scaler / CSI 这类 subdev 的子类
    // 是 0x0000。若把它们一律当成 Sensor（或 Unknown），上游汇总会写出
    // 一个错误的顶端设备，让人去查一颗根本不存在的摄像头。
    EXPECT_EQ(classifyEntity(0x00020000u), MediaEntityKind::Subdev);
    // 反过来：并不是所有 0x0002 类别都该叫 sensor
    EXPECT_NE(classifyEntity(0x00020000u), MediaEntityKind::Sensor);
}

TEST(MediaTopology, UnknownTypeIsUnknownNotGuessed) {
    // 猜错比说不知道更糟：一个凭空的 "sensor" 会让人去查一颗不存在的摄像头
    EXPECT_EQ(classifyEntity(0xdeadbeefu), MediaEntityKind::Unknown);
    EXPECT_EQ(classifyEntity(0u), MediaEntityKind::Unknown);
    // 节点类里的非 V4L 子类（如 processor 0xffff）不该冒充 V4L 节点
    EXPECT_EQ(classifyEntity(0x0001ffffu), MediaEntityKind::Unknown);
}

TEST(MediaTopology, KindNamesAreStableStrings) {
    // 这些字符串会进 describe()，属于可观测输出，不能被悄悄改名
    EXPECT_STREQ(entityKindName(MediaEntityKind::V4lNode), "node");
    EXPECT_STREQ(entityKindName(MediaEntityKind::Sensor), "sensor");
    EXPECT_STREQ(entityKindName(MediaEntityKind::Lens), "lens");
    EXPECT_STREQ(entityKindName(MediaEntityKind::Subdev), "subdev");
    EXPECT_STREQ(entityKindName(MediaEntityKind::Unknown), "unknown");
}

// ---------------------------------------------------------------------------
// 2. 节点认领：必须按设备号匹配，不能猜第一个
// ---------------------------------------------------------------------------

TEST(MediaTopology, ClaimsNodeByDeviceNumberNotByOrder) {
    // 这是实测抓到的第一个真 bug：原实现按「第一个 V4L 节点」认领，
    // 而 vimc 有 3 个，于是把 Raw Capture 0 的上游当成了 RGB/YUV Capture 的。
    MediaTopology t = makeVimcLike();

    // /dev/video3 → 81:3，应是 RGB/YUV Capture，不是第一个 V4L 节点。
    ASSERT_TRUE(claimNodeByDeviceNumber(t, 81, 3));
    EXPECT_EQ(t.node_id, 7u);
    EXPECT_EQ(t.node_name, "RGB/YUV Capture");
}

TEST(MediaTopology, ClaimsEachDistinctNodeCorrectly) {
    // 三个节点各自认领到自己，证明匹配不是「碰巧命中第一个」
    for (uint32_t minor : {0u, 1u, 3u}) {
        MediaTopology t = makeVimcLike();
        ASSERT_TRUE(claimNodeByDeviceNumber(t, 81, minor))
            << "minor=" << minor << " 的节点应存在于拓扑中";
        EXPECT_EQ(t.node_name,
                  minor == 0 ? "Raw Capture 0"
                             : (minor == 1 ? "Raw Capture 1" : "RGB/YUV Capture"));
    }
}

TEST(MediaTopology, ClaimFailsWhenDeviceNumberAbsent) {
    MediaTopology t = makeVimcLike();
    // 用一个不存在的设备号：必须如实返回 false，而不是退回第一个节点。
    // 退回第一个是这里最危险的行为——它会让人以为认领成功了。
    EXPECT_FALSE(claimNodeByDeviceNumber(t, 81, 99));
    EXPECT_EQ(t.node_id, 0u) << "认领失败时不得留下半成品 state";
    EXPECT_TRUE(t.node_name.empty());
}

TEST(MediaTopology, ClaimIgnoresNonV4lEntitiesWithMatchingNumber) {
    // 设备号只对 V4L 节点有意义。若某个 subdev 恰好 major/minor 相同，
    // 也不该抢走节点的身份。
    MediaTopology t;
    t.entities = {
        ent(1, "weird subdev", MediaEntityKind::Subdev, 81, 3),
        ent(2, "real node", MediaEntityKind::V4lNode, 81, 3),
    };
    ASSERT_TRUE(claimNodeByDeviceNumber(t, 81, 3));
    EXPECT_EQ(t.node_id, 2u);
}

// ---------------------------------------------------------------------------
// 3. 上游上溯：多跳、优先 ENABLED、报告 DISABLED
// ---------------------------------------------------------------------------

TEST(MediaTopology, SummarizeWalksMultiHopUpstream) {
    MediaTopology t = makeVimcLike();
    ASSERT_TRUE(claimNodeByDeviceNumber(t, 81, 3));

    const std::string s = summarizeUpstream(t);

    // 链路必须是完整的三跳：Capture ← Scaler ← Debayer A ← Sensor A
    EXPECT_NE(s.find("RGB/YUV Capture"), std::string::npos) << s;
    EXPECT_NE(s.find("Scaler"), std::string::npos) << s;
    EXPECT_NE(s.find("Debayer A"), std::string::npos)
        << "只看直接上游会漏掉中间层，断链就看不见了：" << s;
    EXPECT_NE(s.find("Sensor A"), std::string::npos) << s;
    EXPECT_NE(s.find("3 hops"), std::string::npos) << "应如实报告跳数：" << s;
    EXPECT_NE(s.find("sensor"), std::string::npos)
        << "最上游类型要写出来，否则读者不知道链路顶端是什么：" << s;
}

TEST(MediaTopology, SummarizePrefersEnabledBranchWhenMultipleUpstreams) {
    // Scaler 有两条上游：Debayer A(ENABLED) 与 Debayer B(DISABLED)。
    // 必须走 ENABLED 那条——它是实际生效的通路。走错会 report 一条
    // 根本不承载数据的链路。
    MediaTopology t = makeVimcLike();
    ASSERT_TRUE(claimNodeByDeviceNumber(t, 81, 3));

    const std::string s = summarizeUpstream(t);

    EXPECT_NE(s.find("Debayer A"), std::string::npos) << s;
    EXPECT_NE(s.find("Sensor A"), std::string::npos) << s;
    EXPECT_EQ(s.find("Debayer B"), std::string::npos)
        << "不得选中 DISABLED 分支作为主链路：" << s;
}

TEST(MediaTopology, SummarizeMarksDisabledUpstream) {
    // 当唯一/选中的上游是 DISABLED 时，必须显式标出来。
    // 这正是真机上「节点打开了但拿不到帧」的直接解释，而 V4L2 只会报 EINVAL。
    MediaTopology t;
    t.valid = true;
    t.entities = {
        ent(1, "Sensor", MediaEntityKind::Sensor),
        ent(2, "Capture", MediaEntityKind::V4lNode, 81, 0),
    };
    t.links = {link(1, 2, false)};
    t.node_id = 2;
    t.node_name = "Capture";

    const std::string s = summarizeUpstream(t);

    EXPECT_NE(s.find("DISABLED"), std::string::npos)
        << "断链必须可见，否则现场只能看到 EINVAL 而不知道哪一跳断了：" << s;
    EXPECT_NE(s.find("DISABLED upstream"), std::string::npos) << s;
}

TEST(MediaTopology, SummarizeReportsNoUpstreamLinkForRootNode) {
    MediaTopology t;
    t.valid = true;
    t.entities = {ent(1, "Standalone", MediaEntityKind::V4lNode, 81, 0)};
    t.links.clear();
    t.node_id = 1;
    t.node_name = "Standalone";

    EXPECT_EQ(summarizeUpstream(t), "Standalone (no upstream link)")
        << "无上游是正常形态（vivid 就是这样），必须给出明确措辞而不是空串";
}

TEST(MediaTopology, SummarizeReturnsEmptyWhenTopologyInvalid) {
    MediaTopology t;
    t.valid = false;
    t.entities = {ent(1, "X", MediaEntityKind::V4lNode, 81, 0)};
    t.node_id = 1;
    t.node_name = "X";

    EXPECT_TRUE(summarizeUpstream(t).empty())
        << "拓扑无效时应整段不输出——写成 \"topology unavailable\" 会让读者"
           "以为出了问题，实际只是这个驱动没有 media controller";
}

TEST(MediaTopology, SummarizeReturnsEmptyWhenNodeNotClaimed) {
    MediaTopology t = makeVimcLike();
    // 没有 claim 过节点（node_id 默认 0）
    EXPECT_TRUE(summarizeUpstream(t).empty());
}

TEST(MediaTopology, SummarizeDoesNotLoopOnCyclicTopology) {
    // 坏驱动可能回报环形链路。describe() 是热路径，死循环会挂住整个服务。
    //
    // **只断言「能返回」是无效护栏**：把 kMaxHops 从 16 改成 100000 后
    // 本用例仍然通过（实测确认），因为 100000 次也终究会返回，只是慢。
    // 所以必须断言**跳数上界**——这正是防环措施的实际语义。
    MediaTopology t;
    t.valid = true;
    t.entities = {
        ent(1, "A", MediaEntityKind::Subdev),
        ent(2, "B", MediaEntityKind::Subdev),
        ent(3, "Capture", MediaEntityKind::V4lNode, 81, 0),
    };
    // 1 → 2 → 1 成环，同时 2 → 3 通向节点
    t.links = {link(2, 1, true), link(1, 2, true), link(2, 3, true)};
    t.node_id = 3;
    t.node_name = "Capture";

    const std::string s = summarizeUpstream(t);

    EXPECT_FALSE(s.empty()) << "成环时也必须能返回一个结果，不能挂死";

    // 实现里的上限是 kMaxHops = 16，因此报告的真实跳数不可能超过 16。
    // 这里用 20 作宽松上界：既不会因上限微调而误报，又能抓住「上限被
    // 调大到失去防护意义」的变异（100000 会明显超标）。
    const size_t hop_pos = s.find(" hop");
    ASSERT_NE(hop_pos, std::string::npos) << "成环输出应带跳数统计：" << s;
    const size_t lb = s.rfind('[', hop_pos);
    ASSERT_NE(lb, std::string::npos) << s;
    const size_t comma = s.find(',', lb);
    ASSERT_NE(comma, std::string::npos) << s;
    const int reported = std::stoi(s.substr(comma + 1, hop_pos - comma - 1));
    EXPECT_LE(reported, 20) << "成环时跳数必须有界（防环措施失效会无限上溯）："
                            << s << " reported=" << reported;
    // 顺便约束输出长度：无界上溯会让字符串无限膨胀，这比耗时更早暴露问题。
    EXPECT_LT(s.size(), 512u) << "成环输出不得无限增长";
}

TEST(MediaTopology, SummarizeHandlesMissingSourceEntity) {
    // 链路指向一个不在实体表里的 id（枚举被截断时可能发生）。
    // 必须优雅停下，而不是解引用空指针。
    MediaTopology t;
    t.valid = true;
    t.entities = {ent(1, "Capture", MediaEntityKind::V4lNode, 81, 0)};
    t.links = {link(99, 1, true)};   // 99 不存在
    t.node_id = 1;
    t.node_name = "Capture";

    EXPECT_EQ(summarizeUpstream(t), "Capture (no upstream link)");
}

TEST(MediaTopology, SummarizeUsesSingularHopWordForOneHop) {
    MediaTopology t;
    t.valid = true;
    t.entities = {
        ent(1, "Sensor", MediaEntityKind::Sensor),
        ent(2, "Capture", MediaEntityKind::V4lNode, 81, 0),
    };
    t.links = {link(1, 2, true)};
    t.node_id = 2;
    t.node_name = "Capture";

    const std::string s = summarizeUpstream(t);
    EXPECT_NE(s.find("1 hop]"), std::string::npos)
        << "单跳应写 hop 而不是 hops：" << s;
    EXPECT_EQ(s.find("1 hops"), std::string::npos) << s;
}

TEST(MediaTopology, SummarizeArrowPointsInDataFlowDirection) {
    // 数据从上游流向下游。写成 `上游 -> 下游` 会让读者把链路方向读反，
    // 而这里的全部用途就是判断方向相关的断链问题。
    MediaTopology t;
    t.valid = true;
    t.entities = {
        ent(1, "Sensor", MediaEntityKind::Sensor),
        ent(2, "Capture", MediaEntityKind::V4lNode, 81, 0),
    };
    t.links = {link(1, 2, true)};
    t.node_id = 2;
    t.node_name = "Capture";

    const std::string s = summarizeUpstream(t);
    // 「下游 <- 上游」的顺序：Capture 在前，Sensor 在后
    const size_t pos_cap = s.find("Capture");
    const size_t pos_sen = s.find("Sensor");
    ASSERT_NE(pos_cap, std::string::npos) << s;
    ASSERT_NE(pos_sen, std::string::npos) << s;
    EXPECT_LT(pos_cap, pos_sen) << "下游应写在左侧：" << s;
    EXPECT_NE(s.find(" <- "), std::string::npos)
        << "箭头应指向数据流方向（下游 <- 上游）：" << s;
}
