/**
 * @file weaknet_cli.cpp
 * @brief weaknet-cli 运行时配置命令行工具
 *
 * 薄封装 D-Bus 调用 SetMonitorParam / GetMonitorParam，
 * 所有业务逻辑（校验、原子提交、持久化）在服务端实现。
 *
 * 用法：
 *   weaknet-cli get <monitor>              # 查询监控器参数（JSON）
 *   weaknet-cli set <key> <value>          # 设置参数
 *   weaknet-cli list                       # 列出可用监控器
 *
 * 示例：
 *   weaknet-cli set rtt.interval 5s
 *   weaknet-cli get rtt
 *   weaknet-cli get all
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "weaknet_client.h"

static void printUsage(const char* prog) {
    fprintf(stderr,
        "用法：\n"
        "  %s get <monitor>              # 查询监控器参数（JSON）\n"
        "  %s set <key> <value>          # 设置参数（如 rtt.interval 5s）\n"
        "  %s list                       # 列出可用监控器\n"
        "  %s monitor list|status [name]\n"
        "  %s monitor enable|disable|restart <name>\n"
        "  %s monitor save                # 保存运行时启停状态\n"
        "  %s events [--device <addr>] [--type <TYPE>]\n"
        "         [--start <ms>] [--end <ms>] [--limit <N>]\n"
        "                                 # 查询无线设备事件（JSON，只读）\n"
        "  %s incidents [--state <STATE>]\n"
        "         [--start <ms>] [--end <ms>] [--limit <N>]\n"
        "                                 # 查询区域级异常事件 SiteIncident（JSON，只读）\n"
        "\n"
        "事件类型（--type）：LINK_DISCONNECTED, LINK_DEGRADED, DEVICE_APPEARED, ...\n"
        "区域事件状态（--state）：OPEN, ONGOING, RESOLVED\n"
        "时间均为 Unix 毫秒，0 或省略表示不限；limit 默认 100，上限 10000\n"
        "\n"
        "监控器名：rtt, rssi, tcp_loss, traffic, quality,\n"
        "        bluetooth, dns, wifi_loss, http_latency, process_profiler,\n"
        "        tcp_retrans, tcp_conn, skb_drop, tcp_connect,\n"
        "        active_probe, edge, server, all\n"
        "\n"
        "示例：\n"
        "  %s set rtt.interval 5s\n"
        "  %s get rtt\n"
        "  %s list\n", prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

static bool callSet(const char* key, const char* value) {
    char err[256];
    if (!weaknet_set_monitor_param(key, value, err, sizeof(err))) {
        fprintf(stderr, "Set failed: %s\n", err);
        return false;
    }
    printf("ok\n");
    return true;
}

/// weaknet-cli events：查询规范化无线设备事件（只读）
/// 过滤参数全部可选；默认 limit=100（服务端上限 10000）
static bool callEvents(int argc, char** argv) {
    std::string device, type;
    int64_t start_ms = 0, end_ms = 0;
    int32_t limit = 100;

    for (int i = 2; i < argc; ++i) {
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "缺少 %s 的值\n", opt);
                return nullptr;
            }
            return argv[++i];
        };
        if (strcmp(argv[i], "--device") == 0) {
            const char* v = need("--device"); if (!v) return false;
            device = v;
        } else if (strcmp(argv[i], "--type") == 0) {
            const char* v = need("--type"); if (!v) return false;
            type = v;
        } else if (strcmp(argv[i], "--start") == 0) {
            const char* v = need("--start"); if (!v) return false;
            start_ms = strtoll(v, nullptr, 10);
        } else if (strcmp(argv[i], "--end") == 0) {
            const char* v = need("--end"); if (!v) return false;
            end_ms = strtoll(v, nullptr, 10);
        } else if (strcmp(argv[i], "--limit") == 0) {
            const char* v = need("--limit"); if (!v) return false;
            limit = static_cast<int32_t>(strtol(v, nullptr, 10));
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            return false;
        }
    }

    // 上限 64KiB：limit 上限 10000 × 单条约 500B 最坏情况超出 64K，
    // 超出部分由服务端 limit 钳制约束（10000 条最坏约 5MB）——
    // 这里给 1MB 并提示用 --limit 收窄，避免栈上大数组
    std::vector<char> buf(1 << 20);
    char err[256];
    if (!weaknet_query_device_events(device.c_str(), type.c_str(),
                                     start_ms, end_ms, limit,
                                     buf.data(), buf.size(),
                                     err, sizeof(err))) {
        fprintf(stderr, "事件查询失败: %s\n", err);
        return false;
    }
    printf("%s\n", buf.data());
    return true;
}

/// weaknet-cli incidents：查询区域级异常事件（只读，Phase 3a）
/// 过滤参数全部可选；默认 limit=100（服务端上限 10000）
static bool callIncidents(int argc, char** argv) {
    std::string state;
    int64_t start_ms = 0, end_ms = 0;
    int32_t limit = 100;

    for (int i = 2; i < argc; ++i) {
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "缺少 %s 的值\n", opt);
                return nullptr;
            }
            return argv[++i];
        };
        if (strcmp(argv[i], "--state") == 0) {
            const char* v = need("--state"); if (!v) return false;
            state = v;
        } else if (strcmp(argv[i], "--start") == 0) {
            const char* v = need("--start"); if (!v) return false;
            start_ms = strtoll(v, nullptr, 10);
        } else if (strcmp(argv[i], "--end") == 0) {
            const char* v = need("--end"); if (!v) return false;
            end_ms = strtoll(v, nullptr, 10);
        } else if (strcmp(argv[i], "--limit") == 0) {
            const char* v = need("--limit"); if (!v) return false;
            limit = static_cast<int32_t>(strtol(v, nullptr, 10));
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            return false;
        }
    }

    // 与 callEvents 同一理由的 1MB 缓冲：区域事件单条含受影响设备清单，
    // 比设备事件更大；超出部分由服务端 limit 钳制约束。
    std::vector<char> buf(1 << 20);
    char err[256];
    if (!weaknet_query_site_incidents(state.c_str(), start_ms, end_ms, limit,
                                      buf.data(), buf.size(), err, sizeof(err))) {
        fprintf(stderr, "区域事件查询失败: %s\n", err);
        return false;
    }
    printf("%s\n", buf.data());
    return true;
}

static bool callLifecycle(const char* operation, const char* monitor) {
    char buf[8192];
    char err[256];
    bool ok = false;
    if (strcmp(operation, "status") == 0) {
        ok = monitor ? weaknet_get_monitor_status(monitor, buf, sizeof(buf), err, sizeof(err))
                     : weaknet_list_monitors(buf, sizeof(buf), err, sizeof(err));
    } else if (strcmp(operation, "enable") == 0) {
        ok = weaknet_enable_monitor(monitor, buf, sizeof(buf), err, sizeof(err));
    } else if (strcmp(operation, "disable") == 0) {
        ok = weaknet_disable_monitor(monitor, buf, sizeof(buf), err, sizeof(err));
    } else if (strcmp(operation, "restart") == 0) {
        ok = weaknet_restart_monitor(monitor, buf, sizeof(buf), err, sizeof(err));
    }
    if (ok) printf("%s\n", buf);
    else fprintf(stderr, "Monitor operation failed: %s\n", err);
    return ok;
}

static bool callGet(const char* monitor) {
    char buf[8192];
    char err[256];
    if (!weaknet_get_monitor_param(monitor, buf, sizeof(buf), err, sizeof(err))) {
        fprintf(stderr, "Get failed: %s\n", err);
        return false;
    }
    printf("%s\n", buf);
    return true;
}

static bool callSaveOverrides() {
    char buf[8192];
    char err[256];
    if (!weaknet_save_monitor_overrides(buf, sizeof(buf), err, sizeof(err))) {
        fprintf(stderr, "Save monitor overrides failed: %s\n", err);
        return false;
    }
    printf("%s\n", buf);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // 初始化客户端
    if (!weaknet_init()) {
        fprintf(stderr, "weaknet_init 失败\n");
        return 1;
    }

    const char* cmd = argv[1];
    bool ok = false;

    if (strcmp(cmd, "get") == 0) {
        if (argc != 3) {
            printUsage(argv[0]);
            return 1;
        }
        ok = callGet(argv[2]);
    } else if (strcmp(cmd, "set") == 0) {
        if (argc != 4) {
            printUsage(argv[0]);
            return 1;
        }
        ok = callSet(argv[2], argv[3]);
    } else if (strcmp(cmd, "monitor") == 0) {
        if (argc < 3 || argc > 4) {
            printUsage(argv[0]);
            return 1;
        }
        const char* operation = argv[2];
        const char* monitor = argc == 4 ? argv[3] : nullptr;
        if (strcmp(operation, "list") == 0 || strcmp(operation, "status") == 0) {
            if (strcmp(operation, "list") == 0 && monitor) {
                printUsage(argv[0]);
                return 1;
            }
            ok = callLifecycle(strcmp(operation, "list") == 0 ? "status" : operation, monitor);
        } else if (strcmp(operation, "save") == 0 && !monitor) {
            ok = callSaveOverrides();
        } else if ((strcmp(operation, "enable") == 0 || strcmp(operation, "disable") == 0 ||
                    strcmp(operation, "restart") == 0) && monitor) {
            ok = callLifecycle(operation, monitor);
        } else {
            printUsage(argv[0]);
            return 1;
        }
    } else if (strcmp(cmd, "events") == 0) {
        ok = callEvents(argc, argv);
    } else if (strcmp(cmd, "incidents") == 0) {
        ok = callIncidents(argc, argv);
    } else if (strcmp(cmd, "list") == 0) {
        // 必须与 serializers 端的有效名集合保持一致（weaknet_config.cpp 的
        // serializeMonitorJson）。此前这里漏了 skb_drop / tcp_connect /
        // active_probe / edge——它们可以被 `set` 但 `list` 不显示，用户无从发现。
        printf("rtt\nrssi\ntcp_loss\ntraffic\nquality\n"
               "bluetooth\ndns\nwifi_loss\nhttp_latency\nprocess_profiler\n"
               "tcp_retrans\ntcp_conn\nskb_drop\ntcp_connect\n"
               "active_probe\nedge\nserver\nall\n");
        ok = true;
    } else {
        fprintf(stderr, "未知命令: %s\n", cmd);
        printUsage(argv[0]);
        return 1;
    }

    weaknet_cleanup();
    return ok ? 0 : 1;
}