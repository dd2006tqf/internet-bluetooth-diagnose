/**
 * @file video_encoder.cpp
 * @brief IVideoEncoder 的两个实现：cedar VE 硬件后端 + 桩后端
 *
 * ============================================================================
 * 为什么用 dlopen 而不是链接 libvencoder.so
 * ============================================================================
 *
 * 三个理由，从硬到软：
 *
 * 1. **符号插桩顺序 bug（决定性）**。`H264EncOpenVer2` 这个符号在
 *    `libvenc_codec.so` 和 `libvenc_h264.so` 里**各有一份不同实现**。ELF 全局
 *    符号解析「谁先加载谁生效」，于是链接顺序直接决定用哪份：
 *
 *      demoVencoder: libvenc_codec 先于 libvenc_h264  → 用 codec 那份 → ✅
 *      显式链接 7 个库: libvenc_h264 先               → 用 h264 那份 → ❌
 *
 *    失败表现是 `H264EncOpenVer2: error: the driver do not support the
 *    ic 21320`，`VideoEncCreate` 返回 NULL。而正确的那份本来就会被
 *    `libvencoder.so` 自己 NEEDED 拉进来（它的 DT_NEEDED 里
 *    `libvenc_codec.so` 排在 `libvenc_h264.so` 之前）。所以**只要不显式
 *    链接任何 `libvenc_*.so`，顺序自然对**——但这是个一碰就碎的隐式约定，
 *    写成「绝不显式链接」的注释约束太脆弱。用 dlopen 精确控制加载哪个 so、
 *    按什么顺序，把这个约定变成代码。
 *
 *    （实测记录见 openspec/pitfalls/vencoder-symbol-interposition-trap.md）
 *
 * 2. **sysroot 里没有这些库**。它们不在 Bullseye 上游，是板厂随镜像发的
 *    私有包。直接链接会让 x86 构建（本机开发/CI）在链接期就失败，整条
 *    单元测试线跟着废掉——而这条线正是用来验证计数、几何、失败语义的。
 *    dlopen 让「能不能编译」与「运行时有没有硬件」彻底解耦。
 *
 * 3. **ABI 边界显式化**。dlopen 逼我把用到的每个符号、每个结构体布局写在
 *    一处，板厂换库版本时 diff 就是一份可用清单，而不是散落各处的
 *    include。
 *
 * ============================================================================
 * 零拷贝与对齐（这段是实测账，改之前先读 video_encoder.hpp 头注释）
 * ============================================================================
 *
 * 真实帧 UV 偏移 2030336 恰好等于编码器要求的 Y 平面尺寸 1648×1232。
 * 于是 dma-buf 整块导入、Y 起点 0、UV 起点 2030336，字节级对齐。
 *
 * 但 `nShareBufFd` 导入需要编码器先分配缓冲（`AllocInputBuffer`），
 * 而分配尺寸**只能**由我们告诉它。所以零拷贝的前提是：
 *
 *     config.uv_offset == align16(width) * height
 *
 * 不成立时必须退回 memcpy 路径（并把原因写进 `describe()`），否则
 * UV 起点会和编码器的期望错开，产出偏色画面且**不报任何错**。
 */

#include "video_encoder.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "logger.hpp"

namespace weaknet {
namespace {

/// 16 对齐（编码器的 coded 宽度规则；实测 1640 → 1648）
inline uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

/// 单调时钟毫秒
inline int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

/// 帧身份在「输入 → 码流」之间的传递队列。
///
/// ============================================================================
/// 为什么独立成类，而不是内联在某个后端的成员里
/// ============================================================================
///
/// 硬件编码是**异步**的：`encode()` 返回的那块码流对应的并不是这一次送进去
/// 的帧。而下游（`VideoClipRecorder` 的窗口判定、端到端延迟、QoE 归因）必须
/// 知道「这块码流是哪一次采集编出来的」。
///
/// 2026-11-14 板端真实故障：cedar 后端只填了 `data/size/keyframe`，
/// `timestamp_ns` 一直是默认值 0。录制器把 0 当作「最新帧时刻」，
/// `newest_mono_ms_` 恒为 0，于是「后沿未到」永远成立——录制器既不产出
/// 也不放弃，**静默卡死且不报任何错**（日志里只有一条「上传完成」都不会有）。
///
/// 抽成类是为了让这条契约**可被单测直接打中**：如果回填逻辑只存在于某个
/// 后端的分支里，那么「桩后端自带回填」会让测试假绿——2026-11-14 就发生了
/// 这件事（把入队行注释掉，测试仍然全绿）。现在两个后端共用这一份实现，
/// 针对它的断言才是针对生产代码的断言。
///
/// 守卫上限：编码器内部 pipeline 最多几帧，队列永远不会长到这个量级。
/// 设上限只是为了「万一 drain 完全不推进」时不会无界增长。
class InFlightMetaQueue {
public:
    struct Meta {
        uint64_t sequence{0};
        uint64_t timestamp_ns{0};
    };

    /// 一帧成功送进编码器（**必须**在确认入队成功之后调用）
    void push(uint64_t sequence, uint64_t timestamp_ns) {
        q_.push_back(Meta{sequence, timestamp_ns});
        while (q_.size() > kMaxDepth) q_.pop_front();   // 防御性，正常不会触发
    }

    /// 一块码流被取出：取走队首身份。
    ///
    /// **无论调用方是否需要这份元数据都必须调用**——「腾空间」路径会把码流
    /// 直接丢弃，但那份身份同样已被消耗。只在需要元数据时才出队会让队列
    /// 永久错位一格，症状是「时间轴轻微漂移」，比彻底失效更难发现。
    bool pop(Meta* out) {
        if (q_.empty()) return false;
        if (out) *out = q_.front();
        q_.pop_front();
        return true;
    }

    /// 编码器重建时必须清空：旧身份再也对不上任何码流。
    void clear() { q_.clear(); }

