/**
 * @file nv12_to_rgb.cpp
 * @brief NV12 → RGB888 转换实现
 */

#include "nv12_to_rgb.hpp"

#include <cstring>

namespace weaknet {
namespace dashboard {

namespace {

/// 把 BT.601 的 YUV 三元组转成 RGB，写入三个字节
///
/// 用整数定点而不是浮点：本板 A55 没有硬件除法优势，但整数乘法+
/// 移位在 1640×1232=2M 像素上仍有可观差别；更重要的是**结果确定**，
/// 单测可以逐字节断言，不必担心浮点在不同编译器/优化级别下的末位差异。
///
/// 系数取自 BT.601 limited-range（视频常用的 16-235 / 16-240 范围）：
///   R = 1.164(Y-16) + 1.596(V-128)
///   G = 1.164(Y-16) - 0.391(U-128) - 0.813(V-128)
///   B = 1.164(Y-16) + 2.018(U-128)
///
/// 为什么是 limited 而不是 full range：V4L2 报的 `quantization` 本板是
/// `Default (maps to Limited Range)`（`v4l2-ctl --get-fmt-video` 实测）。
/// 用 full-range 系数会让画面整体偏亮、对比度偏高。
inline uint8_t clampRound(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

inline void yuvToRgb(uint8_t y, uint8_t u, uint8_t v,
                     uint8_t& r, uint8_t& g, uint8_t& b) {
    const int c = static_cast<int>(y) - 16;
    const int d = static_cast<int>(u) - 128;
    const int e = static_cast<int>(v) - 128;

    // 定点化：系数 ×256 后右移 8 位。末尾补 128 做四舍五入。
    const int yy = 298 * c;                       // 1.164 * 256
    r = clampRound((yy + 409 * e + 128) >> 8);    // 1.596 * 256
    g = clampRound((yy - 100 * d - 208 * e + 128) >> 8);  // 0.391/0.813
    b = clampRound((yy + 516 * d + 128) >> 8);    // 2.018 * 256
}

}  // namespace

bool nv12ToRgb888(const void* nv12, size_t src_size,
                  uint32_t width, uint32_t height, uint32_t stride,
                  uint32_t uv_offset,
                  uint8_t* out, size_t out_size) {
    // ---- 参数校验：全部返回 false，绝不越界读 ----
    //
    // 这些检查不是形式主义。中枢在「源比槽短」时会截断（`truncated` 计数），
    // 那种情况下 src_size 就是不够的；而面板块的尺寸来自对端，完全可能
    // 与预期不符。宁可少画一帧，也不能读飞内存。
    if (nv12 == nullptr || out == nullptr) return false;
    if (width == 0 || height == 0) return false;
    // NV12 的 UV 平面按 2×2 取样：宽高都必须是偶数
    if ((width & 1u) != 0 || (height & 1u) != 0) return false;

    const size_t needed_out = static_cast<size_t>(width) * height * 3;
    if (out_size < needed_out) return false;

    // stride 若为 0（异常输入）按 width 兜底，但**不**静默接受 width 之外的
    // 猜测：board 实测 stride > width 是常态，绝不能反过来由 width 推导。
    if (stride == 0) stride = width;
    if (stride < width) return false;   // 行跨度比宽度还小：数据必然损坏

    // UV 平面偏移。0 是「按紧凑布局」的约定值，此时等于 stride*height。
    //
    // **不能**在这里加「否则必须 > stride*height」之类的校验：反过来才常见
    // （紧凑布局下两者相等）。这里只拦一种情况：偏移落在 Y 平面内部，
    // 那一定是调用方把参数接反了（例如把 stride 传给了 uv_offset）。
    const size_t y_bytes = static_cast<size_t>(stride) * height;
    const size_t uv_off = (uv_offset == 0) ? y_bytes : uv_offset;
    if (uv_off < y_bytes) return false;

    // 源长度校验：UV 平面最后一行必须完整落在 src_size 内。
    //
    // 注意这里用 `uv_off` 而不是 `y_bytes`，且**不**要求 src_size >=
    // uv_off + uv_bytes 之外的东西——尾部对齐空洞（本板 4928 字节）不属于
    // 任何平面，缺了也不影响画图。但 UV 平面本身少一行就真的画不出来。
    const size_t uv_bytes = static_cast<size_t>(stride) * (height / 2);
    if (src_size < uv_off + uv_bytes) return false;

    const auto* y_plane = static_cast<const uint8_t*>(nv12);
    const uint8_t* uv_plane = y_plane + uv_off;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* y_row = y_plane + static_cast<size_t>(row) * stride;
        // UV 行：每两个 Y 行共用一行 UV
        const uint8_t* uv_row = uv_plane + static_cast<size_t>(row / 2) * stride;
        uint8_t* out_row = out + static_cast<size_t>(row) * width * 3;

        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t yy = y_row[col];
            // UV 交错：每个 UV 对覆盖 2 个水平像素
            const size_t uv_idx = static_cast<size_t>(col / 2) * 2;
            const uint8_t uu = uv_row[uv_idx];
            const uint8_t vv = uv_row[uv_idx + 1];

            uint8_t r = 0, g = 0, b = 0;
            yuvToRgb(yy, uu, vv, r, g, b);
            out_row[col * 3 + 0] = r;
            out_row[col * 3 + 1] = g;
            out_row[col * 3 + 2] = b;
        }
    }
    return true;
}

bool scaleRgb888(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                 uint8_t* dst, uint32_t dst_w, uint32_t dst_h) {
    if (src == nullptr || dst == nullptr) return false;
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return false;

    // 最近邻：目标像素 (x,y) 取源像素 (x*src_w/dst_w, y*src_h/dst_h)。
    // 用整数乘除而不是浮点比例，取样位置确定，便于单测。
    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint32_t sy = static_cast<uint32_t>(
            static_cast<uint64_t>(y) * src_h / dst_h);
        const uint8_t* src_row = src + static_cast<size_t>(sy) * src_w * 3;
        uint8_t* dst_row = dst + static_cast<size_t>(y) * dst_w * 3;
        for (uint32_t x = 0; x < dst_w; ++x) {
            const uint32_t sx = static_cast<uint32_t>(
                static_cast<uint64_t>(x) * src_w / dst_w);
            const uint8_t* s = src_row + static_cast<size_t>(sx) * 3;
            uint8_t* d = dst_row + static_cast<size_t>(x) * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    return true;
}

}  // namespace dashboard
}  // namespace weaknet
