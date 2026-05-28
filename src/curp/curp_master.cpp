#include "curp/curp_master.h"
#include "curp/curp_client.h"
#include "tracing/tracing.h"
#include "spdlog/spdlog.h"
#include <chrono>

namespace curp {

// ========== 构造和工厂方法 ==========

CurpMaster::~CurpMaster() {
    sync_running_.store(false);
    sync_cv_.notify_all();
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
    spdlog::info("[CurpMaster] 销毁 | node_id={} | total_fast={} | total_slow={} | total_recovered={}",
                cur_node_id, total_fast_path_, total_slow_path_, total_recovered_);
}

std::shared_ptr<CurpMaster> CurpMaster::Create(
    std::vector<NodeConfig> cluster_configs,
    std::string log_dir,
    uint64_t cur_node_id) {
    
    auto master = std::shared_ptr<CurpMaster>(
        new CurpMaster(std::move(cluster_configs), std::move(log_dir), cur_node_id));
    
    master->sync_thread_ = std::thread(&CurpMaster::sync_worker, master.get());
    
    spdlog::info("[CurpMaster] 创建完成 | node_id={} | cluster_size={}", 
                cur_node_id, master->cluster_configs.size());
    return master;
}

CurpMaster::CurpMaster(std::vector<NodeConfig> cluster_configs,
                       std::string log_dir,
                       uint64_t cur_node_id)
    : RaftNode(std::move(cluster_configs), std::move(log_dir), cur_node_id) {
    
    spdlog::info("[CurpMaster] 初始化 | node_id={}", cur_node_id);
}

// ========== CURP 核心接口 ==========

CurpMaster::ProposeResult CurpMaster::propose(const CurpOp& op) {
    // 创建追踪 Span
    auto span = tracing::Tracer::instance().start_span("CurpMaster.propose", tracing::SpanKind::SERVER);
    span.set_attribute("rpc_id", static_cast<int64_t>(op.rpc_id));
    span.set_attribute("client_id", static_cast<int64_t>(op.client_id));
    span.set_attribute("key", op.key);
    span.set_attribute("op_type", static_cast<int64_t>(op.type));
    
    if (curp_state_.load() == CurpState::RECOVERING) {
        spdlog::warn("[CurpMaster] PROPOSE_REJECTED | rpc_id={} | reason=recovering", op.rpc_id);
        span.set_error("Master 正在恢复");
        return {false, false, false, "master is recovering"};
    }
    
    std::lock_guard<std::mutex> lock(curp_mtx_);
    
    total_proposes_++;
    span.set_attribute("total_proposes", static_cast<int64_t>(total_proposes_));
    
    if (is_executed(op.rpc_id)) {
        spdlog::info("[CurpMaster] PROPOSE_DUPLICATE | rpc_id={} | key='{}' | already_executed=true",
                    op.rpc_id, op.key);
        span.set_attribute("result", "DUPLICATE");
        span.set_ok();
        return {true, true, false, ""};
    }
    
    bool can_fast_path = can_speculative_execute(op);
    
    if (can_fast_path) {
        // ===== 快速路径 =====
        auto start = std::chrono::steady_clock::now();
        
        mark_unsynced(op);
        auto result = execute_op(op);
        record_executed(op.rpc_id, result);
        
        total_fast_path_++;
        
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        
        spdlog::info("[CurpMaster] PROPOSE_FAST_PATH | rpc_id={} | key='{}' | "
                    "client_id={} | unsynced_count={} | total_fast={} | latency_us={}",
                    op.rpc_id, op.key, op.client_id, unsynced_ops_.size(), 
                    total_fast_path_, duration);
        
        span.set_attribute("result", "FAST_PATH");
        span.set_attribute("unsynced_count", static_cast<int64_t>(unsynced_ops_.size()));
        span.set_attribute("total_fast", static_cast<int64_t>(total_fast_path_));
        span.set_attribute("latency_us", static_cast<int64_t>(duration));
        span.set_ok();
        
        return {true, true, false, ""};
    } else {
        // ===== 慢速路径 =====
        total_slow_path_++;
        
        spdlog::warn("[CurpMaster] PROPOSE_SLOW_PATH | rpc_id={} | key='{}' | "
                    "client_id={} | reason=key_conflict | unsynced_keys={} | total_slow={}",
                    op.rpc_id, op.key, op.client_id, unsynced_keys_.size(), total_slow_path_);
        
        span.set_attribute("result", "SLOW_PATH");
        span.set_attribute("unsynced_keys", static_cast<int64_t>(unsynced_keys_.size()));
        span.set_attribute("total_slow", static_cast<int64_t>(total_slow_path_));
        span.add_event("need_sync");
        span.set_ok();
        
        return {false, false, true, "key conflict"};
    }
}

CurpMaster::ReadResult CurpMaster::read(const std::string& key) {
    auto span = tracing::Tracer::instance().start_span("CurpMaster.read", tracing::SpanKind::SERVER);
    span.set_attribute("key", key);
    
    if (curp_state_.load() == CurpState::RECOVERING) {
        spdlog::warn("[CurpMaster] READ_REJECTED | key='{}' | reason=recovering", key);
        span.set_error("Master 正在恢复");
        return {false, {}};
    }
    
    std::lock_guard<std::mutex> lock(curp_mtx_);
    
    if (unsynced_keys_.find(key) != unsynced_keys_.end()) {
        spdlog::warn("[CurpMaster] READ_CONFLICT | key='{}' | reason=unsynced_key", key);
        span.set_attribute("result", "CONFLICT");
        span.set_error("key 未同步");
        return {false, {}};
    }
    
    spdlog::info("[CurpMaster] READ_SUCCESS | key='{}'", key);
    span.set_attribute("result", "SUCCESS");
    span.set_ok();
    
    return {true, {}};
}

void CurpMaster::sync() {
    auto span = tracing::Tracer::instance().start_span("CurpMaster.sync", tracing::SpanKind::SERVER);
    
    std::lock_guard<std::mutex> lock(curp_mtx_);
    
    if (unsynced_ops_.empty()) {
        spdlog::debug("[CurpMaster] SYNC_SKIP | reason=no_unsynced_ops");
        span.set_attribute("result", "SKIPPED");
        span.set_ok();
        return;
    }
    
    spdlog::info("[CurpMaster] SYNC_TRIGGERED | unsynced_count={}", unsynced_ops_.size());
    span.set_attribute("unsynced_count", static_cast<int64_t>(unsynced_ops_.size()));
    
    sync_cv_.notify_one();
    span.set_ok();
}

// ========== Witness 管理 ==========

void CurpMaster::set_witness_clients(std::vector<std::shared_ptr<WitnessClient>> clients) {
    std::lock_guard<std::mutex> lock(curp_mtx_);
    witness_clients_ = std::move(clients);
    spdlog::info("[CurpMaster] WITNESS_CLIENTS_SET | count={}", witness_clients_.size());
}

void CurpMaster::set_local_witness(std::shared_ptr<Witness> witness) {
    std::lock_guard<std::mutex> lock(curp_mtx_);
    local_witness_ = witness;
    spdlog::info("[CurpMaster] LOCAL_WITNESS_SET | has_witness=true");
}

// ========== 恢复机制 ==========

void CurpMaster::recover() {
    auto span = tracing::Tracer::instance().start_span("CurpMaster.recover", tracing::SpanKind::INTERNAL);
    auto start_time = std::chrono::steady_clock::now();
    
    spdlog::info("[CurpMaster] RECOVER_START | node_id={} | current_state={}", 
                cur_node_id, static_cast<int>(curp_state_.load()));
    
    curp_state_.store(CurpState::RECOVERING);
    span.add_event("state_recovering");
    
    recover_from_backup();
    
    size_t replayed_count = 0;
    if (local_witness_) {
        replayed_count = replay_from_local_witness();
    } else if (!witness_clients_.empty()) {
        for (auto& client : witness_clients_) {
            try {
                replayed_count = replay_from_witness(client);
                break;
            } catch (const std::exception& e) {
                spdlog::warn("[CurpMaster] REPLAY_WITNESS_FAILED | error={}", e.what());
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(curp_mtx_);
        if (!unsynced_ops_.empty()) {
            do_sync();
        }
    }
    
    curp_state_.store(CurpState::NORMAL);
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    spdlog::info("[CurpMaster] RECOVER_COMPLETE | node_id={} | recovered={} | "
                "replayed={} | duration_ms={}",
                cur_node_id, total_recovered_, replayed_count, duration);
    
    span.set_attribute("recovered", static_cast<int64_t>(total_recovered_));
    span.set_attribute("replayed", static_cast<int64_t>(replayed_count));
    span.set_attribute("duration_ms", static_cast<int64_t>(duration));
    span.set_ok();
}

void CurpMaster::recover_from_backup() {
    spdlog::info("[CurpMaster] RECOVER_FROM_BACKUP | node_id={}", cur_node_id);
    // 从 Raft log 恢复有序数据
}

size_t CurpMaster::replay_from_witness(std::shared_ptr<WitnessClient> witness_client) {
    spdlog::info("[CurpMaster] REPLAY_FROM_REMOTE_WITNESS | node_id={}", cur_node_id);
    
    auto records = witness_client->get_recovery_data(std::to_string(cur_node_id));
    
    spdlog::info("[CurpMaster] REPLAY_RECORDS | count={}", records.size());
    
    replay_ops(records);
    return records.size();
}

size_t CurpMaster::replay_from_local_witness() {
    spdlog::info("[CurpMaster] REPLAY_FROM_LOCAL_WITNESS | node_id={}", cur_node_id);
    
    auto records = local_witness_->get_recovery_data(std::to_string(cur_node_id));
    
    spdlog::info("[CurpMaster] REPLAY_RECORDS | count={}", records.size());
    
    replay_ops(records);
    return records.size();
}

void CurpMaster::replay_ops(const std::vector<WitnessRecord>& records) {
    std::lock_guard<std::mutex> lock(curp_mtx_);
    
    size_t skipped = 0;
    size_t executed = 0;
    
    for (const auto& record : records) {
        if (is_executed(record.rpc_id)) {
            skipped++;
            continue;
        }
        
        CurpOp op;
        op.rpc_id = record.rpc_id;
        op.client_id = record.client_id;
        op.key = record.key;
        op.value = record.request_data;
        op.type = OpType::PUT;
        
        auto result = execute_op(op);
        record_executed(op.rpc_id, result);
        mark_unsynced(op);
        
        executed++;
        total_recovered_++;
    }
    
    spdlog::info("[CurpMaster] REPLAY_COMPLETE | total={} | skipped={} | executed={} | "
                "total_recovered={}",
                records.size(), skipped, executed, total_recovered_);
}

// ========== 状态查询 ==========

CurpState CurpMaster::get_curp_state() const {
    return curp_state_.load();
}

size_t CurpMaster::unsynced_count() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(curp_mtx_));
    return unsynced_ops_.size();
}

bool CurpMaster::has_unsynced_key(const std::string& key) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(curp_mtx_));
    return unsynced_keys_.find(key) != unsynced_keys_.end();
}

