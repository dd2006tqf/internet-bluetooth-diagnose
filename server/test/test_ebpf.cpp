// test_ebpf.cpp
// eBPF 功能专项测试
// 测试 BPF 程序加载、kprobe 挂载、流量数据采集
// 编译: g++ -std=c++17 -O2 -Wall -Iinclude -o test/test_ebpf test/test_ebpf.cpp src/net_traffic.cpp src/traffic_analyzer.cpp src/traffic_anomaly_detector.cpp src/serializer.cpp src/net_info.cpp src/logger.cpp $(pkg-config --cflags --libs libbpf) -lglog -lbpf

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sstream>
#include <iomanip>

#include "net_traffic.h"
#include "logger.hpp"

using namespace weaknet_dbus;

// 测试结果统计
struct TestStats {
    int total = 0;
    int passed = 0;
    int failed = 0;
    
    void addResult(bool success, const std::string& testName) {
        total++;
        if (success) {
            passed++;
            std::cout << "  ✅ " << testName << std::endl;
        } else {
            failed++;
            std::cout << "  ❌ " << testName << std::endl;
        }
    }
    
    void print() {
        std::cout << "\n📊 测试统计: 总计=" << total 
                  << ", 通过=" << passed 
                  << ", 失败=" << failed 
                  << ", 成功率=" << (total > 0 ? (passed * 100.0 / total) : 0.0) << "%" << std::endl;
    }
};

TestStats g_stats;

// 测试1: 检查 BPF 对象文件是否存在
bool testBpfObjectExists() {
    std::cout << "\n🧪 测试1: BPF 对象文件检查" << std::endl;
    
    std::string bpfPath = "build/flow_rate.bpf.o";
    struct stat st;
    
    if (stat(bpfPath.c_str(), &st) == 0) {
        g_stats.addResult(true, "BPF 对象文件存在: " + bpfPath);
        std::cout << "     📦 文件大小: " << st.st_size << " 字节" << std::endl;
        return true;
    } else {
        g_stats.addResult(false, "BPF 对象文件不存在: " + bpfPath);
        return false;
    }
}

// 测试2: 检查 libbpf 支持
bool testLibbpfSupport() {
    std::cout << "\n🧪 测试2: libbpf 支持检查" << std::endl;
    
    #if defined(__has_include)
    #  if __has_include(<linux/bpf.h>) && __has_include(<bpf/libbpf.h>) && __has_include(<bpf/bpf.h>)
        g_stats.addResult(true, "libbpf 头文件可用");
        return true;
    #  else
        g_stats.addResult(false, "libbpf 头文件不可用");
        return false;
    #  endif
    #else
        g_stats.addResult(false, "无法检查 libbpf 支持");
        return false;
    #endif
}

// 测试3: 初始化流量分析器
bool testTrafficAnalyzerInit() {
    std::cout << "\n🧪 测试3: 流量分析器初始化" << std::endl;
    
    auto analyzer = NetTrafficAnalyzer::getInstance();
    if (!analyzer) {
        g_stats.addResult(false, "获取流量分析器实例失败");
        return false;
    }
    g_stats.addResult(true, "获取流量分析器实例");
    
    // 设置 BPF 对象路径
    analyzer->setBpfObjectPath("build/flow_rate.bpf.o");
    g_stats.addResult(true, "设置 BPF 对象路径");
    
    // 设置异常检测参数
    analyzer->setAnomalyDetectionParams(
        5 * 1024 * 1024,    // 突发阈值: 5MB/s
        20 * 1024 * 1024,   // 可疑阈值: 20MB/s
        2.5                 // 突发倍数: 2.5倍
    );
    g_stats.addResult(true, "设置异常检测参数");
    
    return true;
}

// 测试4: 初始化网络接口（加载 BPF 程序）
bool testInterfaceInit() {
    std::cout << "\n🧪 测试4: 网络接口初始化（BPF 加载）" << std::endl;
    
    auto analyzer = NetTrafficAnalyzer::getInstance();
    
    // 获取当前活跃的网络接口
    std::string activeIface = "eth0";  // 默认使用 eth0
    
    // 尝试读取 /proc/net/route 找到默认接口
    std::ifstream routeFile("/proc/net/route");
    if (routeFile.is_open()) {
        std::string line;
        std::getline(routeFile, line);  // 跳过标题行
        while (std::getline(routeFile, line)) {
            std::istringstream iss(line);
            std::string iface;
            unsigned int dest;
            if (iss >> iface >> std::hex >> dest) {
                if (dest == 0) {  // 默认路由
                    activeIface = iface;
                    break;
                }
            }
        }
        routeFile.close();
    }
    
    std::cout << "     📡 使用网络接口: " << activeIface << std::endl;
    
    // 初始化接口（这会加载 BPF 程序）
    bool initResult = analyzer->initForInterface(activeIface);
    
    if (initResult) {
        g_stats.addResult(true, "BPF 程序加载成功");
        g_stats.addResult(true, "kprobe 挂载成功");
        return true;
    } else {
        g_stats.addResult(false, "BPF 程序加载失败（可能需要 root 权限）");
        std::cout << "     💡 提示: 请确保以 root 权限运行此测试" << std::endl;
        return false;
    }
}