    size_t size() const { return q_.size(); }

private:
    static constexpr size_t kMaxDepth = 64;
    std::deque<Meta> q_;
};

// ===========================================================================
// Allwinner cedar VE 的 ABI 声明
//
// 与 third_party/allwinner-venc/include/ 里的头文件保持逐字一致。
// 这里**重复**声明而不是 include：那头文件会捎带 ScMemOpsS/VeOpsS 等
// 一整片与编码无关的结构，且它们依赖板厂特定的指针布局；手写这一份
// 能让「我们到底依赖了哪些字段」一目了然，也让 x86 构建不碰那些头。
// ===========================================================================

extern "C" {

/// 编码器不透明句柄
struct VencHandle;

/**
 * @brief 内存适配器操作表（板厂私有）
 *
 * 只声明**前三个**成员，顺序与 `sc_interface.h` 的 `ScMemOpsS` 逐字一致：
 * `open` / `open2` / `close`。其余二十来个成员（palloc / flush_cache /
 * get_phy_by_fd …）我们一个都不调用，声明出来只会引入一堆板厂特定类型。
 *
 * 为什么必须声明而不是像别处一样当不透明指针：`CdcMemOpen` 和
 * `CdcMemClose` 在 `sc_interface.h` 里是 **`static inline`**，它们不导出
 * 任何符号，`dlsym` 永远找不到。它们只是 `memops->open()` /
 * `memops->close()` 的语法糖。要复用库的这套初始化，唯一办法就是自己
 * 按 ABI 布局去调函数指针。
 *
 * 这条曾经真踩过：早期实现用 `dlsym("CdcMemOpen")` 取符号，本机 dlopen
 * 成功但这两个符号 missing，于是后端被判定不可用、静默回落到桩后端，
 * 板端"跑通了"却一帧真码流都没编出来。
 */
struct ScMemOpsS {
    int (*open)(void);
    int (*open2)(void*, void*);
    void (*close)(void);
};

/// `sc_interface.h` 里 `CdcMemOpen(memops)` 的等价调用
inline int CdcMemOpen(struct ScMemOpsS* memops) {
    return memops->open();
}
/// `sc_interface.h` 里 `CdcMemClose(memops)` 的等价调用
inline void CdcMemClose(struct ScMemOpsS* memops) {
    memops->close();
}

/// VENC_CODEC_TYPE
enum { kCodecH264 = 0, kCodecJpeg = 1, kCodecH264Ver2 = 2, kCodecH265 = 3 };

/// VENC_PIXEL_FMT：只用 NV12
enum { kPixelYuv420Sp = 0 };

/// VENC_INDEXTYPE 里用到的几项（值来自 vencoder.h 的枚举，**不是**猜的偏移）
///
/// 注意 H264 段是从 0x100 开始**连续递增**的，不能凭「大概在这个范围」写：
///   0x100 VENC_IndexParamH264Param
///   0x101 VENC_IndexParamH264SPSPPS
///   0x102 VENC_IndexParamH264QPRange     ← 不是 ProfileLevel
///   0x103 VENC_IndexParamH264ProfileLevel
/// 早期版本把 0x102 标成 ProfileLevel，虽然当时没调用，但注释留着就是误导。
enum : unsigned {
    kIdxBitrate = 0x0,            ///< VENC_IndexParamBitrate, unsigned
    kIdxFramerate = 0x1,          ///< VENC_IndexParamFramerate, unsigned
    kIdxMaxKeyInterval = 0x2,     ///< VENC_IndexParamMaxKeyInterval, unsigned
    kIdxSetVbvSize = 0x0f,        ///< VENC_IndexParamSetVbvSize, unsigned
    kIdxH264Param = 0x100,        ///< VENC_IndexParamH264Param
    kIdxH264SpsPps = 0x101,       ///< VENC_IndexParamH264SPSPPS, VencHeaderData (只读)
    kIdxH264ProfileLevel = 0x103, ///< VENC_IndexParamH264ProfileLevel
};

/// VencRect（crop 信息，未使用时全零）
struct VencRect {
    int nLeft;
    int nTop;
    int nWidth;
    int nHeight;
};

/// VencROIConfig（未使用时全零；这里是 8 个一组，必须按真实尺寸留位）
struct VencROIConfig {
    int bEnable;
    int nROILevel;
    int nAbsQp;
    int nQp;
    VencRect sRect;
};

/// VencInputBuffer：实测布局，字段顺序即内存布局，改动会静默错位
struct VencInputBuffer {
    unsigned long nID;
    long long nPts;
    unsigned int nFlag;
    unsigned char* pAddrPhyY;
    unsigned char* pAddrPhyC;
    unsigned char* pAddrVirY;
    unsigned char* pAddrVirC;
    int nWidth;
    int nHeight;
    int nAlign;
    int bEnableCorp;
    VencRect sCropInfo;
    int ispPicVar;
    int ispPicVarChroma;
    int bUseInputBufferRoi;
    VencROIConfig roi_param[8];
    int bAllocMemSelf;
    int nShareBufFd;
    unsigned char bUseCsiColorFormat;
    int eCsiColorFormat;  ///< VENC_PIXEL_FMT
    int envLV;
};

/// FrameInfo（VencOutputBuffer 里的内嵌结构，未使用但必须占位）
struct FrameInfo {
    unsigned int nWidth;
    unsigned int nHeight;
    unsigned int nAspectRatio;
    unsigned int nScanFormat;
    unsigned int nFrameRate;
    unsigned int nColorPrimaries;
    unsigned int nTransferCharacteristics;
    unsigned int nMatrixCoefficients;
};

/// VencOutputBuffer：实测布局
struct VencOutputBuffer {
    int nID;
    long long nPts;
    unsigned int nFlag;
    unsigned int nSize0;
    unsigned int nSize1;
    unsigned char* pData0;
    unsigned char* pData1;
    FrameInfo frame_info;
    unsigned int nSize2;
    unsigned char* pData2;
};

/// VencBaseConfig：实测布局
struct VencBaseConfig {
    unsigned char bEncH264Nalu;
    unsigned int nInputWidth;
    unsigned int nInputHeight;
    unsigned int nDstWidth;
    unsigned int nDstHeight;
    unsigned int nStride;
    int eInputFormat;  ///< VENC_PIXEL_FMT
    ScMemOpsS* memops;
    void* veOpsS;
    void* pVeOpsSelf;
    unsigned char bOnlyWbFlag;
    unsigned char bLbcLossyComEnFlag2x;
    unsigned char bLbcLossyComEnFlag2_5x;
    unsigned char bIsVbvNoCache;
    unsigned int bVcuAutoMode;
    unsigned int nFrameNumInGroup;
    unsigned int extend_flag;
    unsigned int en_vcu;
};

/// VencAllocateBufferParam：实测布局
struct VencAllocateBufferParam {
    unsigned int nBufferNum;
    unsigned int nSizeY;
    unsigned int nSizeC;
};

/// H264 profile/level（未设置时用库默认）
struct VencH264ProfileLevel {
    int nProfile;
    int nLevel;
};

/// 码流头（SPS/PPS）。`VideoEncGetParameter(kIdxH264SpsPps)` 的返回类型。
///
/// `pBuffer` 指向库内部持有的 Annex-B 小缓冲（实测 24 字节 = SPS 16 + PPS 8），
/// **调用方不得释放**，且下次 GetParameter 之前一直有效。
struct VencHeaderData {
    unsigned char* pBuffer;
    unsigned int nLength;
};

}  // extern "C"

/// 从 libvencoder.so 解析出来的函数指针表
///
/// 全部为 nullptr 表示「硬件不可用」，而不是崩溃——与帧源的能力缺失语义一致。
struct VencApi {
    struct ScMemOpsS* (*MemAdapterGetOpsS)();

    struct VencHandle* (*VideoEncCreate)(int);
    void (*VideoEncDestroy)(struct VencHandle*);
    int (*VideoEncInit)(struct VencHandle*, VencBaseConfig*);
    int (*VideoEncUnInit)(struct VencHandle*);
    int (*AllocInputBuffer)(struct VencHandle*, VencAllocateBufferParam*);
    int (*ReleaseAllocInputBuffer)(struct VencHandle*);
    int (*GetOneAllocInputBuffer)(struct VencHandle*, VencInputBuffer*);
    int (*ReturnOneAllocInputBuffer)(struct VencHandle*, VencInputBuffer*);
    int (*FlushCacheAllocInputBuffer)(struct VencHandle*, VencInputBuffer*);
    int (*AddOneInputBuffer)(struct VencHandle*, VencInputBuffer*);
    int (*VideoEncodeOneFrame)(struct VencHandle*);
    int (*GetOneBitstreamFrame)(struct VencHandle*, VencOutputBuffer*);
    int (*FreeOneBitStreamFrame)(struct VencHandle*, VencOutputBuffer*);
    int (*VideoEncSetParameter)(struct VencHandle*, unsigned, void*);

    /// 读参数。目前只用于取 SPS/PPS（见 configure 里「码流头」那段）。
    int (*VideoEncGetParameter)(struct VencHandle*, unsigned, void*);

    /// 把「硬件已用完」的输入描述符从用毕队列搬回空闲队列。
    ///
    /// **这是必经一步，不是可选优化。** 见 encode() 里「描述符回收」那段注释：
    /// 不调它，H.264 路径恰好编 4 帧后 `AddOneInputBuffer` 永久返回 -1。
    int (*AlreadyUsedInputBuffer)(struct VencHandle*, VencInputBuffer*);

