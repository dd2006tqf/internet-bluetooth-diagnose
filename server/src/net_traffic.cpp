/**
 * @file net_traffic.cpp
 * @brief 基于 eBPF/libbpf 的网络流量分析与异常检测
 *
 * @details 本文件实现 NetTrafficAnalyzer 单例类，利用 eBPF（extended Berkeley Packet
 *          Filter）在内核态实时采集 TCP/UDP 发送流量，再在用户态做流级别的统计、
 *          Top-N 排行、以及突发流量/可疑流量异常检测。
 *
 *          设计要点：
 *          - 通过 libbpf 加载编译好的 BPF 对象（.o 文件），自动 attach kprobe/fentry
 *          - 内核态 BPF 程序 `tcp_transmit_entry` / `udp_send_entry` 按流五元组
 *            聚合计数到 BPF map `current_sec`（bytes、packets、pid）
 *          - 采样流程：清空 map → sleep 采样窗口 → 遍历读取 map → 计算速率
 *          - 接口过滤：通过 `cfg_iface` BPF map 写入 ifindex，内核态可按接口过滤
 *          - 异常检测：维护每流的历史窗口，结合 burstMultiplier / suspiciousThreshold
 *            三个可调参数，识别 burst（突发）、suspicious（可疑）、high_volume（高流量）
 *            三类异常并计算 severity（严重程度 0~1）
 *
 * @note 关键依赖：
 *       - libbpf（运行时库）：libbpf 1.0+，提供 bpf_object__open/load/attach/map 操作
 *       - 内核：需要 eBPF kprobe/fentry 支持（Linux 4.18+）
 *       - BPF 对象文件：由外部传入路径，默认期望包含以下符号：
 *         程序: tcp_transmit_entry, udp_send_entry
 *         map:  current_sec, process_stats, cfg_iface（可选）
 *
 *       编译期通过 __has_include 探测 <linux/bpf.h> + <bpf/libbpf.h> 可用性，
 *       若任一缺失则 HAVE_LIBBPF=0，所有方法安全返回空值。
 */

#include "net_traffic.h"
#include "logger.hpp"

using namespace weaknet_dbus;

#include <arpa/inet.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <net/if.h>
#include <unordered_map>
#include <sstream>
#include <errno.h>
#include <cmath>
#include <numeric>

// ---------------------------------------------------------------------------
// eBPF / libbpf 头文件探测（编译期可选依赖）
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include(<linux/bpf.h>) && __has_include(<bpf/libbpf.h>) && __has_include(<bpf/bpf.h>)
#    define HAVE_LIBBPF 1
// 使用 extern "C" 避免 C++ 名字修饰，因为 libbpf 是 C 库
extern "C" {
#include <linux/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
}
#  else
#    define HAVE_LIBBPF 0
#  endif
#else
#  define HAVE_LIBBPF 0
#endif

// ---------------------------------------------------------------------------
// NetTrafficAnalyzer 单例静态成员
// ---------------------------------------------------------------------------
std::once_flag NetTrafficAnalyzer::s_onceFlag;
std::shared_ptr<NetTrafficAnalyzer> NetTrafficAnalyzer::s_instance;

/** @brief 获取单例实例，线程安全（std::call_once） */
std::shared_ptr<NetTrafficAnalyzer> NetTrafficAnalyzer::getInstance() {
    std::call_once(s_onceFlag, [](){ s_instance = std::shared_ptr<NetTrafficAnalyzer>(new NetTrafficAnalyzer()); });
    return s_instance;
}

/** @brief 设置 BPF 对象（.o 文件）的加载路径 */
void NetTrafficAnalyzer::setBpfObjectPath(const std::string& path) { bpfObjPath_ = path; }

/**
 * @brief 将 32 位网络序 IP 地址转换为点分十进制字符串
 *
 * @param ip 网络字节序的 IPv4 地址（uint32_t）
 * @return 如 "192.168.1.1" 的字符串
 */
static std::string ip_str(uint32_t ip) {
    struct in_addr a{ip}; // in_addr 内部存储 32 位网络序地址
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return std::string(buf);
}

