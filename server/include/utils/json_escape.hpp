#pragma once

#include <string>

namespace weaknet_utils {

// JSON 字符串转义（与 database_manager.cpp 和 net_info.cpp 中的实现一致）
std::string escapeJsonString(const std::string& s);

}  // namespace weaknet_utils