    bool ok() const {
        return MemAdapterGetOpsS && VideoEncCreate && VideoEncDestroy &&
               VideoEncInit && VideoEncUnInit && AllocInputBuffer &&
               ReleaseAllocInputBuffer && GetOneAllocInputBuffer &&
               ReturnOneAllocInputBuffer && FlushCacheAllocInputBuffer &&
               AddOneInputBuffer && VideoEncodeOneFrame &&
               GetOneBitstreamFrame && FreeOneBitStreamFrame &&
               VideoEncSetParameter && VideoEncGetParameter &&
               AlreadyUsedInputBuffer;
    }

    /// 缺少的符号名列表（`ok()` 为 false 时用于 describe）
    std::string missing() const {
#define WN_CHK(f) if (!f) out += #f " "
        std::string out;
        WN_CHK(MemAdapterGetOpsS);
        WN_CHK(VideoEncCreate); WN_CHK(VideoEncDestroy); WN_CHK(VideoEncInit);
        WN_CHK(VideoEncUnInit); WN_CHK(AllocInputBuffer);
        WN_CHK(ReleaseAllocInputBuffer); WN_CHK(GetOneAllocInputBuffer);
        WN_CHK(ReturnOneAllocInputBuffer); WN_CHK(FlushCacheAllocInputBuffer);
        WN_CHK(AddOneInputBuffer); WN_CHK(VideoEncodeOneFrame);
        WN_CHK(GetOneBitstreamFrame); WN_CHK(FreeOneBitStreamFrame);
        WN_CHK(VideoEncSetParameter); WN_CHK(AlreadyUsedInputBuffer);
        WN_CHK(VideoEncGetParameter);
#undef WN_CHK
        return out;
    }
};

/**
 * @brief 进程级 cedarc 库句柄（**只加载一次，永不卸载**）
 *
 * 为什么必须单例且不 unload：
 *
 *   - `VideoEncCreate` 内部会打开 `/dev/cedar_dev_ve2` 并做 chip 版本判定。
 *     这个判定依赖全局状态，一旦某个实例失败过，再 destory + create 也
 *     不会重新走一遍干净的初始化（实测：失败后重试仍失败）。
 *   - dlclose 会把 VE 上下文连同已映射的寄存器页一起撤掉，而其他编码器
 *     实例可能还持有它。
 *
 * 所以这里的策略是：首次使用时加载并缓存，无论成功失败都保持住。
 * 失败结果也被缓存——`/dev/cedar_dev_ve2` 不存在是**环境事实**，
 * 反复重试只是把同一个错误重算一遍。
 */
class VencLibrary {
public:
    static const VencLibrary& instance() {
        static const VencLibrary lib;
        return lib;
    }

    bool available() const { return m_api.ok(); }
    const VencApi& api() const { return m_api; }
    const std::string& error() const { return m_error; }

private:
    VencLibrary() {
        // RTLD_NOW：符号缺一个就报错，不要拖到第一次调用才崩。
        // RTLD_GLOBAL 关闭（默认 LOCAL）：不让这些符号泄漏到全局作用域，
        // 避免与其它模块的同名符号互相插桩——本文件头注释记的那个
        // H264EncOpenVer2 双实现问题正是这类污染的后果。
        //
        // 只 dlopen libvencoder.so，不碰 libvenc_*.so：让动态链接器按 so
        // 自己的 DT_NEEDED 顺序（codec 在 h264 之前）解析，而不是由我们的
        // 链接命令行决定。
        void* h = dlopen("libvencoder.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libvencoder.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            const char* e = dlerror();
            m_error = std::string("dlopen(libvencoder.so) failed: ") +
                      (e ? e : "unknown");
            return;
        }
        m_handle = h;

        auto sym = [&](const char* name) -> void* {
            void* p = dlsym(h, name);
            if (!p) m_missing += std::string(name) + " ";
            return p;
        };

        m_api.MemAdapterGetOpsS =
            reinterpret_cast<struct ScMemOpsS* (*)()>(sym("MemAdapterGetOpsS"));
        m_api.VideoEncCreate =
            reinterpret_cast<struct VencHandle* (*)(int)>(sym("VideoEncCreate"));
        m_api.VideoEncDestroy =
            reinterpret_cast<void (*)(struct VencHandle*)>(sym("VideoEncDestroy"));
        m_api.VideoEncInit = reinterpret_cast<int (*)(struct VencHandle*,
                                                      VencBaseConfig*)>(
            sym("VideoEncInit"));
        m_api.VideoEncUnInit = reinterpret_cast<int (*)(struct VencHandle*)>(
            sym("VideoEncUnInit"));
        m_api.AllocInputBuffer =
            reinterpret_cast<int (*)(struct VencHandle*,
                                     VencAllocateBufferParam*)>(
                sym("AllocInputBuffer"));
        m_api.ReleaseAllocInputBuffer =
            reinterpret_cast<int (*)(struct VencHandle*)>(
                sym("ReleaseAllocInputBuffer"));
        m_api.GetOneAllocInputBuffer =
            reinterpret_cast<int (*)(struct VencHandle*, VencInputBuffer*)>(
                sym("GetOneAllocInputBuffer"));
        m_api.ReturnOneAllocInputBuffer =
            reinterpret_cast<int (*)(struct VencHandle*, VencInputBuffer*)>(
                sym("ReturnOneAllocInputBuffer"));
        m_api.FlushCacheAllocInputBuffer =
            reinterpret_cast<int (*)(struct VencHandle*, VencInputBuffer*)>(
                sym("FlushCacheAllocInputBuffer"));
        m_api.AddOneInputBuffer =
            reinterpret_cast<int (*)(struct VencHandle*, VencInputBuffer*)>(
                sym("AddOneInputBuffer"));
        m_api.VideoEncodeOneFrame =
            reinterpret_cast<int (*)(struct VencHandle*)>(
                sym("VideoEncodeOneFrame"));
        m_api.GetOneBitstreamFrame =
            reinterpret_cast<int (*)(struct VencHandle*, VencOutputBuffer*)>(
                sym("GetOneBitstreamFrame"));
        m_api.FreeOneBitStreamFrame =
            reinterpret_cast<int (*)(struct VencHandle*, VencOutputBuffer*)>(
                sym("FreeOneBitStreamFrame"));
        m_api.VideoEncSetParameter = reinterpret_cast<int (*)(
            struct VencHandle*, unsigned, void*)>(sym("VideoEncSetParameter"));
        m_api.VideoEncGetParameter = reinterpret_cast<int (*)(
            struct VencHandle*, unsigned, void*)>(sym("VideoEncGetParameter"));
        m_api.AlreadyUsedInputBuffer =
            reinterpret_cast<int (*)(struct VencHandle*, VencInputBuffer*)>(
                sym("AlreadyUsedInputBuffer"));

        if (!m_missing.empty()) {
            m_error = "missing symbols: " + m_missing;
        }
    }

    ~VencLibrary() {
        // 故意不 dlclose：见类注释。进程退出时内核回收。
    }

    VencLibrary(const VencLibrary&) = delete;
    VencLibrary& operator=(const VencLibrary&) = delete;

    void* m_handle{nullptr};
    VencApi m_api{};
    std::string m_error;
    std::string m_missing;
};

// ===========================================================================
// 统计辅助：滑动窗口内的分位数
//
// 为什么要在编码器内部算：`encode_ms_p95` 是归因层判断「编码器跟不上」
// 的直接依据。如果让调用方自己从外部计时，两边会各自采样、各自聚合，
// 窗口一错开就得出互相矛盾的结论。
// ===========================================================================

class LatencyWindow {
public:
    explicit LatencyWindow(size_t cap = 256) : m_cap(cap) { m_samples.reserve(cap); }

    void add(uint32_t ms) {
        if (m_samples.size() >= m_cap) m_samples.clear();  // 简单滚动
        m_samples.push_back(ms);
    }