/**
 * @brief 初始化并 attach BPF 程序到指定接口
 *
 * 工作流程：
 * 1. 设置 libbpf 严格模式 + 自定义日志输出
 * 2. bpf_object__open 加载 .o 文件，bpf_object__load 加载到内核
 * 3. 查找必需的 current_sec map 和可选的 process_stats / cfg_iface map
 * 4. 若 cfg_iface map 存在，写入目标接口的 ifindex，内核态可据此过滤
 * 5. 查找 tcp_transmit_entry / udp_send_entry 程序并 bpf_program__attach
 *
 * @param ifaceName 要绑定的网卡接口名（用于写入 cfg_iface map）
 *
 * @return true  - BPF 对象加载成功且至少一个程序 attach 成功
 *         false - libbpf 不可用、文件缺失、加载失败、attach 全部失败
 */
bool NetTrafficAnalyzer::initForInterface(const std::string& ifaceName) {
#if !HAVE_LIBBPF
    (void)ifaceName;
    return false;
#else
    // 开启 libbpf 严格模式，尽早暴露 API 误用
    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
    // 将 libbpf 日志输出到 stderr
    libbpf_set_print([](enum libbpf_print_level level, const char *fmt, va_list args) -> int {
        (void)level; return vfprintf(stderr, fmt, args);
    });
    if (attached_) return true;

    // ========== 1. 打开并加载 BPF 对象 ==========
    bpf_object* obj = bpf_object__open(bpfObjPath_.c_str());
    if (!obj) return false;
    if (bpf_object__load(obj)) { bpf_object__close(obj); return false; }

    // ========== 2. 查找 BPF map fd ==========
    // current_sec：必需，按流五元组聚合计数的主 map
    mapCurrFd_ = bpf_object__find_map_fd_by_name(obj, "current_sec");
    if (mapCurrFd_ < 0) { bpf_object__close(obj); return false; }

    // process_stats：可选，供 ProcessNetProfiler 共享读取
    mapProcessStatsFd_ = bpf_object__find_map_fd_by_name(obj, "process_stats");

    // cfg_iface：可选控制 map，若内核态支持则写入接口索引做过滤
    mapCfgFd_ = bpf_object__find_map_fd_by_name(obj, "cfg_iface");
    if (mapCfgFd_ >= 0) {
        // 将接口名转为 ifindex，写入 BPF map 的 key=0 位置
        unsigned ifi = if_nametoindex(ifaceName.c_str());
        if (ifi > 0) {
            int zero = 0; unsigned val = ifi;
            bpf_map_update_elem(mapCfgFd_, &zero, &val, BPF_ANY);
        }
    }

    // ========== 3. attach BPF 程序 ==========
    // 自动识别 kprobe / fentry / tc 等 attach 类型
    bpf_program* prog_tcp = bpf_object__find_program_by_name(obj, "tcp_transmit_entry");
    bpf_program* prog_udp = bpf_object__find_program_by_name(obj, "udp_send_entry");
    if (!prog_tcp || !prog_udp) { bpf_object__close(obj); return false; }
    bpf_link* l1 = bpf_program__attach(prog_tcp);
    long err_tcp = libbpf_get_error(l1);
    if (err_tcp) {
        LOG_ERROR(LogModule::NETWORK, "attach kprobe/ip_queue_xmit failed: err=" << err_tcp << " errno=" << errno);
        if (!l1) {/* noop */} else { bpf_link__destroy(l1); }
        l1 = nullptr;
    }
    bpf_link* l2 = bpf_program__attach(prog_udp);
    long err_udp = libbpf_get_error(l2);
    if (err_udp) {
        LOG_ERROR(LogModule::NETWORK, "attach kprobe/udp_sendmsg failed: err=" << err_udp << " errno=" << errno);
        if (!l2) {/* noop */} else { bpf_link__destroy(l2); }
        l2 = nullptr;
    }

    // 至少一个程序 attach 成功即可认为初始化成功
    if (!l1 && !l2) { bpf_object__close(obj); return false; }
    bpfObj_ = obj; linkTcp_ = l1; linkUdp_ = l2; attached_ = true; boundIface_ = ifaceName;
    return true;
#endif
}

