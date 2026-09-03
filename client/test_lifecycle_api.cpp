#include "weaknet_client.h"

#include <cstdio>

int main() {
    char buffer[8] = {};
    char error[64] = {};
    if (weaknet_list_monitors(nullptr, 0, error, sizeof(error))) return 1;
    if (weaknet_list_monitors(buffer, sizeof(buffer), nullptr, 0)) return 2;
    if (weaknet_get_monitor_status(nullptr, buffer, sizeof(buffer), error, sizeof(error))) return 3;
    return 0;
}