    /// p50/p95，样本为空时返回 0
    void quantiles(uint32_t& p50, uint32_t& p95) const {
        if (m_samples.empty()) { p50 = p95 = 0; return; }
        std::vector<uint32_t> v = m_samples;
        std::sort(v.begin(), v.end());
        p50 = v[v.size() * 50 / 100];
        p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
    }

    void clear() { m_samples.clear(); }

private:
    std::vector<uint32_t> m_samples;
    size_t m_cap;
};

}  // namespace

// ===========================================================================
// CedarVideoEncoder：板厂 VE 硬件编码器
// ===========================================================================

class CedarVideoEncoder : public IVideoEncoder {
public:
    CedarVideoEncoder() = default;
    ~CedarVideoEncoder() override { teardown(); }

    bool configure(const VideoEncoderConfig& config) override {
        if (m_enc) {
            setError("already configured");
            return false;
        }
        if (config.width == 0 || config.height == 0) {
            setError("zero geometry");
            return false;
        }
        if (config.fourcc != 0 && config.fourcc != kFourccNv12) {
            // 只支持 NV12。别的格式不是「不支持就算了吧」——是把错误
            // 数据喂进硬件，产出绿屏紫屏，且一路无错。必须显式拒绝。
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "unsupported fourcc 0x%08x (only NV12 0x%08x)",
                     config.fourcc, kFourccNv12);
            setError(buf);
            return false;
        }

        const VencLibrary& lib = VencLibrary::instance();
        if (!lib.available()) {
            setError("cedar VE unavailable: " + lib.error());
            return false;
        }
        const VencApi& api = lib.api();

        m_config = config;

        // ---- 对齐与零拷贝可行性判定 ----
        m_align_w = align16(config.width);
        m_y_size = static_cast<uint64_t>(m_align_w) * config.height;
        m_c_size = m_y_size / 2;

        // 驱动回报的事实 UV 偏移。0 表示调用方没填，按 stride*height 推。
        m_uv_offset = config.uv_offset
                          ? config.uv_offset
                          : config.stride * config.height;