/**
 * @brief 采样当前 Top-N 流量流
 *
 * 采样窗口流程：
 * 1. 清空 current_sec map（bpf_map_get_next_key + bpf_map_delete_elem 遍历删除）
 * 2. sleep(intervalSec) 等待内核态在窗口内采集
 * 3. 遍历读取 map，按五元组解析并换算为 bps/pps
 * 4. 按 bps 降序排序，截断到 topN 条
 *
 * @param intervalSec 采样窗口长度（秒）
 * @param topN        保留的最大流数量（超出截断）
 *
 * @return FlowRate 向量，每条记录代表一个五元组流；未 attach 时返回空
 */
std::vector<FlowRate> NetTrafficAnalyzer::sampleTopFlows(int intervalSec, int topN) {
	    std::vector<FlowRate> out;
	    if (!attached_) return out;
	#if !HAVE_LIBBPF
	    return out;
	#else
	    // 与内核态 BPF map key/val 布局一致的结构体
	    struct conn_key { __u32 saddr, daddr; __u16 sport, dport; __u8 protocol; } key{}, next_key{};
	    struct flow_data { __u64 bytes; __u64 packets; __u32 pid; } val{};

	    // ========== 清空 map：确保采样窗口只包含新产生的流量 ==========
	    int ret = bpf_map_get_next_key(mapCurrFd_, nullptr, &next_key);
	    while (ret == 0) {
	        bpf_map_delete_elem(mapCurrFd_, &next_key);
	        key = next_key;
	        ret = bpf_map_get_next_key(mapCurrFd_, &key, &next_key);
	    }

	    // ========== 等待采样窗口 ==========
	    std::this_thread::sleep_for(std::chrono::seconds(intervalSec));

	    // ========== 读取窗口内所有流量（无需差值，因为 map 已清空） ==========
	    ret = bpf_map_get_next_key(mapCurrFd_, nullptr, &next_key);
	    while (ret == 0) {
	        if (bpf_map_lookup_elem(mapCurrFd_, &next_key, &val) == 0) {
	            FlowRate fr;
	            // IP 从网络序转为字符串、端口从网络序转主机序
	            fr.src = ip_str(next_key.saddr);
	            fr.dst = ip_str(next_key.daddr);
	            fr.sport = ntohs(next_key.sport);
	            fr.dport = ntohs(next_key.dport);
	            // protocol: 6=TCP, 17=UDP，其他直接输出数字
	            fr.proto = (next_key.protocol == 6) ? "TCP" : (next_key.protocol == 17 ? "UDP" : std::to_string(next_key.protocol));
	            // bytes / interval = bps，packets / interval = pps
	            fr.bps = val.bytes / (uint64_t)intervalSec;
	            fr.pps = val.packets / (uint64_t)intervalSec;
	            fr.pid = val.pid;
	            out.push_back(fr);
	        }
	        key = next_key;
	        ret = bpf_map_get_next_key(mapCurrFd_, &key, &next_key);
	    }

	    // 按 bps 降序排序，截断到 topN
	    std::sort(out.begin(), out.end(), [](const FlowRate& a, const FlowRate& b){ return a.bps > b.bps; });
	    if ((int)out.size() > topN) out.resize(topN);
	    return out;
	#endif
	}

// 新增功能实现

/**
 * @brief 生成流的唯一标识字符串
 *
 * 格式：<src>:<sport>-<dst>:<dport>/<proto>
 * 例如 "192.168.1.100:54321-8.8.8.8:53/UDP"
 *
 * @param flow 流记录
 * @return 可用于 unordered_map/unordered_set 的键
 */
std::string NetTrafficAnalyzer::generateFlowKey(const FlowRate& flow) {
    std::ostringstream oss;
    oss << flow.src << ":" << flow.sport << "-" << flow.dst << ":" << flow.dport << "/" << flow.proto;
    return oss.str();
}

/**
 * @brief 判断当前流量是否为突发流量
 *
 * 判断条件：当前 bps 超过历史平均值 × burstMultiplier
 *
 * @param history     该流的历史统计窗口（bpsHistory 长度 ≥ 3 才判定）
 * @param currentBps  当前采样值
 *
 * @return true  - 检测到突发
 *         false - 历史不足或未超过倍数阈值
 */
