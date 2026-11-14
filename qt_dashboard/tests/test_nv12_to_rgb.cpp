/**
 * @file test_nv12_to_rgb.cpp
 * @brief NV12→RGB 转换与缩放的单测
 *
 * ## 这些测试在守什么
 *
 * 转换代码写错**不会报错**，只会画出一张颜色不对的图。而「颜色不对」在现场
 * 有无数种可能原因（传感器、ISP、显示器、曝光），根本没法靠看画面反推是
 * 不是代码问题。所以必须用**已知输入的纯色块**断言精确输出，把「公式错」
 * 与「链路错」彻底分开。
 *
 * 本文件主要守 6 件事：
 *   1. stride != width 时 UV 平面偏移正确（**最关键**，只用 stride==width
 *      的用例发现不了）
 *   2. stride != width 时逐行取样不斜切
 *   3. UV 交错取样：每 2 像素共享一对 UV
 *   4. 边界校验：源不够长 / 尺寸非法时返回 false 而不是越界读
 *   5. 纯灰/纯黑/纯白的定点转换结果在合理范围内
 *   6. 最近邻缩放的取样位置精确
 *
 * ## 关于「合理范围」而不是精确值
 *
 * 定点转换的舍入与标准公式有 ±1~2 的差。断言精确到字节会让测试变成
 * 「实现快照」——改一个系数就得改测试，而看不出对错。所以对**转换数值**
 * 用容差断言，对**结构性问题**（偏移、取样、边界）用精确断言。
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "ui/nv12_to_rgb.hpp"

namespace {

using weaknet::dashboard::nv12ToRgb888;
using weaknet::dashboard::scaleRgb888;

/// 构造一张 NV12 图：Y 与 UV 分别由函数填充，行跨度可独立指定
struct Nv12Image {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
};

Nv12Image makeNv12(uint32_t w, uint32_t h, uint32_t stride, uint8_t y_val, uint8_t u_val,
                   uint8_t v_val) {
    Nv12Image img;
    img.width = w;
    img.height = h;
    img.stride = stride;
    img.data.assign(static_cast<size_t>(stride) * h + static_cast<size_t>(stride) * (h / 2), 0);

    // Y 平面：只填有效宽度，行尾 padding 刻意留 0
    //
    // 为什么 padding 要留成 0 而不是也填 y_val：这样一旦实现把 stride 当成
    // width、或者 UV 偏移算错，就会读到 0（黑），断言立刻能发现。
    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t col = 0; col < w; ++col) {
            img.data[static_cast<size_t>(row) * stride + col] = y_val;
        }
    }
    // UV 平面：同样只填有效宽度
    const size_t uv_off = static_cast<size_t>(stride) * h;
    for (uint32_t row = 0; row < h / 2; ++row) {
        for (uint32_t col = 0; col + 1 < w; col += 2) {
            img.data[uv_off + static_cast<size_t>(row) * stride + col] = u_val;
            img.data[uv_off + static_cast<size_t>(row) * stride + col + 1] = v_val;
        }
    }
    return img;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. UV 平面偏移：本文件最重要的一条
// ---------------------------------------------------------------------------

TEST(Nv12ToRgb, UvPlaneOffsetUsesStrideNotWidth) {
    // ========================================================================
    // 守的是「UV 平面起始偏移必须是 stride × height」
    // ========================================================================
    //
    // 板端实测：1640 宽，bytesperline=2460。若实现按 width 算偏移
    // （1640×height），会**提前**读到 UV 平面之前的 Y 数据，于是整个画面
    // 下半部分整块偏色。
    //
    // 构造：上半部分 Y 是某值、UV 是「中性」(128,128) 应得灰；
    //       下半部分 Y 相同，但 UV 平面**真正的**位置在 stride×h。
    // 若实现偏移算错，下半部分的 UV 会从 Y 区读到 0，颜色立刻偏离。
    constexpr uint32_t kW = 16;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kStride = 24;   // != kW，这是关键

    // Y=128（中性），UV=(128,128) 中性 → 应接近灰 (约 124~130)
    Nv12Image img = makeNv12(kW, kH, kStride, 128, 128, 128);

    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 0);
    ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride, 0, rgb.data(),
                             rgb.size()));

    // 逐像素检查：整张图都应是中性灰（容差 ±6）
    //
    // 若 UV 偏移用了 width 而不是 stride，UV 平面会被读错位置，下半部分
    // （row >= h/2 的行取 UV 时）颜色会明显偏离中性。
    for (uint32_t row = 0; row < kH; ++row) {
        for (uint32_t col = 0; col < kW; ++col) {
            const size_t idx = (static_cast<size_t>(row) * kW + col) * 3;
            EXPECT_NEAR(rgb[idx + 0], 126, 8) << "row=" << row << " col=" << col << " (R)";
            EXPECT_NEAR(rgb[idx + 1], 126, 8) << "row=" << row << " col=" << col << " (G)";
            EXPECT_NEAR(rgb[idx + 2], 126, 8) << "row=" << row << " col=" << col << " (B)";
        }
    }
}

TEST(Nv12ToRgb, StrideGreaterThanWidthDoesNotSkewRows) {
    // ========================================================================
    // 守「逐行取样必须按 stride 跳行，不能按 width」
    // ========================================================================
    //
    // 构造一张按行渐变的图：第 r 行的 Y = r * (255/height)。若实现按 width
    // 跳行而不是 stride，每行都会读到上一行的部分数据，输出会呈现「斜切」
    // 或行错位。
    //
    // 断言方式：每一行的输出应当**整行同值**（因为本行 Y 是常数），且行与
    // 行之间单调递增。这同时验证了「没有跨行串数据」。
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kStride = 16;   // 大幅宽于 width，斜切会非常明显

    Nv12Image img;
    img.width = kW;
    img.height = kH;
    img.stride = kStride;
    img.data.assign(static_cast<size_t>(kStride) * kH + static_cast<size_t>(kStride) * (kH / 2), 0);

    for (uint32_t row = 0; row < kH; ++row) {
        const uint8_t y = static_cast<uint8_t>(16 + row * 12);   // 逐行变亮
        for (uint32_t col = 0; col < kW; ++col) {
            img.data[static_cast<size_t>(row) * kStride + col] = y;
        }
    }
    const size_t uv_off = static_cast<size_t>(kStride) * kH;
    for (uint32_t row = 0; row < kH / 2; ++row) {
        for (uint32_t col = 0; col + 1 < kW; col += 2) {
            img.data[uv_off + static_cast<size_t>(row) * kStride + col] = 128;
            img.data[uv_off + static_cast<size_t>(row) * kStride + col + 1] = 128;
        }
    }

    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 0);
    ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride, 0, rgb.data(),
                             rgb.size()));

    for (uint32_t row = 0; row < kH; ++row) {
        const size_t base = static_cast<size_t>(row) * kW * 3;
        const int r0 = rgb[base + 0];
        // 整行必须同值：若按 width 跳行，行内会混入相邻行的数据
        for (uint32_t col = 1; col < kW; ++col) {
            EXPECT_EQ(rgb[base + col * 3 + 0], r0)
                << "row=" << row << " col=" << col << "：行内出现不同值，说明跨行串数据";
        }
        if (row > 0) {
            const size_t prev = static_cast<size_t>(row - 1) * kW * 3;
            EXPECT_GT(rgb[base + 0], rgb[prev + 0])
                << "row=" << row << "：亮度未随行递增，行序错乱";
        }
    }
}

// ---------------------------------------------------------------------------
// 2. UV 交错取样
// ---------------------------------------------------------------------------

TEST(Nv12ToRgb, UvIsSharedPerTwoHorizontalPixels) {
    // NV12 的 UV 是交错存放（U0 V0 U1 V1…），每个 UV 对覆盖 2 个水平像素。
    // 构造：左半 UV=(U低,V高) → 偏蓝；右半 UV=(U高,V低) → 偏红。
    // 若实现按 col 而不是 col/2 取 UV，左右两侧的颜色边界会错位一像素。
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 4;
    constexpr uint32_t kStride = 8;

    Nv12Image img;
    img.width = kW;
    img.height = kH;
    img.stride = kStride;
    img.data.assign(static_cast<size_t>(kStride) * kH + static_cast<size_t>(kStride) * (kH / 2), 0);

    // Y 全部中性偏亮
    for (uint32_t row = 0; row < kH; ++row) {
        for (uint32_t col = 0; col < kW; ++col) {
            img.data[static_cast<size_t>(row) * kStride + col] = 180;
        }
    }
    // 前 4 个像素的 UV 对 → 偏蓝；后 4 个 → 偏红
    const size_t uv_off = static_cast<size_t>(kStride) * kH;
    for (uint32_t row = 0; row < kH / 2; ++row) {
        const uint8_t* pair_blue = nullptr;
        (void)pair_blue;
        for (uint32_t col = 0; col + 1 < kW; col += 2) {
            const bool left = (col < kW / 2);
            img.data[uv_off + static_cast<size_t>(row) * kStride + col] =
                left ? 240 : 16;    // U
            img.data[uv_off + static_cast<size_t>(row) * kStride + col + 1] =
                left ? 16 : 240;    // V
        }
    }

    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 0);
    ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride, 0, rgb.data(),
                             rgb.size()));

    const size_t left_px = 0;                  // 最左像素
    const size_t right_px = (kW - 1) * 3;      // 最右像素（同一行）
    EXPECT_GT(rgb[left_px + 2], rgb[left_px + 0]) << "左侧应偏蓝（B > R）";
    EXPECT_GT(rgb[right_px + 0], rgb[right_px + 2]) << "右侧应偏红（R > B）";
}

// ---------------------------------------------------------------------------
// 3. 边界校验：不够长/参数非法都必须返回 false，不能越界读
// ---------------------------------------------------------------------------

TEST(Nv12ToRgb, RejectsSourceShorterThanDeclared) {
    // ========================================================================
    // 守的是「中枢截断时不得越界读」
    // ========================================================================
    // `VideoFrameHub::publish()` 在 src_size < 槽位大小时会**截断**拷贝
    // 并计入 `truncated`。此时传给本函数的就是一段不够长的数据。
    // 这里必须返回 false，而不是按 width/height 一路读下去。
    constexpr uint32_t kW = 16;
    constexpr uint32_t kH = 16;
    constexpr uint32_t kStride = 16;

    Nv12Image img = makeNv12(kW, kH, kStride, 128, 128, 128);
    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 0);

    // 完整长度应当成功
    ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride, 0, rgb.data(),
                             rgb.size()));

    // 少 1 字节：UV 平面最后一行不全
    EXPECT_FALSE(nv12ToRgb888(img.data.data(), img.data.size() - 1, kW, kH, kStride, 0, rgb.data(),
                              rgb.size()));
    // 只给 Y 平面：UV 完全缺失
    const size_t y_only = static_cast<size_t>(kStride) * kH;
    EXPECT_FALSE(nv12ToRgb888(img.data.data(), y_only, kW, kH, kStride, 0, rgb.data(), rgb.size()));
}

TEST(Nv12ToRgb, RejectsInvalidArguments) {
    std::vector<uint8_t> buf(64 * 64 * 3, 0);
    std::vector<uint8_t> src(64 * 64 * 3 / 2, 128);

    EXPECT_FALSE(nv12ToRgb888(nullptr, src.size(), 64, 64, 64, 0, buf.data(), buf.size()));
    EXPECT_FALSE(nv12ToRgb888(src.data(), src.size(), 64, 64, 64, 0, nullptr, buf.size()));

    // 零尺寸
    EXPECT_FALSE(nv12ToRgb888(src.data(), src.size(), 0, 64, 64, 0, buf.data(), buf.size()));
    EXPECT_FALSE(nv12ToRgb888(src.data(), src.size(), 64, 0, 64, 0, buf.data(), buf.size()));

    // 奇数宽高：NV12 的 UV 按 2×2 取样，奇数没有对应的 UV
    EXPECT_FALSE(nv12ToRgb888(src.data(), src.size(), 63, 64, 64, 0, buf.data(), buf.size()));
    EXPECT_FALSE(nv12ToRgb888(src.data(), src.size(), 64, 63, 64, 0, buf.data(), buf.size()));

    // stride < width：行跨度比宽度还小，数据必然损坏
    EXPECT_FALSE(nv12ToRgb888(src.data(), src.size(), 64, 64, 32, 0, buf.data(), buf.size()));

    // 输出缓冲不够
    std::vector<uint8_t> small(10, 0);
    EXPECT_FALSE(nv12ToRgb888(src.data(), src.size(), 64, 64, 64, 0, small.data(), small.size()));
}

TEST(Nv12ToRgb, ZeroStrideFallsBackToWidth) {
    // stride==0 是异常输入，按 width 兜底。这条不是为了「宽容」，
    // 而是为了让调用方漏填 stride 时得到**可预期的**结果而不是拒绝服务。
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 4;
    Nv12Image img = makeNv12(kW, kH, kW, 128, 128, 128);

    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 0);
    EXPECT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, 0, 0, rgb.data(), rgb.size()));
}

// ---------------------------------------------------------------------------
// 3.5 UV 平面偏移是**事实值**，不能由 stride*height 推导
// ---------------------------------------------------------------------------

TEST(Nv12ToRgb, UvOffsetWithAlignmentHoleIsHonored) {
    // ========================================================================
    // 这一条钉的是本板 sunxi-vin 的真实布局（2026-11-14 实测）
    // ========================================================================
    //
    // 实测原始帧 3,045,504 字节的布局：
    //
    //   [0,        2,020,480)  Y  plane  stride=1640 × 1232，紧凑
    //   [2,020,480, 2,030,336) 对齐空洞 9856 字节，**实测全零**
    //   [2,030,336, 3,040,576) UV plane  stride=1640 × 616，紧凑
    //   [3,040,576, 3,045,504) 尾部空洞 4928 字节，实测全零
    //
    // 关键：UV 起始 = align16(1640)*align16(1232) = 2030336，
    // 而 stride*height = 1640*1232 = 2020480，差 9856 字节。
    //
    // 这段差来自驱动 `vin_video.c:216,512` 的
    //   pix_size = ALIGN(o_width,16) * ALIGN(o_height,16)
    // 而 payload[0] = pix_size*12/8；NV12 的 payload = Y+UV = 1.5Y，
    // 所以 Y 平面字节数 = payload*2/3 = pix_size = UV 起始偏移。
    //
    // 若实现硬用 stride*height 定位 UV，会读到那块**全零**空洞，即 U=V=0
    // —— 转换出来是饱和度极高的偏色，且不报任何错。这条用例把「空洞全零」
    // 与「UV 中性」的差异变成可断言的判据。
    constexpr uint32_t kW = 16;      // 真实是 1640，这里等比缩小
    constexpr uint32_t kH = 16;      // 真实是 1232
    constexpr uint32_t kStride = kW; // 真实 stride == width
    constexpr uint32_t kHole = 64;   // 模拟那 9856 字节的对齐空洞

    const size_t y_bytes = static_cast<size_t>(kStride) * kH;
    const size_t uv_off = y_bytes + kHole;
    const size_t uv_bytes = static_cast<size_t>(kStride) * (kH / 2);

    std::vector<uint8_t> frame(uv_off + uv_bytes, 0);

    // Y 平面：中性灰亮度
    for (uint32_t row = 0; row < kH; ++row) {
        std::fill_n(frame.begin() + static_cast<size_t>(row) * kStride, kW,
                    static_cast<uint8_t>(128));
    }
    // 空洞刻意留 0（真实帧实测就是全零）——这正是错误实现的"诱饵"：
    // 读到它得到 U=V=0，画面严重偏色，与中性灰差异极大。
    // UV 平面：中性 (128,128) → 应转出灰色
    for (uint32_t row = 0; row < kH / 2; ++row) {
        for (uint32_t col = 0; col + 1 < kW; col += 2) {
            frame[uv_off + static_cast<size_t>(row) * kStride + col] = 128;
            frame[uv_off + static_cast<size_t>(row) * kStride + col + 1] = 128;
        }
    }

    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 0);

    // 先确认前提：源长度确实比 stride*height*1.5 长（这正是真机情形）
    ASSERT_GT(frame.size(), y_bytes + uv_bytes);
    ASSERT_GT(uv_off, y_bytes) << "本用例要求 UV 偏移严格大于 stride*height";

    ASSERT_TRUE(nv12ToRgb888(frame.data(), frame.size(), kW, kH, kStride,
                             static_cast<uint32_t>(uv_off), rgb.data(), rgb.size()));

    // 整张图都应是中性灰。若实现忽略 uv_offset（按 stride*height 取 UV），
    // 它会读到空洞里的 0 → U=V=0 → 明显偏色，这里立刻失败。
    for (uint32_t row = 0; row < kH; ++row) {
        for (uint32_t col = 0; col < kW; ++col) {
            const size_t p = (static_cast<size_t>(row) * kW + col) * 3;
            EXPECT_NEAR(rgb[p + 0], rgb[p + 1], 6)
                << "r/g 偏离说明 UV 读到了空洞 (row=" << row << " col=" << col << ")";
            EXPECT_NEAR(rgb[p + 1], rgb[p + 2], 6)
                << "g/b 偏离说明 UV 读到了空洞 (row=" << row << " col=" << col << ")";
        }
    }
}

TEST(Nv12ToRgb, ZeroUvOffsetMeansStrideTimesHeight) {
    // uv_offset == 0 是「标准紧凑 NV12」的约定值，必须等价于 stride*height。
    // 这条保证不关心布局的调用方（以及所有既有单测）行为不变。
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kStride = 16;
    Nv12Image img = makeNv12(kW, kH, kStride, 200, 128, 128);

    std::vector<uint8_t> rgb_a(static_cast<size_t>(kW) * kH * 3, 0);
    std::vector<uint8_t> rgb_b(static_cast<size_t>(kW) * kH * 3, 0);

    ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride, 0,
                             rgb_a.data(), rgb_a.size()));
    ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride,
                             kStride * kH, rgb_b.data(), rgb_b.size()));
    EXPECT_EQ(rgb_a, rgb_b) << "uv_offset=0 必须等价于 stride*height";
}

TEST(Nv12ToRgb, UvOffsetInsideYPlaneIsRejected) {
    // uv_offset 落在 Y 平面内部一定是调用方把参数接反了
    // （最典型：把 stride 当成 uv_offset 传进来）。这种输入必须拒绝，
    // 否则会拿亮度当色度，画面全错且无从定位。
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kStride = 8;
    Nv12Image img = makeNv12(kW, kH, kStride, 128, 128, 128);
    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 0);

    EXPECT_FALSE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride,
                              kStride, rgb.data(), rgb.size()))
        << "uv_offset 落在 Y 平面内应被拒绝";

    // 恰好等于 Y 平面末尾是合法的
    EXPECT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride,
                             kStride * kH, rgb.data(), rgb.size()));
}

TEST(Nv12ToRgb, UvPlaneMustFitEvenWhenTailHoleMissing) {
    // 尾部空洞不属于任何平面：缺了不影响画图。
    // 这条守的是「不要把 src_size 校验写成 sizeimage 全等」——
    // 真机的 sizeimage 含尾部对齐空洞，若要求全等就会永远失败。
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kStride = 8;
    Nv12Image img = makeNv12(kW, kH, kStride, 128, 128, 128);

    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 0);
    // 正好到 UV 平面末尾（无尾部空洞）
    EXPECT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), kW, kH, kStride, 0,
                             rgb.data(), rgb.size()));
    // UV 少一行就不行
    const size_t without_last_uv_row =
        static_cast<size_t>(kStride) * kH + static_cast<size_t>(kStride) * (kH / 2 - 1);
    EXPECT_FALSE(nv12ToRgb888(img.data.data(), without_last_uv_row, kW, kH, kStride, 0,
                              rgb.data(), rgb.size()));
}

// ---------------------------------------------------------------------------
// 4. 转换数值的合理性
// ---------------------------------------------------------------------------

TEST(Nv12ToRgb, BlackAndWhiteMapToExpectedRange) {
    // Y=16（limited-range 黑）中性 UV → 应接近 (0,0,0)
    {
        Nv12Image img = makeNv12(8, 4, 8, 16, 128, 128);
        std::vector<uint8_t> rgb(8 * 4 * 3, 0);
        ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), 8, 4, 8, 0, rgb.data(), rgb.size()));
        EXPECT_LE(rgb[0], 3) << "limited-range Y=16 应接近全黑";
        EXPECT_LE(rgb[1], 3);
        EXPECT_LE(rgb[2], 3);
    }
    // Y=235（limited-range 白）中性 UV → 应接近 (255,255,255)
    {
        Nv12Image img = makeNv12(8, 4, 8, 235, 128, 128);
        std::vector<uint8_t> rgb(8 * 4 * 3, 0);
        ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), 8, 4, 8, 0, rgb.data(), rgb.size()));
        EXPECT_GE(rgb[0], 250) << "limited-range Y=235 应接近全白";
        EXPECT_GE(rgb[1], 250);
        EXPECT_GE(rgb[2], 250);
    }
}

TEST(Nv12ToRgb, NeutralUvProducesGrayNotTinted) {
    // 中性 UV=(128,128) 且 Y 为常数时，R/G/B 三通道必须相等（灰）。
    // 若系数写反（例如把 U/V 对调），灰度图会明显偏色——这是最典型的
    // 「公式写错但看起来像链路问题」的形态。
    Nv12Image img = makeNv12(16, 8, 16, 128, 128, 128);
    std::vector<uint8_t> rgb(16 * 8 * 3, 0);
    ASSERT_TRUE(nv12ToRgb888(img.data.data(), img.data.size(), 16, 8, 16, 0, rgb.data(), rgb.size()));

    for (size_t i = 0; i < rgb.size(); i += 3) {
        EXPECT_EQ(rgb[i + 0], rgb[i + 1]) << "中性 UV 时 R 与 G 应相等 @pixel " << (i / 3);
        EXPECT_EQ(rgb[i + 1], rgb[i + 2]) << "中性 UV 时 G 与 B 应相等 @pixel " << (i / 3);
    }
}

// ---------------------------------------------------------------------------
// 5. 缩放
// ---------------------------------------------------------------------------

TEST(Nv12ToRgb, ScaleByHalfPicksEveryOtherPixel) {
    // 最近邻 2:1 缩小的取样位置是确定的：目标 (x,y) ← 源 (2x, 2y)。
    // 用可辨认的图案断言，防止有人把取样公式改成「取整偏移」而悄悄移像素。
    constexpr uint32_t kSrcW = 8;
    constexpr uint32_t kSrcH = 8;
    constexpr uint32_t kDstW = 4;
    constexpr uint32_t kDstH = 4;

    std::vector<uint8_t> src(static_cast<size_t>(kSrcW) * kSrcH * 3, 0);
    for (uint32_t y = 0; y < kSrcH; ++y) {
        for (uint32_t x = 0; x < kSrcW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kSrcW + x) * 3;
            src[i + 0] = static_cast<uint8_t>(x * 16);
            src[i + 1] = static_cast<uint8_t>(y * 16);
            src[i + 2] = 64;
        }
    }

    std::vector<uint8_t> dst(static_cast<size_t>(kDstW) * kDstH * 3, 0);
    ASSERT_TRUE(scaleRgb888(src.data(), kSrcW, kSrcH, dst.data(), kDstW, kDstH));

    for (uint32_t y = 0; y < kDstH; ++y) {
        for (uint32_t x = 0; x < kDstW; ++x) {
            const size_t di = (static_cast<size_t>(y) * kDstW + x) * 3;
            const uint32_t sx = x * 2;
            const uint32_t sy = y * 2;
            EXPECT_EQ(dst[di + 0], static_cast<uint8_t>(sx * 16)) << "x=" << x << " y=" << y;
            EXPECT_EQ(dst[di + 1], static_cast<uint8_t>(sy * 16)) << "x=" << x << " y=" << y;
            EXPECT_EQ(dst[di + 2], 64);
        }
    }
}

TEST(Nv12ToRgb, ScaleRejectsNullAndZero) {
    std::vector<uint8_t> buf(64, 0);
    EXPECT_FALSE(scaleRgb888(nullptr, 4, 4, buf.data(), 2, 2));
    EXPECT_FALSE(scaleRgb888(buf.data(), 4, 4, nullptr, 2, 2));
    EXPECT_FALSE(scaleRgb888(buf.data(), 0, 4, buf.data(), 2, 2));
    EXPECT_FALSE(scaleRgb888(buf.data(), 4, 4, buf.data(), 0, 2));
}

TEST(Nv12ToRgb, ScaleSameSizeIsIdentity) {
    constexpr uint32_t kW = 4;
    constexpr uint32_t kH = 3;
    std::vector<uint8_t> src(static_cast<size_t>(kW) * kH * 3);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i & 0xFF);

    std::vector<uint8_t> dst(src.size(), 0);
    ASSERT_TRUE(scaleRgb888(src.data(), kW, kH, dst.data(), kW, kH));
    EXPECT_EQ(src, dst) << "等尺寸缩放必须是恒等变换";
}