        // 零拷贝的前提有两条，缺一不可：
        //
        //   1. 调用方显式开启 allow_zero_copy（默认关闭，见头文件里那条
        //      租约生命周期约束——硬件是异步读的，调用方必须替它守住内存）；
        //   2. 帧里的 UV 起点正好是编码器期望的 Y 平面末尾。
        //
        // 第 2 条差一个字节都不能导入：硬件不会告诉你它读错了地方。
        const bool geo_ok = (m_uv_offset == m_y_size);
        m_zero_copy = config.allow_zero_copy && geo_ok;
        if (config.allow_zero_copy && !geo_ok) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "zero-copy disabled: uv_offset=%u != align16(w)*h=%llu "
                     "(falling back to memcpy, expect one full-frame copy)",
                     m_uv_offset, static_cast<unsigned long long>(m_y_size));
            m_notes = buf;
        } else if (!config.allow_zero_copy) {
            m_notes = "zero-copy off (allow_zero_copy=false): memcpy path";
        }

        if (config.frame_bytes && config.frame_bytes < m_uv_offset + m_c_size) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "frame_bytes=%u < uv_offset+chroma=%llu: frame layout "
                     "inconsistent with driver-reported geometry",
                     config.frame_bytes,
                     static_cast<unsigned long long>(m_uv_offset + m_c_size));
            setError(buf);
            return false;
        }

        // ---- 内存适配器 ----
        m_memops = api.MemAdapterGetOpsS();
        if (!m_memops) {
            setError("MemAdapterGetOpsS returned null");
            return false;
        }
        // 注意：这里调的是本文件里那份 inline 等价实现（解引用 memops->open），
        // 不是 dlsym 出的符号——那两个名字在 sc_interface.h 里是 static inline，
        // 库里根本没有。见 ScMemOpsS 声明处的说明。
        if (CdcMemOpen(m_memops) != 0) {
            setError("CdcMemOpen (memops->open) failed");
            m_memops = nullptr;
            return false;
        }
        m_mem_open = true;

        // ---- 创建编码器 ----
        //
        // 用 VENC_CODEC_H264（=0），不用 H264_VER2（=2）：实测 demo 用的是
        // 前者，而 VER2 会走另一条 OpenVer2 分支，正是那个双实现符号冲突
        // 的落点。
        m_enc = api.VideoEncCreate(kCodecH264);
        if (!m_enc) {
            setError("VideoEncCreate(H264) returned null — check dmesg for "
                     "cedar errors, or the VE may be wedged "
                     "(see describe() for the reload hint)");
            teardown();
            return false;
        }

        // ---- 编码参数 ----
        unsigned fps = config.fps ? config.fps : 25;
        unsigned bitrate = config.bitrate_kbps ? config.bitrate_kbps : 2000;
        unsigned gop = config.gop_size ? config.gop_size : fps;  // 默认 1 秒一个 IDR
        setParam(kIdxBitrate, bitrate);
        setParam(kIdxFramerate, fps);
        setParam(kIdxMaxKeyInterval, gop);

        // ---- 初始化 ----
        VencBaseConfig base;
        std::memset(&base, 0, sizeof(base));
        // bEncH264Nalu = 0 → Annex-B 输出（每帧前带 00 00 00 01 起始码）。
        //
        // 但要注意：**它不负责把 SPS/PPS 放进码流**。板端实测（探针 26）：
        //   bEncH264Nalu=0 时，第 0 帧只有 NAL type 5（IDR），没有 SPS/PPS
        //   bEncH264Nalu=1 时，输出变成 xx xx xx xx 长度前缀格式（非 Annex-B）
        // 而板厂 demo 的落盘文件首帧是 67(SPS) → 68(PPS) → 65(IDR)。
        //
        // 结论：SPS/PPS 只能通过 `VideoEncGetParameter(kIdxH264SpsPps)` 单独取，
        // 由我们自己在首个关键帧前拼上去（见下面「码流头」与 drainOne）。
        // 这不是可选项——没有 SPS 的码流解不出来，播放器会直接判为损坏。
        base.bEncH264Nalu = 0;
        base.nInputWidth = config.width;
        base.nInputHeight = config.height;
        base.nDstWidth = config.width;
        base.nDstHeight = config.height;
        base.nStride = config.stride ? config.stride : config.width;
        base.eInputFormat = kPixelYuv420Sp;
        base.memops = m_memops;
        base.nFrameNumInGroup = 1;

        int rc = api.VideoEncInit(m_enc, &base);
        if (rc != 0) {
            char buf[160];
            snprintf(buf, sizeof(buf), "VideoEncInit rc=%d", rc);
            setError(buf);
            teardown();
            return false;
        }
        m_inited = true;

        // ---- 取 SPS/PPS（码流头）----
        //
        // 必须在 VideoEncInit 之后、编码第一帧之前取：库在这个时刻才生成
        // 参数集，早取是空的。实测返回 24 字节（SPS 16 + PPS 8，均带起始码）。
        //
        // 拿不到不是致命错误——有些配置下库可能推迟到首帧后才生成，那种
        // 情况下在 drainOne 里遇到 IDR 时会再取一次（见 headerBytes()）。
        // 但拿到了就必须留着，它是每个 GOP 起点的必需品。
        refreshHeader(api, "after-init");

        // ---- 输入缓冲 ----
        //
        // 即便走零拷贝导入，也先 AllocInputBuffer：它决定了编码器内部对
        // 「一帧有多大」的认知。尺寸必须是 16 对齐后的 Y/C，不是
        // width*height。
        //
        // nBufferNum 是**像素缓冲池**大小（每块 3 MB），与「4 帧上限」无关。
        // 那个上限来自 VideoEncCreate 里硬编码的描述符队列（固定 4 槽），
        // 由 AlreadyUsedInputBuffer 回收——见 encode() 里那段注释。
        // 这里给 4 是为了让 GetOneAllocInputBuffer 在硬件在途时仍有货可取。
        VencAllocateBufferParam abp;
        std::memset(&abp, 0, sizeof(abp));
        abp.nBufferNum = 4;
        abp.nSizeY = static_cast<unsigned>(m_y_size);
        abp.nSizeC = static_cast<unsigned>(m_c_size);
        rc = api.AllocInputBuffer(m_enc, &abp);
        if (rc != 0) {
            char buf[160];
            snprintf(buf, sizeof(buf), "AllocInputBuffer(Y=%u C=%u) rc=%u",
                     abp.nSizeY, abp.nSizeC, static_cast<unsigned>(rc));
            setError(buf);
            teardown();
            return false;
        }
        m_alloc = true;

        m_window_start_ms = nowMs();
        m_stats.valid = true;
        m_stats.window_ms = 0;  // 由 snapshotAndReset 填事实值
        return true;
    }

    bool encode(const VideoFrameView& frame, EncodedFrame& out) override {
        out = EncodedFrame{};
        if (!m_enc || !m_inited) {
            setError("encode() before successful configure()");
            return false;
        }
        const VencApi& api = VencLibrary::instance().api();

        if (!frame.data && frame.dmabuf_fd < 0) {
            setError("frame has neither data nor dmabuf_fd");
            return false;
        }

        // 主动丢帧：队列满时丢**最旧的**（这里队列深度恒为 1，实际是
        // 「上一帧还没被取走就丢新的」——采集侧要保实时，宁可少一帧
        // 也不要积压）。必须计入 dropped 而不是 failed。
        if (m_in_flight && m_queue_depth >= m_config.max_queue_depth) {
            m_stats.dropped++;
            drainOne(api, nullptr, false);  // 尽力腾出空间，但不产出
        }

        const int64_t t0 = nowMs();

        VencInputBuffer in;
        std::memset(&in, 0, sizeof(in));
        in.nPts = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::nanoseconds(frame.timestamp_ns))
                .count());

        bool used_alloc = false;
        if (m_zero_copy && frame.dmabuf_fd >= 0) {
            // ---- 零拷贝导入 ----
            //
            // 只给 fd + 虚拟地址，不 GetOneAllocInputBuffer。编码器用
            // nShareBufFd 把这块 dma-buf 映射进自己的地址空间，硬件直接
            // 从这块内存读，没有中间拷贝。
            //
            // pAddrVirY/C 仍然填：库在判定是否需要 cache flush 时会用到，
            // 而我们不 flush——dma-buf 是设备一致性内存，且 V4L2 出队时
            // 已同步过。
            in.nShareBufFd = frame.dmabuf_fd;
            in.pAddrVirY = static_cast<unsigned char*>(
                const_cast<void*>(frame.data));
            in.pAddrVirC = in.pAddrVirY
                               ? in.pAddrVirY + m_uv_offset
                               : nullptr;
        } else {
            // ---- memcpy 回退路径 ----
            int grc = api.GetOneAllocInputBuffer(m_enc, &in);
            if (grc != 0) {
                m_stats.failed++;
                setError("GetOneAllocInputBuffer failed");
                return false;
            }
            used_alloc = true;
            if (!frame.data) {
                // 只有 fd 没有映射地址，无法拷贝。这是调用方的错：
                // 要走零拷贝就得让 uv_offset == align16(w)*h，否则必须
                // 提供可读的映射地址。
                api.ReturnOneAllocInputBuffer(m_enc, &in);
                m_stats.failed++;
                setError("memcpy path needs frame.data, but only dmabuf_fd "
                         "was provided (uv_offset mismatch?)");
                return false;
            }
            const size_t have = frame.size;
            const size_t need = static_cast<size_t>(m_uv_offset + m_c_size);
            if (have < need) {
                api.ReturnOneAllocInputBuffer(m_enc, &in);
                m_stats.failed++;
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "frame too small: have=%zu need=%zu", have, need);
                setError(buf);
                return false;
            }
            // Y 与 UV 分段拷贝：中间那 9856 字节对齐空洞留在编码器
            // 缓冲的原值（全零），正好对。
            std::memcpy(in.pAddrVirY, frame.data, m_y_size);
            std::memcpy(in.pAddrVirC,
                        static_cast<const uint8_t*>(frame.data) + m_uv_offset,
                        m_c_size);
            api.FlushCacheAllocInputBuffer(m_enc, &in);
        }

        int arc = api.AddOneInputBuffer(m_enc, &in);
        if (arc != 0) {
            if (used_alloc) api.ReturnOneAllocInputBuffer(m_enc, &in);
            m_stats.failed++;
            char buf[128];
            snprintf(buf, sizeof(buf), "AddOneInputBuffer rc=%d", arc);
            setError(buf);
            return false;
        }

        m_stats.frames_in++;
        m_in_flight = true;
        // 记下这一帧的身份，等它对应的码流被取走时回填（见 drainOne）。
        // 入队点必须在「确认进了硬件」之后：AddOneInputBuffer 失败的帧
        // 永远不会产出码流，若提前入队就会让后面每一帧都错位一格。
        m_in_flight_meta.push(frame.sequence, frame.timestamp_ns);
        m_queue_depth = std::min<uint32_t>(m_queue_depth + 1,
                                           m_config.max_queue_depth + 1);

        int erc = api.VideoEncodeOneFrame(m_enc);

        // ------------------------------------------------------------------
        // 描述符回收：**必须调 AlreadyUsedInputBuffer，否则恰好 4 帧后卡死**
        //
        // 这是本模块最贵的一课，反汇编 + 板端矩阵双重确认：
        //
        //   VideoEncCreate      : ctx+104 硬编码 = 4
        //   VideoEncInit        : FrameBufferManagerCreate(ctx+104) —— 用这个 4
        //                         建「描述符空闲队列」，即 fbm+0x68 只有 4 个槽
        //   AddOneInputBuffer   : dequeue(fbm+0x68)，空则返回 -1
        //                         （那句 "all input buffer is used by encoder"）
        //   VideoEncodeOneFrame : AddUsedInputBuffer 把描述符 +0x60 → +0x58
        //   AlreadyUsedInputBuffer : +0x58 → +0x68，**全库唯一的回收动作**
        //
        // 板厂只在 JPEG 路径（AWJpecEnc）里调它，H.264 demo 用另一条封装路径。
        // 我们照直觉写「encode 完就 ReturnOneAllocInputBuffer」，结果是：
        // H.264 每帧消耗一个描述符，而那个 4 槽队列没有任何补充，
        // 于是**第 5 帧开始 AddOneInputBuffer 永久返回 -1**。
        //
        // 为什么这个坑特别难查：`AllocInputBuffer(nBufferNum)` 改 1/4/8/16
        // 全都无效——它填的是**另一套大内存池**（真·像素缓冲），
        // 与这 4 个描述符槽毫无关系。而 `GetOneAllocInputBuffer` 在卡死时
        // 仍然成功，于是「池子还有空」这个观察把人引向错误的方向。
        //
        // 板端实测（20 帧，同一份探针）：
        //   不调 AlreadyUsedInputBuffer : add_ok=4  bs=4  STUCK
        //   调了                        : add_ok=20 bs=20 FULL
        // 而 memcpy 与零拷贝两条路径的产出字节数**完全相同**（3573429），
        // 顺带证明了零拷贝导入与拷贝回退等价。
        // ------------------------------------------------------------------
        {
            VencInputBuffer used;
            std::memset(&used, 0, sizeof(used));
            const int rrc = api.AlreadyUsedInputBuffer(m_enc, &used);
            if (rrc != 0) {
                // 回收失败意味着描述符队列正在泄漏。这一帧已经不归我们管了，
                // 但必须让计数可见——否则几帧之后又会退化成「莫名卡死」。
                m_stats.failed++;
                char buf[128];
                snprintf(buf, sizeof(buf), "AlreadyUsedInputBuffer rc=%d", rrc);
                setError(buf);
            } else if (used_alloc) {
                // 描述符已回到空闲队列，它指向的那块大内存还给我们自己的池。
                // 零拷贝路径不做这一步：那条路从未从池里取过缓冲。
                api.ReturnOneAllocInputBuffer(m_enc, &used);
            }
        }

        if (erc != 0) {
            // rc=1 (NO_FRAME_BUFFER) 是背压而非故障：这一刻没有可交出的输入，
            // 不计 failed。其它非零码才是真失败。
            if (erc != 1) {
                m_stats.failed++;
                char buf[128];
                snprintf(buf, sizeof(buf), "VideoEncodeOneFrame rc=%d", erc);
                setError(buf);
            }
        }

        if (erc != 0 && erc != 1) {
            // 这一帧没能进硬件。描述符已在上面回收，不留悬空。
            return false;
        }

        const bool produced = drainOne(api, &out, true);
        if (produced) {
            m_latency.add(static_cast<uint32_t>(nowMs() - t0));
            m_stats.frames_out++;
            m_stats.bytes_out += out.size;
            if (out.keyframe) m_stats.keyframes++;
        }
        return produced;
    }

    size_t flush(const std::function<void(const EncodedFrame&)>& out) override {
        if (!m_enc || !m_inited) return 0;
        const VencApi& api = VencLibrary::instance().api();
        size_t n = 0;
        for (int guard = 0; guard < 64; ++guard) {
            EncodedFrame ef;
            if (!drainOne(api, &ef, false)) break;
            m_stats.frames_out++;
            m_stats.bytes_out += ef.size;
            if (ef.keyframe) m_stats.keyframes++;
            if (out) out(ef);
            ++n;
        }
        return n;
    }

    EncoderStats snapshotAndReset() override {
        EncoderStats s = m_stats;
        s.queue_depth = m_queue_depth;
        const int64_t now = nowMs();
        // window_ms 填**事实**时长，不是配置值。0 会让归因层拒绝归因，
        // 这正是我们要的：没测过就不许下结论。
        s.window_ms = (m_window_start_ms > 0 && now > m_window_start_ms)
                          ? static_cast<uint32_t>(now - m_window_start_ms)
                          : 0;
        m_latency.quantiles(s.encode_ms_p50, s.encode_ms_p95);
        s.valid = m_stats.valid && s.frames_in > 0;

        // 清零本期计数，保留配置与窗口起点
        m_stats = EncoderStats{};
        m_stats.valid = (m_enc != nullptr);
        m_window_start_ms = now;
        m_latency.clear();
        return s;
    }

    EncoderStats peekStats() const override {
        EncoderStats s = m_stats;
        s.queue_depth = m_queue_depth;
        const int64_t now = nowMs();
        s.window_ms = (m_window_start_ms > 0 && now > m_window_start_ms)
                          ? static_cast<uint32_t>(now - m_window_start_ms)
                          : 0;
        m_latency.quantiles(s.encode_ms_p50, s.encode_ms_p95);
        s.valid = m_stats.valid && s.frames_in > 0;
        return s;
    }

    std::string backend() const override { return "cedar-ve"; }

    std::string describe() const override {
        std::ostringstream os;
        os << "cedar-ve";
        if (m_enc) {
            os << " " << m_config.width << "x" << m_config.height
               << " align_w=" << m_align_w
               << " uv_offset=" << m_uv_offset
               << " path=" << (m_zero_copy ? "zero-copy(dmabuf)" : "memcpy");
        }
        if (!m_notes.empty()) os << " | " << m_notes;
        if (!m_error.empty()) os << " | error: " << m_error;
        return os.str();
    }

