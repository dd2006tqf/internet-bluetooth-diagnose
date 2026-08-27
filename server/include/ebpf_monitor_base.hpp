#pragma once

#include <string>
#include <vector>
#include <memory>

// 前置声明 libbpf 类型
struct bpf_object;
struct bpf_link;
struct bpf_program;

namespace weaknet_dbus {

// eBPF 监控器公共基类
// 提取 6 个 eBPF 监控器的公共初始化/清理逻辑
class EbpfMonitorBase {
public:
    virtual ~EbpfMonitorBase();

    // 公共的 BPF 加载/attach/detach 逻辑
    bool loadBpfObject(const std::string& path);
    bool attachProbe(bpf_program* prog, const std::string& funcName, bool isKretprobe = false);
    void detachAll();

    // 检查是否可用
    bool isAvailable() const { return available_; }

protected:
    // 构造函数保护，只能通过子类实例化
    EbpfMonitorBase() = default;

    // BPF 对象和链接
    std::unique_ptr<bpf_object, void(*)(bpf_object*)> obj_{nullptr, nullptr};
    std::vector<bpf_link*> links_;

    // 状态标志
    bool available_ = false;
    bool initialized_ = false;

    // 辅助方法
    void clearLinks();
};

}  // namespace weaknet_dbus