bool CurpMaster::is_recovering() const {
    return curp_state_.load() == CurpState::RECOVERING;
}

// ========== 内部方法 ==========

bool CurpMaster::can_speculative_execute(const CurpOp& op) {
    if (op.type == OpType::PUT || op.type == OpType::DELETE_OP) {
        return unsynced_keys_.find(op.key) == unsynced_keys_.end();
    }
    
    if (op.type == OpType::GET) {
        return unsynced_keys_.find(op.key) == unsynced_keys_.end();
    }
    
    return true;
}

void CurpMaster::mark_unsynced(const CurpOp& op) {
    unsynced_keys_.insert(op.key);
    unsynced_ops_[op.rpc_id] = op;
}

std::vector<uint8_t> CurpMaster::execute_op(const CurpOp& op) {
    switch (op.type) {
        case OpType::PUT:
            spdlog::debug("[CurpMaster] EXECUTE_PUT | key='{}' | value_size={}", 
                         op.key, op.value.size());
            break;
            
        case OpType::DELETE_OP:
            spdlog::debug("[CurpMaster] EXECUTE_DELETE | key='{}'", op.key);
            break;
            
        case OpType::GET:
            spdlog::debug("[CurpMaster] EXECUTE_GET | key='{}'", op.key);
            break;
    }
    
    return {};
}