private:
    static constexpr uint32_t kFourccNv12 = 0x3231564Eu;  // 'NV12'

    void setError(const std::string& e) {
        m_error = e;
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO, "CedarVideoEncoder: " << e);
    }

    void setParam(unsigned idx, unsigned value) {
        const VencApi& api = VencLibrary::instance().api();
        int rc = api.VideoEncSetParameter(m_enc, idx, &value);
        if (rc != 0) {
            LOG_WARNING(weaknet_dbus::LogModule::VIDEO, "CedarVideoEncoder: SetParameter(0x" << std::hex << idx
                        << ", " << std::dec << value << ") rc=" << rc
                        << " (continuing with library default)");
        }
    }

    /**
     * @brief 从库里取一份 SPS/PPS 存进 m_header
     *
     * 为什么单独存一份而不是每次现取：`VencHeaderData::pBuffer` 指向库内部
     * 缓冲，生命周期不由我们掌握；而下游要在任意时刻拿它去拼码流头。
     *
     * @param when 仅用于日志区分调用时机
     */
    void refreshHeader(const VencApi& api, const char* when) {
        if (!api.VideoEncGetParameter) return;
        VencHeaderData hd;
        std::memset(&hd, 0, sizeof(hd));
        const int rc =
            api.VideoEncGetParameter(m_enc, kIdxH264SpsPps, &hd);
        if (rc != 0 || hd.pBuffer == nullptr || hd.nLength < 4) {
            // 不是致命错误：某些配置下库要等首帧后才生成。
            if (m_header.empty()) {
                m_notes += std::string(m_notes.empty() ? "" : "; ") +
                           "SPS/PPS not ready " + when;
            }
            return;
        }
        // 必须是 Annex-B。库若给了长度前缀格式，拼进码流就是坏数据。
        if (!(hd.pBuffer[0] == 0 && hd.pBuffer[1] == 0 &&
              hd.pBuffer[2] == 0 && hd.pBuffer[3] == 1)) {
            m_notes += std::string(m_notes.empty() ? "" : "; ") +
                       "SPS/PPS not Annex-B " + when;
            return;
        }
        m_header.assign(hd.pBuffer, hd.pBuffer + hd.nLength);
    }

    /// 码流头（SPS+PPS）字节数；0 表示还没取到
    size_t headerBytes() const { return m_header.size(); }

    /**
     * @brief 取一块码流
     *
     * @param out       非空时填入该结构
     * @param count_stat true 表示这是 encode() 的主路径（会计 in_flight）
     * @return 是否取到了非空码流
     */
    bool drainOne(const VencApi& api, EncodedFrame* out, bool count_stat) {
        // 先清空上一块：调用方若把 data 指针存了下来，重新 assign 会
        // 让那个指针失效得「晚一点」，但至少不会读到上一次的旧码流。
        m_bs_buf.clear();
        VencOutputBuffer ob;
        std::memset(&ob, 0, sizeof(ob));
        if (api.GetOneBitstreamFrame(m_enc, &ob) != 0) return false;

        const size_t size = static_cast<size_t>(ob.nSize0) + ob.nSize1;
        if (size == 0) {
            api.FreeOneBitStreamFrame(m_enc, &ob);
            return false;
        }

        // 关键帧判定：nFlag 的 bit0 是 KEY_FRAME（库约定），但更可靠的
        // 是看 NAL type。Annex-B 起始码 00 00 00 01 后紧跟 NAL header，
        // type = header & 0x1F，5 = IDR。
        bool key = (ob.nFlag & 0x1) != 0;
        if (ob.pData0 && ob.nSize0 >= 5) {
            const uint8_t* p = ob.pData0;
            if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
                const uint8_t nal_type = p[4] & 0x1F;
                if (nal_type == 5) key = true;
            }
        }

        // ------------------------------------------------------------------
        // 必须先拷贝，不能直接把 pData0 交出去再 Free。
        //
        // FreeOneBitStreamFrame 把这块缓冲还给库的内部池，下一次
        // GetOneBitstreamFrame 会立刻复用它。而接口契约是「data 在本次
        // 调用返回后仍然有效」——直接把指针交出去就成了 use-after-free，
        // 调用方拿到的内容会在下一次编码时被静默覆盖。
        //
        // 板端 demo 之所以看不出这个问题，是因为它 Free 之后立刻写文件，
        // 中间没有别的编码发生：库还没来得及复用，所以数据「看起来是对的」。
        // 那是时序侥幸，不是契约。
        //
        // 代价是每次编码多一次 memcpy（典型 I 帧 68 KB / P 帧 2-3 KB，
        // 18.5fps 下不到 2 MB/s）。相对每帧 3 MB 的像素搬运可以忽略，
        // 换来的是「码流指针真的能读」这个不能省的保证。
        // ------------------------------------------------------------------
        //
        // 关键帧前置 SPS/PPS
        //
        // 编码器**不会**把参数集放进码流（见 configure 里 bEncH264Nalu 那段）。
        // 而一份没有 SPS 的 H.264 是解不出来的：解码器拿不到分辨率、profile、
        // 帧/场编码方式，直接判为损坏。所以每个关键帧都要自带一份参数集，
        // 这样任何一段码流从关键帧切进去都能独立解码。
        //
        // 为什么每个 IDR 前都拼、而不是只在文件开头拼一次：
        //   - 事件触发的录像片段可能从任意 GOP 开始；
        //   - 网络丢包后解码器需要重新同步；
        //   - 24 字节的开销对比 I 帧的几十 KB 可以忽略。
        //
        // SPS/PPS 取不到时（m_header 为空）这里退化成原样输出。这种情况
        // 下码流不可解码，describe() 里会带 "SPS/PPS not ready" 说明原因——
        // 不静默、不假装正常。
        // ------------------------------------------------------------------
        if (key && !m_header.empty()) {
            m_bs_buf.clear();
            m_bs_buf.reserve(m_header.size() + size);
            m_bs_buf.insert(m_bs_buf.end(), m_header.begin(), m_header.end());
            m_bs_buf.insert(m_bs_buf.end(), ob.pData0, ob.pData0 + size);
        } else {
            m_bs_buf.assign(ob.pData0, ob.pData0 + size);
        }
        api.FreeOneBitStreamFrame(m_enc, &ob);

        if (out) {
            out->data = m_bs_buf.data();
            out->size = m_bs_buf.size();
            out->keyframe = key;
        }

        // ------------------------------------------------------------------
        // 回填并出队：**这是下游唯一的对时依据**
        // ------------------------------------------------------------------
        //
        // 每块码流都是某一帧输入编出来的，但硬件是异步的——出码流时
        // 当下正在 `encode()` 的那帧并不是它。若不回填，`EncodedFrame`
        // 默认的 `timestamp_ns=0` 会被下游当成「这一帧的时刻」：
        //
        //   - `VideoClipRecorder` 用它做窗口判定。时间戳恒为 0 时
        //     `newest_mono_ms_` 永远是 0，于是「后沿未到」恒成立——
        //     录制器既不产出也不放弃，**静默卡死且不报任何错**。
        //     （2026-11-14 探针在板端就是这样暴露的：编码 60 帧全成功，
        //     录制器 produced=0 aborted=0。）
        //   - 端到端延迟、QoE 归因同样依赖它把「采集的哪一帧」与
        //     「哪块码流」对上。
        //
        // 出队**必须在 `if (out)` 之外**：`out == nullptr` 是「腾空间」调用
        // （见 encode() 里主动丢帧的分支），那块码流已被丢弃，但它的身份
        // 也已被消耗。若只在 `out` 非空时出队，队列就会永久错位一格——
        // 之后每一帧都贴上前一帧的时间戳，而症状只是「时间轴轻微漂移」，
        // 比彻底失效更难发现。
        //
        // 为什么用自建队列而不是读 `VencOutputBuffer::nPts`：
        // 库里那个字段确实存在，但**是否原样回传没有任何证据**
        // （全仓库从未读过它，板厂 demo 也不用）。而这条路径一旦读到
        // 一个被库改写过的值，症状同样是「时间轴错乱」而不是报错。
        // 自建队列的代价只有几个字节/帧，换来的是「这个值一定是我
        // 送进去的那个」——对时依据必须是事实，不能是库的善意。
        // ------------------------------------------------------------------
        InFlightMetaQueue::Meta meta;
        const bool have_meta = m_in_flight_meta.pop(&meta);
        if (out && have_meta) {
            out->sequence = meta.sequence;
            out->timestamp_ns = meta.timestamp_ns;
        }

        if (count_stat) {
            if (m_queue_depth > 0) m_queue_depth--;
            if (m_queue_depth == 0) m_in_flight = false;
        }
        return true;
    }

    void teardown() {
        if (m_enc) {
            const VencApi& api = VencLibrary::instance().api();
            if (m_alloc && api.ReleaseAllocInputBuffer)
                api.ReleaseAllocInputBuffer(m_enc);
            if (m_inited && api.VideoEncUnInit) api.VideoEncUnInit(m_enc);
            if (api.VideoEncDestroy) api.VideoEncDestroy(m_enc);
            m_enc = nullptr;
        }
        m_alloc = false;
        m_inited = false;
        // 在途身份属于这个实例：重建之后旧队列再也对不上任何码流，
        // 留着只会让新实例的第一帧被贴上旧身份。
        m_in_flight_meta.clear();
        m_in_flight = false;
        m_queue_depth = 0;
        // 参数集属于这个编码器实例，换了实例必须重新取。
        m_header.clear();
        if (m_mem_open) {
            // 同理：inline 等价实现（memops->close），不是 dlsym 符号。
            if (m_memops && m_memops->close) CdcMemClose(m_memops);
            m_mem_open = false;
            m_memops = nullptr;
        }
    }

    VencHandle* m_enc{nullptr};
    struct ScMemOpsS* m_memops{nullptr};
    bool m_mem_open{false};
    bool m_inited{false};
    bool m_alloc{false};
    bool m_in_flight{false};

    /// 「已送进硬件、码流还没取出」的帧身份队列。
    /// 与 `m_in_flight` 同步推进；实现见文件上方的 `InFlightMetaQueue`。
    InFlightMetaQueue m_in_flight_meta;

    VideoEncoderConfig m_config{};
    uint32_t m_align_w{0};
    uint32_t m_uv_offset{0};
    uint64_t m_y_size{0};
    uint64_t m_c_size{0};
    bool m_zero_copy{false};

    EncoderStats m_stats{};
    LatencyWindow m_latency{};
    uint32_t m_queue_depth{0};
    int64_t m_window_start_ms{0};
    /// 当前码流的内部副本。存在的唯一理由是 drainOne 必须 Free 之后
    /// 才能返回，而接口承诺 data 在返回后仍有效（见 drainOne 内注释）。
    std::vector<uint8_t> m_bs_buf;

    /// SPS/PPS（Annex-B，SPS 与 PPS 各带起始码）。在 configure() 里取一次，
    /// 之后每个关键帧输出前都会拼上去。空 = 取不到，见 describe() 的 notes。
    std::vector<uint8_t> m_header;

    mutable std::string m_error;
    std::string m_notes;
};