bool NetTrafficAnalyzer::isBurstTraffic(const TrafficHistory& history, uint64_t currentBps) {
    if (history.bpsHistory.size() < 3) return false;
    
    // 计算历史平均值（使用 accumulate 求和）
    uint64_t avgBps = std::accumulate(history.bpsHistory.begin(), history.bpsHistory.end(), 0ULL) / history.bpsHistory.size();
    
    // 检查是否超过平均值的突发倍数阈值
    return currentBps > (avgBps * burstMultiplier_);
}

/**
 * @brief 判断当前流量是否达到可疑流量阈值
 *
 * 基础实现：bps 超过 suspiciousThresholdBps 即视为可疑；
 * 预留扩展点可增加 PID 行为模式检测。
 *
 * @param currentBps 当前采样值
 * @param pid        产生流量的进程 PID（暂未使用）
 *
 * @return true  - 超过可疑阈值
 */
bool NetTrafficAnalyzer::isSuspiciousTraffic(uint64_t currentBps, uint32_t pid) {
    (void)pid;
    // 检查是否超过可疑流量阈值
    if (currentBps < suspiciousThresholdBps_) return false;
    
    // 可扩展：特定 PID 的异常流量模式检测
    return true;
}

/**
 * @brief 计算流量异常的严重程度
 *
 * 公式：severity = clamp((ratio - 1) / (multiplier - 1), 0, 1)
 * 其中 ratio = currentBps / threshold。
 * 当 currentBps ≤ threshold 时返回 0。
 *
 * @param currentBps 当前 bps 值
 * @param threshold  阈值（burstThreshold 或 suspiciousThreshold）
 * @param multiplier 归一化放大倍数（决定 severity 达 1.0 时对应的 currentBps 倍数）
 *
 * @return 严重程度，范围 [0.0, 1.0]
 */
double NetTrafficAnalyzer::calculateSeverity(uint64_t currentBps, uint64_t threshold, double multiplier) {
    if (currentBps <= threshold) return 0.0;
    
    double ratio = (double)currentBps / threshold;
    // 线性映射到 [0, 1]，上限截断到 1.0
    double severity = std::min(1.0, (ratio - 1.0) / (multiplier - 1.0));
    return severity;
}

/**
 * @brief 执行一次完整的流量异常检测
 *
 * 流程：采样 Top 流 → 更新每流历史窗口 → 依次检测 burst / suspicious / high_volume 三类异常
 *
 * @param intervalSec          采样窗口（秒）
 * @param burstThresholdBps    突发流量阈值（bps）
 * @param suspiciousThresholdBps 可疑/高流量阈值（bps）
 * @param burstMultiplier      突发判定倍率（历史均值 × 该值视为突发）
 *
 * @return 异常列表，每条包含 flowKey、类型、当前值、阈值、严重程度、时间戳、描述
 */
