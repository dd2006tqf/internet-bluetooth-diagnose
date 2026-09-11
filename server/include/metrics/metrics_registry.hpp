#pragma once

#include "metrics/metric_types.hpp"
#include "metrics/metric_sample.hpp"
#include "metrics/metric_series.hpp"
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

namespace weaknet {

class InterfaceMetrics {
public:
    explicit InterfaceMetrics(std::string iface) : iface_(std::move(iface)) {}

    void publish(MetricId id, const MetricSample& sample) {
        auto* series = getOrCreateSeries(id);
        series->push(sample);
    }

    std::optional<MetricSample> latest(MetricId id) const {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = series_map_.find(id);
        if (it == series_map_.end()) return std::nullopt;
        return it->second->latest();
    }

    std::vector<MetricSample> window(MetricId id, std::chrono::milliseconds duration,
                                     std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        MetricSeries* series = nullptr;
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            auto it = series_map_.find(id);
            if (it != series_map_.end()) {
                series = it->second.get();
            }
        }
        if (!series) return {};
        // 关键：在 Series 自己的细粒度锁保护下 copy 出 window，不阻塞其他指标与 Interface
        return series->window(duration, now);
    }

    const std::string& iface() const { return iface_; }

private:
    MetricSeries* getOrCreateSeries(MetricId id) {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = series_map_.find(id);
        if (it != series_map_.end()) {
            return it->second.get();
        }
        auto series = std::make_unique<MetricSeries>(getMetricDescriptor(id));
        MetricSeries* ptr = series.get();
        series_map_[id] = std::move(series);
        return ptr;
    }

    const std::string iface_;
    mutable std::mutex map_mutex_;
    std::unordered_map<MetricId, std::unique_ptr<MetricSeries>> series_map_;
};

class MetricsRegistry {
public:
    MetricsRegistry() = default;

    void publish(const std::string& iface, MetricId id, const MetricSample& sample) {
        auto* im = getOrCreateInterface(iface);
        im->publish(id, sample);
    }

    // HOST/RESOLVER scoped metrics use explicit namespace keys and never share
    // an interface series (SR-3/SR-4). The binding epoch remains in the sample
    // producer's key discipline; query callers can select the namespace.
    void publishScoped(MetricScope scope, const std::string& address,
                       MetricId id, const MetricSample& sample) {
        publish(scopeKey(scope, address), id, sample);
    }

    std::optional<MetricSample> latest(const std::string& iface, MetricId id) const {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = iface_map_.find(iface);
        if (it == iface_map_.end()) return std::nullopt;
        return it->second->latest(id);
    }

    std::vector<MetricSample> window(const std::string& iface, MetricId id, std::chrono::milliseconds duration,
                                     std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        InterfaceMetrics* im = nullptr;
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = iface_map_.find(iface);
            if (it != iface_map_.end()) {
                im = it->second.get();
            }
        }
        if (!im) return {};
        return im->window(id, duration, now);
    }

    std::vector<std::string> listInterfaces() const {
        std::vector<std::string> res;
        std::lock_guard<std::mutex> lock(registry_mutex_);
        res.reserve(iface_map_.size());
        for (const auto& kv : iface_map_) {
            res.push_back(kv.first);
        }
        return res;
    }

private:
    static std::string scopeKey(MetricScope scope, const std::string& address) {
        switch (scope) {
            case MetricScope::HOST: return "host:" + address;
            case MetricScope::RESOLVER: return "resolver:" + address;
            case MetricScope::GLOBAL: return "global:" + address;
            case MetricScope::INTERFACE:
            default: return address;
        }
    }

    InterfaceMetrics* getOrCreateInterface(const std::string& iface) {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = iface_map_.find(iface);
        if (it != iface_map_.end()) {
            return it->second.get();
        }
        auto im = std::make_unique<InterfaceMetrics>(iface);
        InterfaceMetrics* ptr = im.get();
        iface_map_[iface] = std::move(im);
        return ptr;
    }

    mutable std::mutex registry_mutex_;
    std::unordered_map<std::string, std::unique_ptr<InterfaceMetrics>> iface_map_;
};

} // namespace weaknet
