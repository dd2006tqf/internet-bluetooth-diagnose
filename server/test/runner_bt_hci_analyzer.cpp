#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

#include "bt_data_path_analyzer.hpp"
#include "bt_hci_connection_tracker.hpp"

using namespace weaknet_dbus;
using namespace weaknet;

static std::atomic<bool> g_running{true};
void sigint_handler(int) {
    g_running.store(false);
}

int main(int argc, char** argv) {
    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);

    std::string bpf_path = "server/build/bt_hci_traffic.bpf.o";
    if (argc > 1) {
        bpf_path = argv[1];
    }

    std::cout << "====================================================" << std::endl;
    std::cout << "  BtDataPathAnalyzer 板端真机实测运行器" << std::endl;
    std::cout << "  BPF 目标文件: " << bpf_path << std::endl;
    std::cout << "====================================================" << std::endl;

    auto tracker = std::make_shared<BtHciConnectionTracker>();
    // 注入当前已知的 vivo X300s 身份映射（Handle 129 -> D0:96:EA:52:30:AC）
    tracker->onConnectionEstablished("hci0", 129, "D0:96:EA:52:30:AC", "PUBLIC");
    std::cout << "ℹ️  已预设 Tracker 身份映射: hci0 Handle 129 -> D0:96:EA:52:30:AC" << std::endl;

    BtDataPathAnalyzer analyzer(tracker);
    if (!analyzer.init(bpf_path, "hci0")) {
        std::cerr << "❌ BtDataPathAnalyzer 初始化失败: " << analyzer.lastError() << std::endl;
        return 1;
    }

    std::cout << "✅ eBPF 探针挂载成功: " << analyzer.attachedHookName() << std::endl;
    std::cout << "📡 开始周期采样 (每 1 秒一次，按 Ctrl+C 退出)..." << std::endl;

    int tick = 0;
    while (g_running.load() && tick < 10) { // 默认采样 10 轮
        std::this_thread::sleep_for(std::chrono::seconds(1));
        tick++;

        auto obs_list = analyzer.collectObservations();
        std::cout << "\n--- [Tick " << tick << "] 观测项数量: " << obs_list.size() << " ---" << std::endl;

        for (const auto& obs : obs_list) {
            std::cout << "  [Handle " << obs.connection_handle
                      << " (gen " << obs.connection_generation << ")]"
                      << " Scope: " << obs.attribution_scope
                      << " Device: " << (obs.device_id ? *obs.device_id : "N/A")
                      << " | TX: " << obs.tx_packets << " pkts (" << obs.tx_bytes << " B, "
                      << std::fixed << std::setprecision(1) << obs.tx_throughput_bps << " bps)"
                      << " | RX: " << obs.rx_packets << " pkts (" << obs.rx_bytes << " B, "
                      << obs.rx_throughput_bps << " bps)"
                      << " | Gap: " << obs.max_gap_ms << " ms"
                      << " | Status: " << obs.status
                      << std::endl;
            std::cout << "    JSON: " << obs.toJson() << std::endl;
        }
    }

    analyzer.stop();
    std::cout << "\n✅ 实测运行结束，分析器安全退出。" << std::endl;
    return 0;
}