std::vector<TrafficAnomaly> NetTrafficAnalyzer::detectAnomalies(int intervalSec, 
                                                               uint64_t burstThresholdBps,
                                                               uint64_t suspiciousThresholdBps,
                                                                                         double burstMultiplier) {
    std::vector<TrafficAnomaly> anomalies;
    
    if (!attached_) return anomalies;
    
    // 获取当前流量数据（取更多流以覆盖长尾）
    auto flows = sampleTopFlows(intervalSec, 1000);
    
    // 加锁保护 trafficHistory_ 的并发访问
    std::lock_guard<std::mutex> lock(historyMutex_);
    auto now = std::chrono::system_clock::now();
    
    for (const auto& flow : flows) {
        std::string flowKey = generateFlowKey(flow);
        
        // 更新该流的历史窗口（bps/pps 环形缓冲、累计字节包数、最后更新时间）
        auto& history = trafficHistory_[flowKey];
        history.bpsHistory.push_back(flow.bps);
        history.ppsHistory.push_back(flow.pps);
        history.totalBytes += flow.bps * intervalSec;
        history.totalPackets += flow.pps * intervalSec;
        history.lastUpdate = now;
        
        // 限制历史窗口大小，避免 map 无限膨胀
        if (history.bpsHistory.size() > MAX_HISTORY_SIZE) {
            history.bpsHistory.pop_front();
            history.ppsHistory.pop_front();
        }
        
        // ---------- 检测突发流量 ----------
        if (flow.bps > burstThresholdBps && isBurstTraffic(history, flow.bps)) {
            TrafficAnomaly anomaly;
            anomaly.flowKey = flowKey;
            anomaly.anomalyType = "burst";
            anomaly.currentBps = flow.bps;
            anomaly.thresholdBps = burstThresholdBps;
            anomaly.severity = calculateSeverity(flow.bps, burstThresholdBps, burstMultiplier);
            anomaly.timestamp = now;
            anomaly.description = "检测到突发流量: " + std::to_string(flow.bps / (1024*1024)) + " MB/s";
            anomalies.push_back(anomaly);
        }
        
        // ---------- 检测可疑流量 ----------
        if (isSuspiciousTraffic(flow.bps, flow.pid)) {
            TrafficAnomaly anomaly;
            anomaly.flowKey = flowKey;
            anomaly.anomalyType = "suspicious";
            anomaly.currentBps = flow.bps;
            anomaly.thresholdBps = suspiciousThresholdBps;
            anomaly.severity = calculateSeverity(flow.bps, suspiciousThresholdBps, 2.0);
            anomaly.timestamp = now;
            anomaly.description = "检测到可疑流量: " + std::to_string(flow.bps / (1024*1024)) + " MB/s, PID: " + std::to_string(flow.pid);
            anomalies.push_back(anomaly);
        }
        
        // ---------- 检测高流量 ----------
        if (flow.bps > suspiciousThresholdBps) {
            TrafficAnomaly anomaly;
            anomaly.flowKey = flowKey;
            anomaly.anomalyType = "high_volume";
            anomaly.currentBps = flow.bps;
            anomaly.thresholdBps = suspiciousThresholdBps;
            anomaly.severity = calculateSeverity(flow.bps, suspiciousThresholdBps, 1.5);
            anomaly.timestamp = now;
            anomaly.description = "检测到高流量: " + std::to_string(flow.bps / (1024*1024)) + " MB/s";
            anomalies.push_back(anomaly);
        }
    }
    
    return anomalies;
}

/**
 * @brief 获取当前所有流的历史统计窗口（线程安全拷贝）
 * @return map<流键, TrafficHistory> 的完整拷贝
 */
std::map<std::string, TrafficHistory> NetTrafficAnalyzer::getTrafficHistory() {
    std::lock_guard<std::mutex> lock(historyMutex_);
    return trafficHistory_;
}

/**
 * @brief 动态调整异常检测参数（线程安全）
 * @param burstThreshold       突发流量阈值（bps）
 * @param suspiciousThreshold  可疑/高流量阈值（bps）
 * @param burstMultiplier      突发判定倍率
 */
void NetTrafficAnalyzer::setAnomalyDetectionParams(uint64_t burstThreshold, uint64_t suspiciousThreshold, double burstMultiplier) {
    std::lock_guard<std::mutex> lock(historyMutex_);
    burstThresholdBps_ = burstThreshold;
    suspiciousThresholdBps_ = suspiciousThreshold;
    burstMultiplier_ = burstMultiplier;
}

/**
 * @brief 获取 1 秒窗口的实时流量统计摘要
 *
 * 统计当前活跃流数量、总 bps、总 pps。
 *
 * @return RealTimeStats 结构体；未 attach 时 activeFlows/totalBps/totalPps 全为 0
 */
NetTrafficAnalyzer::RealTimeStats NetTrafficAnalyzer::getRealTimeStats() {
    RealTimeStats stats;
    
    if (!attached_) return stats;
    
    // 1 秒采样窗口，取足够多的流（1000 覆盖绝大多数场景）
    auto flows = sampleTopFlows(1, 1000);
    
    stats.timestamp = std::chrono::system_clock::now();
    stats.activeFlows = flows.size();
    
    for (const auto& flow : flows) {
        stats.totalBps += flow.bps;
        stats.totalPps += flow.pps;
    }
    
    return stats;
}

/**
 * @brief 清空所有流的历史统计窗口（线程安全）
 */
void NetTrafficAnalyzer::clearHistory() {
    std::lock_guard<std::mutex> lock(historyMutex_);
    trafficHistory_.clear();
}
