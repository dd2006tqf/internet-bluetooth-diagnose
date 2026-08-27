#include "ebpf_monitor_base.hpp"
#include "logger.hpp"

#if defined(__has_include)
#  if __has_include(<linux/bpf.h>) && __has_include(<bpf/libbpf.h>) && __has_include(<bpf/bpf.h>)
#    define HAVE_LIBBPF 1
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

namespace weaknet_dbus {

EbpfMonitorBase::~EbpfMonitorBase() {
    detachAll();
}

bool EbpfMonitorBase::loadBpfObject(const std::string& path) {
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "EbpfMonitorBase: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    return false;
#else
    LOG_INFO(LogModule::NETWORK, "EbpfMonitorBase: loading BPF object from " << path);

    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object *obj = bpf_object__open_file(path.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::NETWORK, "EbpfMonitorBase: failed to open BPF object: " << path);
        available_ = false;
        initialized_ = true;
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::NETWORK, "EbpfMonitorBase: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    obj_ = std::unique_ptr<bpf_object, void(*)(bpf_object*)>(obj, bpf_object__close);
    LOG_INFO(LogModule::NETWORK, "EbpfMonitorBase: BPF object loaded successfully");
    return true;
#endif
}

bool EbpfMonitorBase::attachProbe(bpf_program* prog, const std::string& funcName, bool isKretprobe) {
#if !HAVE_LIBBPF
    return false;
#else
    if (!prog || !obj_) {
        return false;
    }

    bpf_link* link = bpf_program__attach(prog);
    long err = libbpf_get_error(link);
    if (err) {
        LOG_ERROR(LogModule::NETWORK, "EbpfMonitorBase: attach " << funcName
                  << " failed: " << err << " (errno=" << errno << " " << strerror(errno) << ")");
        return false;
    }

    links_.push_back(link);
    LOG_INFO(LogModule::NETWORK, "EbpfMonitorBase: attached to " << funcName);
    return true;
#endif
}

void EbpfMonitorBase::detachAll() {
#if HAVE_LIBBPF
    for (auto* link : links_) {
        if (link) {
            bpf_link__destroy(link);
        }
    }
    links_.clear();
#endif

    if (obj_) {
        obj_.reset();
    }

    available_ = false;
}

void EbpfMonitorBase::clearLinks() {
#if HAVE_LIBBPF
    for (auto* link : links_) {
        if (link) {
            bpf_link__destroy(link);
        }
    }
    links_.clear();
#endif
}

}  // namespace weaknet_dbus