// ===========================================================================
// FakeVideoEncoder：无硬件的桩实现
//
// 它的价值不在于「能跑」，而在于**把失败语义做成可断言的真值表**：
// 几何非法要失败、队列满要计 dropped 且不能计 failed、没有输入时 valid
// 必须是 false。这些才是归因层真正依赖的东西。
// ===========================================================================

class FakeVideoEncoder : public IVideoEncoder {
public:
    explicit FakeVideoEncoder(std::string reason = "")
        : m_reason(std::move(reason)) {}

    bool configure(const VideoEncoderConfig& config) override {
        if (config.width == 0 || config.height == 0) {
            m_error = "zero geometry";
            return false;
        }
        if (config.fourcc != 0 && config.fourcc != 0x3231564Eu) {
            m_error = "unsupported fourcc";
            return false;
        }
        m_config = config;

        // 零拷贝可行性与真后端用同一套判据——桩必须能复现这个判定，
        // 否则「满足条件的机器上会不会走零拷贝」在无硬件环境下测不到。
        const uint64_t align_w = align16(config.width);
        m_y_size = align_w * config.height;
        m_uv_offset = config.uv_offset ? config.uv_offset
                                       : static_cast<uint64_t>(config.stride) *
                                             config.height;
        m_zero_copy = config.allow_zero_copy && (m_uv_offset == m_y_size);
        if (config.allow_zero_copy && m_uv_offset != m_y_size) {
            m_notes = "zero-copy disabled: uv_offset=" +
                      std::to_string(m_uv_offset) +
                      " != align16(w)*h=" + std::to_string(m_y_size);
        } else if (!config.allow_zero_copy) {
            m_notes = "zero-copy off (allow_zero_copy=false)";
        }

        // 帧总长的下限：UV 起点 + 色度平面。比这小说明数据源与配置不一致。
        m_min_frame_bytes = m_uv_offset + m_y_size / 2;
        if (config.frame_bytes && config.frame_bytes < m_min_frame_bytes) {
            m_error = "frame_bytes=" + std::to_string(config.frame_bytes) +
                      " < uv_offset+chroma=" +
                      std::to_string(m_min_frame_bytes);
            return false;
        }

        m_configured = true;
        m_window_start_ms = nowMs();
        // 重新配置 = 新的编码器实例语义：旧的在途身份再也对不上任何码流。
        m_in_flight_meta.clear();
        return true;
    }

