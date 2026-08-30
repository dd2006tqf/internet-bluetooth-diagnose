/**
 * @file history_query_tool.cpp
 * @brief SQLite 历史监控数据查询命令行工具 — 用于调试/运维，查询 network_history 表
 *
 * 模块职责：
 *   - 独立可执行程序（与服务端分开编译），连接 DatabaseManager 指向的同一个数据库文件
 *   - 支持按接口名、相对时间（--last 1h/30m/7d）或绝对时间范围（--start/--end）查询
 *   - 输出模式：默认表格视图（人类可读）或 --json 原始 JSON（供脚本消费）
 *   - 内置 --info 显示数据库元信息，--cleanup N 天直接触发过期清理
 *
 * 命令行参数：
 *   --iface <name>       指定网卡（默认 wlan0，--all 忽略此项）
 *   --all                查询所有网卡
 *   --last <duration>    相对时间：数字+单位（h/m/d），如 1h、30m、7d
 *   --start / --end      绝对时间范围（ISO8601），与 --last 互斥
 *   --limit <N>          最大返回行数（默认 100）
 *   --info               仅打印数据库元信息后退出
 *   --cleanup <days>     删除超过 N 天的记录后退出
 *   --json               输出原始 JSON 数组
 *   -h / --help          打印用法
 *
 * 依赖：
 *   - database_manager.hpp   复用 DatabaseManager 类，指定 kDatabasePath 常量
 *   - common.hpp             提供 kDatabasePath 常量定义
 */

#include "database_manager.hpp"
#include "common.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>

using namespace weaknet_dbus;

static std::string parseLastTime(const std::string& last) {
    // 解析 "1h", "30m", "7d" 格式
    int value = 0;
    char unit = 'm';

    if (last.size() >= 2) {
        value = std::stoi(last.substr(0, last.size() - 1));
        unit = last.back();
    }

    auto now = std::chrono::system_clock::now();
    switch (unit) {
        case 'h': now -= std::chrono::hours(value); break;
        case 'm': now -= std::chrono::minutes(value); break;
        case 'd': now -= std::chrono::hours(value * 24); break;
        default: now -= std::chrono::minutes(30); break;
    }

    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_r(&time_t_now, &tm_now);
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

static void printUsage() {
    std::cout << "WeakNet 历史监控数据查询工具\n"
              << "\n"
              << "用法:\n"
              << "  --iface <name>       指定网卡 (默认: wlan0)\n"
              << "  --last <duration>    查询最近时间 (1h/30m/7d)\n"
              << "  --start <timestamp>  起始时间 (ISO 8601)\n"
              << "  --end <timestamp>    结束时间 (ISO 8601)\n"
              << "  --limit <N>          最大行数 (默认: 100)\n"
              << "  --all                查询所有网卡\n"
              << "  --info               显示数据库信息\n"
              << "  --cleanup <days>     清理超过 N 天的数据\n"
              << "  --json               输出原始 JSON\n"
              << "\n"
              << "示例:\n"
              << "  ./history_query_tool --iface wlan0 --last 1h\n"
              << "  ./history_query_tool --all --last 30m\n"
              << "  ./history_query_tool --info\n"
              << "  ./history_query_tool --cleanup 7\n";
}

int main(int argc, char* argv[]) {
    std::string iface = "wlan0";
    std::string start_time, end_time, last;
    int limit = 100;
    bool show_info = false;
    int cleanup_days = -1;
    bool json_output = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            iface = argv[++i];
        } else if (strcmp(argv[i], "--last") == 0 && i + 1 < argc) {
            last = argv[++i];
        } else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            start_time = argv[++i];
        } else if (strcmp(argv[i], "--end") == 0 && i + 1 < argc) {
            end_time = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--all") == 0) {
            iface = "";
        } else if (strcmp(argv[i], "--info") == 0) {
            show_info = true;
        } else if (strcmp(argv[i], "--cleanup") == 0 && i + 1 < argc) {
            cleanup_days = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            json_output = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage();
            return 0;
        } else {
            std::cerr << "未知参数: " << argv[i] << "\n";
            printUsage();
            return 1;
        }
    }

    DatabaseManager db(kDatabasePath);
    if (!db.isOpen()) {
        std::cerr << "错误: 无法打开数据库 " << kDatabasePath << "\n";
        std::cerr << "提示: 数据库文件由服务端首次运行时自动创建\n";
        return 1;
    }

    if (show_info) {
        std::cout << db.getDbInfo() << "\n";
        return 0;
    }

    if (cleanup_days >= 0) {
        int deleted = db.cleanup(cleanup_days);
        std::cout << "已清理 " << deleted << " 条超过 " << cleanup_days << " 天的记录\n";
        return 0;
    }

    // 解析 --last 参数
    if (!last.empty() && start_time.empty()) {
        start_time = parseLastTime(last);
    }

    // 查询数据
    std::string result = db.queryHistory(iface, start_time, end_time, limit);

    if (json_output) {
        std::cout << result << "\n";
        return 0;
    }

    // 解析 JSON 并格式化输出（简化版）
    if (result == "[]" || result.empty()) {
        std::cout << "没有查询到数据\n";
        return 0;
    }

    // 计算记录数
    int count = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i] == '{') count++;
    }

    std::cout << "查询到 " << count << " 条记录\n\n";

    // 简单格式化输出（表格）
    printf("%-20s %-8s %6s %8s %6s %6s %-8s %5s\n",
           "时间", "网卡", "RTT", "Jitter", "RSSI", "丢包", "质量", "评分");
    printf("%-20s %-8s %6s %8s %6s %6s %-8s %5s\n",
           "----", "----", "---", "------", "----", "----", "----", "----");

    // 逐行解析 JSON（简化：用字符串查找）
    size_t pos = 0;
    while ((pos = result.find("{", pos)) != std::string::npos) {
        size_t end = result.find("}", pos);
        if (end == std::string::npos) break;

        std::string entry = result.substr(pos + 1, end - pos - 1);

        // 提取字段
        auto extract = [&](const std::string& key) -> std::string {
            std::string search = "\"" + key + "\":";
            size_t k = entry.find(search);
            if (k == std::string::npos) return "";
            k += search.size();
            if (entry[k] == '"') {
                k++;
                size_t q = entry.find("\"", k);
                return entry.substr(k, q - k);
            }
            size_t v = entry.find_first_of(",}", k);
            return entry.substr(k, v - k);
        };

        std::string ts = extract("ts");
        std::string iface_name = extract("iface");
        std::string rtt = extract("rtt_ms");
        std::string jitter = extract("jitter_ms");
        std::string rssi = extract("rssi_dbm");
        std::string tcp_loss = extract("tcp_loss");
        std::string quality = extract("quality");
        std::string score = extract("score");

        // 截取时间戳的最后部分（去掉日期）
        if (ts.size() > 11) ts = ts.substr(11, 8);

        printf("%-20s %-8s %5sms %7sms %5sdB %5s%% %-8s %5s\n",
               ts.c_str(), iface_name.c_str(),
               rtt.c_str(), jitter.c_str(),
               rssi.c_str(), tcp_loss.c_str(),
               quality.c_str(), score.c_str());

        pos = end + 1;
    }

    return 0;
}
