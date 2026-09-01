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

#include "weaknet_client.h"

static void printUsage(const char* prog) {
    fprintf(stderr,
        "用法：\n"
        "  %s get <monitor>              # 查询监控器参数（JSON）\n"
        "  %s set <key> <value>          # 设置参数（如 rtt.interval 5s）\n"
        "  %s list                       # 列出可用监控器\n"
        "\n"
        "监控器名：rtt, jitter, rssi, tcp_loss, traffic, quality,\n"
        "        bluetooth, dns, wifi_loss, http_latency, process_profiler,\n"
        "        tcp_retrans, tcp_conn, server, all\n"
        "\n"
        "示例：\n"
        "  %s set rtt.interval 5s\n"
        "  %s get rtt\n"
        "  %s list\n", prog, prog, prog, prog, prog, prog);
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
    } else if (strcmp(cmd, "list") == 0) {
        printf("rtt\njitter\nrssi\ntcp_loss\ntraffic\nquality\n"
               "bluetooth\ndns\nwifi_loss\nhttp_latency\nprocess_profiler\n"
               "tcp_retrans\ntcp_conn\nserver\nall\n");
        ok = true;
    } else {
        fprintf(stderr, "未知命令: %s\n", cmd);
        printUsage(argv[0]);
        return 1;
    }

    weaknet_cleanup();
    return ok ? 0 : 1;
}