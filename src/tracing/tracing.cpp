#include "tracing/tracing.h"
#include "spdlog/spdlog.h"
#include <sstream>
#include <iomanip>

namespace curp {
namespace tracing {

Span::Span(const std::string& name, SpanKind kind)
    : name_(name), start_time_(std::chrono::steady_clock::now()) {
    valid_ = Tracer::instance().is_enabled();
    if (valid_) {
        spdlog::trace("[TRACE] Span 开始: {} kind={}", name_, to_string(kind));
    }
}

Span::~Span() {
    if (valid_ && !ended_) {
        end();
    }
}

Span::Span(Span&& other) noexcept
    : valid_(other.valid_)
    , ended_(other.ended_)
    , name_(std::move(other.name_))
    , status_(std::move(other.status_))
    , start_time_(other.start_time_) {
    other.valid_ = false;
}

Span& Span::operator=(Span&& other) noexcept {
    if (this != &other) {
        if (valid_ && !ended_) {
            end();
        }
        valid_ = other.valid_;
        ended_ = other.ended_;
        name_ = std::move(other.name_);
        status_ = std::move(other.status_);
        start_time_ = other.start_time_;
        other.valid_ = false;
    }
    return *this;
}

void Span::set_attribute(const std::string& key, const std::string& value) {
    spdlog::trace("[TRACE] Span '{}': {} = {}", name_, key, value);
}

void Span::set_attribute(const std::string& key, int64_t value) {
    spdlog::trace("[TRACE] Span '{}': {} = {}", name_, key, value);
}

void Span::set_attribute(const std::string& key, double value) {
    spdlog::trace("[TRACE] Span '{}': {} = {}", name_, key, value);
}

void Span::set_attribute(const std::string& key, bool value) {
    spdlog::trace("[TRACE] Span '{}': {} = {}", name_, key, value);
}

void Span::add_event(const std::string& name) {
    spdlog::trace("[TRACE] Span '{}': 事件 '{}'", name_, name);
}

void Span::add_event(const std::string& name, const std::string& attr_key, const std::string& attr_value) {
    spdlog::trace("[TRACE] Span '{}': 事件 '{}', {}={}", name_, name, attr_key, attr_value);
}

void Span::set_ok() {
    status_ = "ok";
}

void Span::set_error(const std::string& description) {
    status_ = "error";
    spdlog::trace("[TRACE] Span '{}': 错误 - {}", name_, description);
}

std::string Span::trace_id() const {
    return "";
}

std::string Span::span_id() const {
    return "";
}

int64_t Span::duration_ms() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
}

void Span::end() {
    if (!valid_ || ended_) {
        return;
    }
    
    auto duration = duration_ms();
    spdlog::trace("[TRACE] Span 结束: {}, 耗时={}ms, status={}", name_, duration, status_);
    ended_ = true;
}

// ========== Tracer 实现 ==========

Tracer& Tracer::instance() {
    static Tracer instance;
    return instance;
}

void Tracer::init(const TracingConfig& config) {
    config_ = config;
    enabled_ = config.enabled;
    spdlog::info("[Tracer] 初始化 | enabled={} | service={}", enabled_, config.service_name);
}

void Tracer::shutdown() {
    spdlog::info("[Tracer] 关闭");
}

Span Tracer::start_span(const std::string& name, SpanKind kind) {
    return Span(name, kind);
}

std::string Tracer::current_trace_context() const {
    return "";
}

} // namespace tracing
} // namespace curp