    bool encode(const VideoFrameView& frame, EncodedFrame& out) override {
        out = EncodedFrame{};
        if (!m_configured) {
            m_error = "encode() before configure()";
            m_stats.failed++;
            return false;
        }
        if (!frame.data && frame.dmabuf_fd < 0) {
            m_error = "frame has neither data nor dmabuf_fd";
            m_stats.failed++;
            return false;
        }
        // 帧比几何要求的小 = 数据源与配置不一致，必须拒绝而不是猜。
        if (frame.data && frame.size < m_min_frame_bytes) {
            m_error = "frame too small: have=" + std::to_string(frame.size) +
                      " need=" + std::to_string(m_min_frame_bytes);
            m_stats.failed++;
            return false;
        }
        // 桩的队列语义与硬件一致：满了就丢，计入 dropped 而非 failed。
        if (m_queue_depth >= std::max<uint32_t>(1, m_config.max_queue_depth)) {
            m_stats.dropped++;
            return false;
        }
        m_stats.frames_in++;
        m_queue_depth++;
        m_stats.frames_out++;
        m_queue_depth--;
        // 假码流：4 字节起始码 + 1 字节 NAL header。不是合法 H.264，
        // 但足以让「有没有产出、是不是关键帧」的判定被测试。
        static thread_local uint8_t buf[5] = {0, 0, 0, 1, 0x65};
        out.data = buf;
        out.size = sizeof(buf);
        out.keyframe = (m_stats.frames_out % (m_config.gop_size ? m_config.gop_size : 1) == 1);

        // 身份回填**必须走和 cedar 后端同一个队列**。
        //
        // 桩是同步的（一次 encode 对应一块码流），本来直接把 frame 的字段
        // 抄过去就是对的。但仍然过一遍 `InFlightMetaQueue`，理由只有一个：
        // 若桩自己另写一份回填，那么「cedar 忘了回填」这件事在单测里
        // 就永远打不中——2026-11-14 正是这样漏掉的（把真后端的入队行
        // 注释掉，桩的测试依然全绿）。共用同一条代码路径，测试才有意义。
        m_in_flight_meta.push(frame.sequence, frame.timestamp_ns);
        InFlightMetaQueue::Meta meta;
        if (m_in_flight_meta.pop(&meta)) {
            out.sequence = meta.sequence;
            out.timestamp_ns = meta.timestamp_ns;
        }

        m_stats.bytes_out += sizeof(buf);
        if (out.keyframe) m_stats.keyframes++;
        (void)m_latency;  // 桩不产生延迟样本：0 表示「没测过」而非「很快」
        return true;
    }

    size_t flush(const std::function<void(const EncodedFrame&)>&) override {
        return 0;  // 桩没有 pipeline 延迟
    }

    EncoderStats snapshotAndReset() override {
        EncoderStats s = m_stats;
        const int64_t now = nowMs();
        s.window_ms = (m_window_start_ms > 0 && now > m_window_start_ms)
                          ? static_cast<uint32_t>(now - m_window_start_ms)
                          : 0;
        s.queue_depth = 0;
        s.valid = m_configured && s.frames_in > 0;
        m_stats = EncoderStats{};
        m_window_start_ms = now;
        return s;
    }

    EncoderStats peekStats() const override {
        EncoderStats s = m_stats;
        const int64_t now = nowMs();
        s.window_ms = (m_window_start_ms > 0 && now > m_window_start_ms)
                          ? static_cast<uint32_t>(now - m_window_start_ms)
                          : 0;
        s.queue_depth = m_queue_depth;
        s.valid = m_configured && s.frames_in > 0;
        return s;
    }

    std::string backend() const override { return "fake"; }

    std::string describe() const override {
        std::ostringstream os;
        os << "fake";
        if (m_configured) {
            os << " " << m_config.width << "x" << m_config.height
               << " path=" << (m_zero_copy ? "zero-copy(dmabuf)" : "memcpy");
        }
        if (!m_notes.empty()) os << " | " << m_notes;
        if (!m_reason.empty()) os << " | " << m_reason;
        if (!m_error.empty()) os << " | error: " << m_error;
        return os.str();
    }

private:
    VideoEncoderConfig m_config{};
    EncoderStats m_stats{};
    bool m_configured{false};
    uint32_t m_queue_depth{0};
    uint64_t m_y_size{0};
    uint64_t m_uv_offset{0};
    uint64_t m_min_frame_bytes{0};
    bool m_zero_copy{false};
    int64_t m_window_start_ms{0};
    std::string m_reason;
    std::string m_error;
    std::string m_notes;
    LatencyWindow m_latency{};
    /// 与 cedar 后端**共用**的身份队列（见 encode() 里的说明）。
    InFlightMetaQueue m_in_flight_meta;
};

// ===========================================================================
// 工厂
// ===========================================================================

std::unique_ptr<IVideoEncoder> createVideoEncoder(bool prefer_hardware) {
    if (prefer_hardware) {
        const VencLibrary& lib = VencLibrary::instance();
        if (lib.available()) {
            LOG_INFO(weaknet_dbus::LogModule::VIDEO, "VideoEncoder: 使用 cedar VE 硬件后端"
                     << "（符号表就绪）");
            return std::make_unique<CedarVideoEncoder>();
        }
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO, "VideoEncoder: cedar VE 不可用（"
                    << lib.error() << "），回落到桩后端");
        return std::make_unique<FakeVideoEncoder>(
            "cedar VE unavailable: " + lib.error());
    }
    return std::make_unique<FakeVideoEncoder>("hardware not preferred");
}

std::unique_ptr<IVideoEncoder> createFakeVideoEncoder() {
    return std::make_unique<FakeVideoEncoder>();
}

}  // namespace weaknet
