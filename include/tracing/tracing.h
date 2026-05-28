#pragma once

/**
 * @file tracing.h
 * @brief OpenTelemetry 集成 - 分布式追踪支持（轻量级版本）
 */

#include <string>
#include <memory>
#include <chrono>

namespace curp {
namespace tracing {

struct TracingConfig {
    bool enabled{false};
    std::string service_name{"curp-node"};
    std::string exporter_endpoint{"http://localhost:14268/api/traces"};
};

enum class SpanKind {
    INTERNAL, SERVER, CLIENT, PRODUCER, CONSUMER
};

/**
 * @brief Span 包装器（轻量级，不依赖 OpenTelemetry）
 */
class Span {
public:
    Span(const std::string& name, SpanKind kind = SpanKind::INTERNAL);
    ~Span();
    
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    Span(Span&& other) noexcept;
    Span& operator=(Span&& other) noexcept;
    
    // 统一使用 int64_t 和 string，避免 uint64_t 歧义
    void set_attribute(const std::string& key, const std::string& value);
    void set_attribute(const std::string& key, int64_t value);
    void set_attribute(const std::string& key, double value);
    void set_attribute(const std::string& key, bool value);
    
    void add_event(const std::string& name);
    void add_event(const std::string& name, const std::string& attr_key, const std::string& attr_value);
    
    void set_ok();
    void set_error(const std::string& description);
    
    std::string trace_id() const;
    std::string span_id() const;
    int64_t duration_ms() const;
    void end();
    
    explicit operator bool() const { return valid_; }
    
private:
    bool valid_{false};
    bool ended_{false};
    std::string name_;
    std::string status_{"ok"};
    std::chrono::steady_clock::time_point start_time_;
};

class Tracer {
public:
    static Tracer& instance();
    void init(const TracingConfig& config);
    void shutdown();
    Span start_span(const std::string& name, SpanKind kind = SpanKind::INTERNAL);
    std::string current_trace_context() const;
    bool is_enabled() const { return enabled_; }
    
private:
    Tracer() = default;
    bool enabled_{false};
    TracingConfig config_;
};

inline std::string to_string(SpanKind kind) {
    switch (kind) {
        case SpanKind::INTERNAL: return "internal";
        case SpanKind::SERVER:   return "server";
        case SpanKind::CLIENT:   return "client";
        case SpanKind::PRODUCER: return "producer";
        case SpanKind::CONSUMER: return "consumer";
        default: return "unknown";
    }
}

} // namespace tracing
} // namespace curp