// 测试5: 流量数据采集
bool testTrafficSampling() {
    std::cout << "\n🧪 测试5: 流量数据采集" << std::endl;
    
    auto analyzer = NetTrafficAnalyzer::getInstance();
    
    // 采样流量数据（采样间隔 2 秒，取前 10 个流）
    std::cout << "     🔍 正在采样流量数据（2秒）..." << std::endl;
    auto flows = analyzer->sampleTopFlows(2, 10);
    
    if (flows.empty()) {
        g_stats.addResult(false, "未采集到流量数据");
        std::cout << "     💡 提示: 可能需要生成网络流量（如 ping、curl）" << std::endl;
        return false;
    }
    
    g_stats.addResult(true, "流量数据采集成功");
    std::cout << "     📊 采集到 " << flows.size() << " 个流量流:" << std::endl;
    
    for (size_t i = 0; i < flows.size() && i < 5; i++) {
        const auto& flow = flows[i];
        std::cout << "       " << (i + 1) << ". " 
                  << flow.src << ":" << flow.sport << " -> " 
                  << flow.dst << ":" << flow.dport 
                  << " [" << flow.proto << "] "
                  << flow.bps << " B/s, " 
                  << flow.pps << " pkt/s" << std::endl;
    }
    
    return true;
}

// 测试6: 实时统计
bool testRealTimeStats() {
    std::cout << "\n🧪 测试6: 实时统计获取" << std::endl;
    
    auto analyzer = NetTrafficAnalyzer::getInstance();
    
    try {
        auto stats = analyzer->getRealTimeStats();
        
        g_stats.addResult(true, "获取实时统计成功");
        std::cout << "     📊 实时统计:" << std::endl;
        std::cout << "       - 总字节/秒: " << stats.totalBps << " B/s" << std::endl;
        std::cout << "       - 总包/秒: " << stats.totalPps << " pkt/s" << std::endl;
        std::cout << "       - 活跃流数: " << stats.activeFlows << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        g_stats.addResult(false, "获取实时统计失败: " + std::string(e.what()));
        return false;
    }
}

// 测试7: 异常流量检测
bool testAnomalyDetection() {
    std::cout << "\n🧪 测试7: 异常流量检测" << std::endl;
    
    auto analyzer = NetTrafficAnalyzer::getInstance();
    
    try {
        auto anomalies = analyzer->detectAnomalies(5);
        
        if (anomalies.empty()) {
            g_stats.addResult(true, "异常检测运行成功（未检测到异常）");
            std::cout << "     ✅ 当前流量正常，未检测到异常" << std::endl;
        } else {
            g_stats.addResult(true, "异常检测运行成功");
            std::cout << "     ⚠️  检测到 " << anomalies.size() << " 个异常:" << std::endl;
            
            for (const auto& anomaly : anomalies) {
                std::cout << "       - " << anomaly.anomalyType 
                          << " on " << anomaly.flowKey 
                          << " (严重度: " << (anomaly.severity * 100) << "%)" << std::endl;
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        g_stats.addResult(false, "异常检测失败: " + std::string(e.what()));
        return false;
    }
}

// 测试8: 流量历史记录
bool testTrafficHistory() {
    std::cout << "\n🧪 测试8: 流量历史记录" << std::endl;
    
    auto analyzer = NetTrafficAnalyzer::getInstance();
    
    try {
        auto history = analyzer->getTrafficHistory();
        
        if (history.empty()) {
            g_stats.addResult(true, "流量历史记录获取成功（暂无记录）");
            std::cout << "     ℹ️  暂无历史记录（正常，需要先进行流量采样）" << std::endl;
        } else {
            g_stats.addResult(true, "流量历史记录获取成功");
            std::cout << "     📊 记录了 " << history.size() << " 个接口的历史数据" << std::endl;
            
            for (const auto& [iface, hist] : history) {
                std::cout << "       - " << iface << ": " 
                          << hist.bpsHistory.size() << " 条 BPS 记录, "
                          << hist.ppsHistory.size() << " 条 PPS 记录" << std::endl;
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        g_stats.addResult(false, "获取流量历史失败: " + std::string(e.what()));
        return false;
    }
}

// 测试9: 清理历史数据
bool testClearHistory() {
    std::cout << "\n🧪 测试9: 清理历史数据" << std::endl;
    
    auto analyzer = NetTrafficAnalyzer::getInstance();
    
    try {
        analyzer->clearHistory();
        g_stats.addResult(true, "清理历史数据成功");
        return true;
    } catch (const std::exception& e) {
        g_stats.addResult(false, "清理历史数据失败: " + std::string(e.what()));
        return false;
    }
}

int main() {
    std::cout << "🚀 开始 eBPF 功能专项测试" << std::endl;
    std::cout << "============================================================" << std::endl;
    
    // 初始化日志
    Logger::init("test_ebpf", "./logs/test", LogLevel::INFO, true);
    
    // 检查是否为 root 用户
    if (geteuid() != 0) {
        std::cout << "\n⚠️  警告: 当前不是 root 用户，eBPF 功能可能无法正常工作" << std::endl;
        std::cout << "💡 提示: 请使用 sudo 运行此测试" << std::endl;
    }
    
    // 运行测试
    testBpfObjectExists();
    testLibbpfSupport();
    
    if (!testTrafficAnalyzerInit()) {
        std::cout << "\n❌ 流量分析器初始化失败，终止测试" << std::endl;
        g_stats.print();
        return 1;
    }
    
    if (!testInterfaceInit()) {
        std::cout << "\n❌ BPF 程序加载失败，后续测试可能无法进行" << std::endl;
    }
    
    testTrafficSampling();
    testRealTimeStats();
    testAnomalyDetection();
    testTrafficHistory();
    testClearHistory();
    
    // 打印统计
    std::cout << "\n============================================================" << std::endl;
    g_stats.print();
    
    std::cout << "\n🎉 eBPF 功能测试完成!" << std::endl;
    
    return g_stats.failed > 0 ? 1 : 0;
}