void CurpMaster::sync_worker() {
    spdlog::info("[CurpMaster] SYNC_WORKER_START | node_id={}", cur_node_id);
    
    while (sync_running_.load()) {
        std::unique_lock<std::mutex> lock(curp_mtx_);
        
        sync_cv_.wait_for(lock, std::chrono::milliseconds(100));
        
        if (!sync_running_.load()) {
            break;
        }
        
        if (!unsynced_ops_.empty() && !syncing_.load() && 
            curp_state_.load() == CurpState::NORMAL) {
            lock.unlock();
            do_sync();
        }
    }
    
    spdlog::info("[CurpMaster] SYNC_WORKER_STOP | node_id={}", cur_node_id);
}

void CurpMaster::do_sync() {
    auto span = tracing::Tracer::instance().start_span("CurpMaster.do_sync", tracing::SpanKind::INTERNAL);
    auto start_time = std::chrono::steady_clock::now();
    
    std::lock_guard<std::mutex> lock(curp_mtx_);
    
    if (unsynced_ops_.empty()) {
        span.set_attribute("result", "SKIPPED_EMPTY");
        span.set_ok();
        return;
    }
    
    syncing_.store(true);
    
    size_t sync_count = unsynced_ops_.size();
    spdlog::info("[CurpMaster] SYNC_START | node_id={} | sync_count={} | "
                "unsynced_keys={}",
                cur_node_id, sync_count, unsynced_keys_.size());
    
    span.set_attribute("sync_count", static_cast<int64_t>(sync_count));
    span.add_event("sync_started");
    
    // 模拟同步到 Backup（实际应调用 Raft）
    for (const auto& [rpc_id, op] : unsynced_ops_) {
        raft::Entry entry;
        entry.set_seq(rpc_id);
        entry.set_term(currentTerm);
        entry.set_key(op.key);
        if (!op.value.empty()) {
            entry.set_value(op.value.data(), op.value.size());
        }
    }
    
    unsynced_keys_.clear();
    unsynced_ops_.clear();
    last_sync_index_ = log.size() > 0 ? log.size() - 1 : 0;
    
    syncing_.store(false);
    
    // 重置 Witness
    if (local_witness_) {
        local_witness_->reset(std::to_string(cur_node_id));
    }
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    spdlog::info("[CurpMaster] SYNC_COMPLETE | node_id={} | sync_count={} | "
                "last_sync_index={} | duration_ms={} | witness_reset=true",
                cur_node_id, sync_count, last_sync_index_, duration);
    
    span.set_attribute("duration_ms", static_cast<int64_t>(duration));
    span.set_attribute("last_sync_index", static_cast<int64_t>(last_sync_index_));
    span.set_attribute("witness_reset", static_cast<bool>(true));
    span.add_event("sync_completed");
    span.set_ok();
}

bool CurpMaster::is_executed(uint64_t rpc_id) const {
    return executed_ops_.find(rpc_id) != executed_ops_.end();
}

void CurpMaster::record_executed(uint64_t rpc_id, const std::vector<uint8_t>& result) {
    executed_ops_[rpc_id] = result;
}

} // namespace curp