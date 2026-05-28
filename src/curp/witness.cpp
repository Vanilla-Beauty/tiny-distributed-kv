#include "curp/witness.h"
#include "tracing/tracing.h"
#include "spdlog/spdlog.h"
#include <algorithm>

namespace curp {

Witness::Witness(const std::string& storage_dir) 
    : storage_dir_(storage_dir) {
    spdlog::info("[Witness] 初始化完成 | storage_dir={} | max_records={}", 
                 storage_dir.empty() ? "(内存)" : storage_dir, kMaxRecords);
}

RecordResult Witness::record(const std::string& master_id, uint64_t rpc_id,
                              const std::string& key,
                              const std::vector<uint8_t>& request_data,
                              uint64_t client_id) {
    // 创建追踪 Span
    auto span = tracing::Tracer::instance().start_span("Witness.record", tracing::SpanKind::SERVER);
    span.set_attribute("rpc_id", static_cast<int64_t>(rpc_id));
    span.set_attribute("master_id", master_id);
    span.set_attribute("key", key);
    span.set_attribute("client_id", static_cast<int64_t>(client_id));
    span.set_attribute("request_size", static_cast<int64_t>(request_data.size()));
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    // 检查是否已停止接受
    if (!accepting_.load()) {
        spdlog::warn("[Witness] REJECTED_STOPPED | rpc_id={} | reason=已停止接受", rpc_id);
        total_rejected_++;
        span.set_attribute("result", "REJECTED_STOPPED");
        span.set_error("已停止接受");
        return RecordResult::REJECTED_STOPPED;
    }
    
    // 检查 Master ID（如果已绑定）
    if (!master_id_.empty() && master_id != master_id_) {
        spdlog::warn("[Witness] REJECTED_WRONG_MASTER | rpc_id={} | expected={} | actual={}", 
                    rpc_id, master_id_, master_id);
        total_rejected_++;
        span.set_attribute("result", "REJECTED_WRONG_MASTER");
        span.set_error("Master 不匹配");
        return RecordResult::REJECTED_WRONG_MASTER;
    }
    
    // 检查空间
    if (records_.size() >= kMaxRecords) {
        spdlog::warn("[Witness] REJECTED_NO_SPACE | rpc_id={} | current={} | max={}", 
                    rpc_id, records_.size(), kMaxRecords);
        total_rejected_++;
        span.set_attribute("result", "REJECTED_NO_SPACE");
        span.set_error("空间不足");
        return RecordResult::REJECTED_NO_SPACE;
    }
    
    // 检查 RPC ID 是否重复
    if (rpc_ids_.find(rpc_id) != rpc_ids_.end()) {
        spdlog::debug("[Witness] ACCEPTED (duplicate) | rpc_id={} | reason=已存在，忽略重复", rpc_id);
        span.set_attribute("result", "ACCEPTED_DUPLICATE");
        span.set_ok();
        return RecordResult::ACCEPTED;
    }
    
    // 检查可交换性：同一个 key 只能有一个未同步的请求
    if (records_.find(key) != records_.end()) {
        const auto& existing = records_[key];
        spdlog::warn("[Witness] REJECTED_NOT_COMMUTATIVE | rpc_id={} | key='{}' | "
                    "conflict_with_rpc_id={}", rpc_id, key, existing.rpc_id);
        total_rejected_++;
        span.set_attribute("result", "REJECTED_NOT_COMMUTATIVE");
        span.set_attribute("conflict_rpc_id", static_cast<int64_t>(existing.rpc_id));
        span.set_error("key 冲突");
        return RecordResult::REJECTED_NOT_COMMUTATIVE;
    }
    
    // ===== 成功记录 =====
    
    WitnessRecord record(rpc_id, key, request_data, client_id);
    records_[key] = record;
    rpc_ids_.insert(rpc_id);
    
    // 绑定 Master ID（首次记录）
    if (master_id_.empty()) {
        master_id_ = master_id;
        spdlog::info("[Witness] MASTER_BOUND | master_id={}", master_id_);
        span.add_event("master_bound", "master_id", master_id_);
    }
    
    total_recorded_++;
    
    spdlog::info("[Witness] ACCEPTED | rpc_id={} | key='{}' | client_id={} | "
                "records_count={} | total_recorded={} | total_rejected={}",
                rpc_id, key, client_id, records_.size(), total_recorded_, total_rejected_);
    
    span.set_attribute("result", "ACCEPTED");
    span.set_attribute("records_count", static_cast<int64_t>(records_.size()));
    span.set_attribute("total_recorded", static_cast<int64_t>(total_recorded_));
    span.set_ok();
    
    return RecordResult::ACCEPTED;
}

uint32_t Witness::garbage_collect(const std::string& master_id,
                                   const std::vector<GCEntry>& entries) {
    auto span = tracing::Tracer::instance().start_span("Witness.garbage_collect", tracing::SpanKind::SERVER);
    span.set_attribute("master_id", master_id);
    span.set_attribute("entries_count", static_cast<int64_t>(entries.size()));
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    // 验证 Master ID
    if (!master_id_.empty() && master_id != master_id_) {
        spdlog::warn("[Witness] GC_REJECTED | expected_master={} | actual_master={}", 
                    master_id_, master_id);
        span.set_error("Master 不匹配");
        return 0;
    }
    
    uint32_t dropped = 0;
    std::vector<uint64_t> stale_rpc_ids;
    
    for (const auto& entry : entries) {
        auto it = records_.find(entry.key);
        if (it != records_.end() && it->second.rpc_id == entry.rpc_id) {
            rpc_ids_.erase(it->second.rpc_id);
            records_.erase(it);
            dropped++;
        }
    }
    
    total_gc_ += dropped;
    
    if (dropped > 0) {
        spdlog::info("[Witness] GC_COMPLETED | dropped={} | remaining={} | total_gc={}", 
                    dropped, records_.size(), total_gc_);
    } else {
        spdlog::debug("[Witness] GC_COMPLETED | dropped=0 | remaining={} | total_gc={}", 
                    records_.size(), total_gc_);
    }
    
    span.set_attribute("dropped", static_cast<int64_t>(dropped));
    span.set_attribute("remaining", static_cast<int64_t>(records_.size()));
    span.set_attribute("total_gc", static_cast<int64_t>(total_gc_));
    span.set_ok();
    
    return dropped;
}

std::vector<WitnessRecord> Witness::get_recovery_data(const std::string& master_id) {
    auto span = tracing::Tracer::instance().start_span("Witness.get_recovery_data", tracing::SpanKind::SERVER);
    span.set_attribute("master_id", master_id);
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    // 验证 Master ID
    if (!master_id_.empty() && master_id != master_id_) {
        spdlog::error("[Witness] RECOVERY_REJECTED | expected_master={} | actual_master={}", 
                     master_id_, master_id);
        span.set_error("Master 不匹配");
        return {};
    }
    
    // 停止接受新请求
    accepting_.store(false);
    
    std::vector<WitnessRecord> result;
    result.reserve(records_.size());
    
    for (const auto& [key, record] : records_) {
        result.push_back(record);
    }
    
    spdlog::info("[Witness] RECOVERY_MODE | records_returned={} | accepting=false | "
                "total_recorded={} | total_rejected={} | total_gc={}",
                result.size(), total_recorded_, total_rejected_, total_gc_);
    
    span.set_attribute("records_returned", static_cast<int64_t>(result.size()));
    span.set_attribute("accepting", static_cast<bool>(false));
    span.set_ok();
    
    return result;
}

void Witness::stop_accepting() {
    auto span = tracing::Tracer::instance().start_span("Witness.stop_accepting", tracing::SpanKind::INTERNAL);
    
    accepting_.store(false);
    
    spdlog::info("[Witness] STOP_ACCEPTING | accepting=false | records_count={}", 
                records_.size());
    
    span.set_attribute("accepting", static_cast<bool>(false));
    span.set_attribute("records_count", static_cast<int64_t>(records_.size()));
    span.set_ok();
}

void Witness::reset(const std::string& new_master_id) {
    auto span = tracing::Tracer::instance().start_span("Witness.reset", tracing::SpanKind::INTERNAL);
    span.set_attribute("new_master_id", new_master_id);
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    size_t old_count = records_.size();
    uint64_t old_recorded = total_recorded_;
    uint64_t old_rejected = total_rejected_;
    uint64_t old_gc = total_gc_;
    
    records_.clear();
    rpc_ids_.clear();
    master_id_ = new_master_id;
    accepting_.store(true);
    
    spdlog::info("[Witness] RESET | new_master={} | cleared={} | "
                "old_recorded={} | old_rejected={} | old_gc={} | accepting=true",
                new_master_id.empty() ? "(未绑定)" : new_master_id, old_count,
                old_recorded, old_rejected, old_gc);
    
    span.set_attribute("cleared", static_cast<int64_t>(old_count));
    span.set_attribute("accepting", static_cast<bool>(true));
    span.set_ok();
}

Witness::Status Witness::get_status() const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    Status status;
    status.master_id = master_id_;
    status.accepting = accepting_.load();
    status.request_count = static_cast<uint32_t>(records_.size());
    
    if (!records_.empty()) {
        uint64_t oldest = UINT64_MAX;
        for (const auto& [key, record] : records_) {
            if (record.timestamp < oldest) {
                oldest = record.timestamp;
            }
        }
        status.oldest_timestamp = oldest;
    }
    
    return status;
}

bool Witness::has_key_conflict(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mtx_);
    bool has_conflict = records_.find(key) != records_.end();
    
    if (has_conflict) {
        spdlog::debug("[Witness] KEY_HAS_CONFLICT | key='{}' | records_count={}", 
                     key, records_.size());
    }
    
    return has_conflict;
}

size_t Witness::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return records_.size();
}

} // namespace curp