// mock_dbus_service.cpp
// DbusService 的最小桩实现，仅供单元测试链接使用。
//
// 设计目的：
//   event_manager.cpp 在 emitEvent() 中调用 DbusService::emitSpecificSignal /
//   emitNetworkQualitySignal，但测试场景下 server_ctx_->service 为 nullptr，
//   这些方法运行时不会被调用。为避免链接整个 dbus_service.cpp（及其大量依赖），
//   此桩文件提供空实现满足链接器符号需求。
//
// 注意：此文件仅用于测试编译链接，不应包含任何业务逻辑。

#include "dbus_service.hpp"

namespace weaknet_dbus {

// 构造函数：仅保存上下文指针（测试中通常为 nullptr）
DbusService::DbusService(ServerContext* ctx) : ctx_(ctx) {}

// 桩实现：直接返回 true，不实际发送 D-Bus 信号
bool DbusService::emitSpecificSignal(const std::string& /*signalName*/,
                                      const std::string& /*message*/,
                                      int32_t /*counter*/) {
    return true;
}

// 桩实现：直接返回 true，不实际发送 D-Bus 信号
bool DbusService::emitNetworkQualitySignal(const std::string& /*message*/,
                                            const std::string& /*details*/,
                                            int32_t /*counter*/) {
    return true;
}

}  // namespace weaknet_dbus
