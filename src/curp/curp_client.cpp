#include "curp/curp_client.h"
#include "curp/witness.h"
#include "tracing/tracing.h"
#include "spdlog/spdlog.h"
#include <future>
#include <chrono>

namespace curp {

// ========== CurpClient ==========

CurpClient::CurpClient(const CurpClientConfig& config)
    : config_(config) {
    
    // 创建追踪 Span
    auto span = tracing::Tracer::instance().start_span("CurpClient.init", tracing::SpanKind::INTERNAL);
    
    master_client_ = std::make_unique<CurpMasterClient>(config.master_addr);
    for (const auto& addr : config.witness_addrs) {
        witness_clients_.push_back(std::make_shared<WitnessClient>(addr));
    }
    
    spdlog::info("[CurpClient] INIT_COMPLETE | client_id={} | master={} | witness_count={} | "
                "timeout_ms={} | retry_count={}",
                config_.client_id, config_.master_addr, witness_clients_.size(),
                config_.timeout_ms, 0);
    
    span.set_attribute("client_id", static_cast<int64_t>(config_.client_id));
    span.set_attribute("master_addr", config_.master_addr);
    span.set_attribute("witness_count", static_cast<int64_t>(witness_clients_.size()));
    span.set_ok();
}

CurpClient::~CurpClient() {
    spdlog::info("[CurpClient] DESTROY | client_id={} | total_writes={} | total_reads={} | "
                "fast_path={} | slow_path={} | failed={}",
                config_.client_id, total_writes_, total_reads_, 
                total_fast_path_, total_slow_path_, total_failed_);
}

uint64_t CurpClient::next_rpc_id() {
    std::lock_guard<std::mutex> lock(mtx_);
    uint64_t rpc_id = (config_.client_id << 48) | (next_rpc_id_++);
    
    spdlog::debug("[CurpClient] RPC_ID_GENERATED | client_id={} | rpc_id={} | seq={}",
                 config_.client_id, rpc_id, next_rpc_id_ - 1);
    
    return rpc_id;
}

void CurpClient::reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    next_rpc_id_ = 1;
    
    spdlog::info("[CurpClient] RESET | client_id={} | next_rpc_id=1", config_.client_id);
}

CurpClient::WriteResult CurpClient::write(const std::string& key, 
                                          const std::vector<uint8_t>& value) {
    // 创建追踪 Span
    auto span = tracing::Tracer::instance().start_span("CurpClient.write", tracing::SpanKind::CLIENT);
    span.set_attribute("key", key);
    span.set_attribute("value_size", static_cast<int64_t>(value.size()));
    
    auto start_time = std::chrono::steady_clock::now();
    
    CurpOp op;
    op.rpc_id = next_rpc_id();
    op.client_id = config_.client_id;
    op.type = OpType::PUT;
    op.key = key;
    op.value = value;
    
    span.set_attribute("rpc_id", static_cast<int64_t>(op.rpc_id));
    span.set_attribute("client_id", static_cast<int64_t>(op.client_id));
    
    spdlog::info("[CurpClient] WRITE_START | rpc_id={} | key='{}' | value_size={} | client_id={}",
                op.rpc_id, key, value.size(), config_.client_id);
    
    auto result = parallel_write(op);
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    // 统计
    total_writes_++;
    if (result.success) {
        if (result.fast_path) {
            total_fast_path_++;
        } else {
            total_slow_path_++;
        }
    } else {
        total_failed_++;
    }
    
    if (result.success) {
        spdlog::info("[CurpClient] WRITE_SUCCESS | rpc_id={} | key='{}' | "
                    "fast_path={} | rtt_count={} | latency_ms={} | "
                    "total_fast={} | total_slow={} | total_failed={}",
                    op.rpc_id, key, result.fast_path, result.rtt_count, duration,
                    total_fast_path_, total_slow_path_, total_failed_);
        
        span.set_attribute("result", "SUCCESS");
        span.set_attribute("fast_path", static_cast<bool>(result.fast_path));
        span.set_attribute("rtt_count", static_cast<int64_t>(result.rtt_count));
        span.set_attribute("latency_ms", static_cast<int64_t>(duration));
        span.set_ok();
    } else {
        spdlog::warn("[CurpClient] WRITE_FAILED | rpc_id={} | key='{}' | "
                    "error='{}' | latency_ms={} | total_failed={}",
                    op.rpc_id, key, result.error, duration, total_failed_);
        
        span.set_attribute("result", "FAILED");
        span.set_attribute("error", result.error);
        span.set_attribute("latency_ms", static_cast<int64_t>(duration));
        span.set_error(result.error);
    }
    
    return result;
}

CurpClient::WriteResult CurpClient::deleteOp(const std::string& key) {
    auto span = tracing::Tracer::instance().start_span("CurpClient.delete", tracing::SpanKind::CLIENT);
    span.set_attribute("key", key);
    
    auto start_time = std::chrono::steady_clock::now();
    
    CurpOp op;
    op.rpc_id = next_rpc_id();
    op.client_id = config_.client_id;
    op.type = OpType::DELETE_OP;
    op.key = key;
    
    span.set_attribute("rpc_id", static_cast<int64_t>(op.rpc_id));
    
    spdlog::info("[CurpClient] DELETE_START | rpc_id={} | key='{}'", op.rpc_id, key);
    
    auto result = parallel_write(op);
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    total_writes_++;
    if (!result.success) {
        total_failed_++;
    }
    
    spdlog::info("[CurpClient] DELETE_COMPLETE | rpc_id={} | key='{}' | success={} | "
                "latency_ms={}", op.rpc_id, key, result.success, duration);
    
    span.set_attribute("success", static_cast<bool>(result.success));
    span.set_attribute("latency_ms", static_cast<int64_t>(duration));
    if (result.success) {
        span.set_ok();
    } else {
        span.set_error(result.error);
    }
    
    return result;
}

CurpClient::WriteResult CurpClient::parallel_write(const CurpOp& op) {
    auto span = tracing::Tracer::instance().start_span("CurpClient.parallel_write", tracing::SpanKind::INTERNAL);
    span.set_attribute("rpc_id", static_cast<int64_t>(op.rpc_id));
    span.set_attribute("key", op.key);
    
    spdlog::debug("[CurpClient] PARALLEL_WRITE_START | rpc_id={} | key='{}' | "
                 "master_addr={} | witness_count={}",
                 op.rpc_id, op.key, config_.master_addr, witness_clients_.size());
    
    auto request_data = serialize_request(op);
    
    // 并行发送到 Master 和 Witness
    auto master_future = std::async(std::launch::async, [&]() {
        auto subspan = tracing::Tracer::instance().start_span("CurpClient.master_propose", tracing::SpanKind::CLIENT);
        subspan.set_attribute("rpc_id", static_cast<int64_t>(op.rpc_id));
        subspan.set_attribute("target", config_.master_addr);
        
        spdlog::debug("[CurpClient] MASTER_PROPOSE_START | rpc_id={} | target={}", 
                     op.rpc_id, config_.master_addr);
        
        auto resp = master_client_->propose(op);
        
        spdlog::debug("[CurpClient] MASTER_PROPOSE_COMPLETE | rpc_id={} | success={} | "
                     "fast_path={} | need_sync={}",
                     op.rpc_id, resp.success, resp.fast_path, resp.need_sync);
        
        subspan.set_attribute("success", static_cast<bool>(resp.success));
        subspan.set_attribute("fast_path", static_cast<bool>(resp.fast_path));
        subspan.set_attribute("need_sync", resp.need_sync);
        if (resp.success) {
            subspan.set_ok();
        } else {
            subspan.set_error(resp.error);
        }
        
        return resp;
    });
    
    auto witness_future = std::async(std::launch::async, [&]() {
        auto subspan = tracing::Tracer::instance().start_span("CurpClient.witness_record", tracing::SpanKind::CLIENT);
        subspan.set_attribute("rpc_id", static_cast<int64_t>(op.rpc_id));
        subspan.set_attribute("witness_count", static_cast<int64_t>(witness_clients_.size()));
        
        spdlog::debug("[CurpClient] WITNESS_RECORD_START | rpc_id={} | witness_count={}", 
                     op.rpc_id, witness_clients_.size());
        
        int accepted = record_to_witnesses(op.rpc_id, op.key, request_data);
        
        spdlog::debug("[CurpClient] WITNESS_RECORD_COMPLETE | rpc_id={} | accepted={} | "
                     "total={} | all_accepted={}",
                     op.rpc_id, accepted, witness_clients_.size(), 
                     accepted == static_cast<int>(witness_clients_.size()));
        
        subspan.set_attribute("accepted", static_cast<int64_t>(accepted));
        subspan.set_attribute("all_accepted", accepted == static_cast<int>(witness_clients_.size()));
        subspan.set_ok();
        
        return accepted;
    });
    
    // 等待 Master 响应
    auto master_resp = master_future.get();
    
    if (!master_resp.success) {
        spdlog::warn("[CurpClient] PARALLEL_WRITE_FAILED | rpc_id={} | reason=master_rejected | "
                     "error='{}'", op.rpc_id, master_resp.error);
        
        span.set_attribute("result", "MASTER_REJECTED");
        span.set_attribute("error", master_resp.error);
        span.set_error(master_resp.error);
        
        return {false, false, 1, master_resp.error};
    }
    
    if (master_resp.need_sync) {
        // 需要走慢速路径
        spdlog::info("[CurpClient] NEED_SYNC | rpc_id={} | key='{}' | reason=master_need_sync",
                    op.rpc_id, op.key);
        
        bool sync_ok = send_sync_to_master(op.rpc_id);
        
        if (sync_ok) {
            spdlog::info("[CurpClient] SYNC_SUCCESS | rpc_id={} | key='{}' | rtt=2",
                        op.rpc_id, op.key);
            
            span.set_attribute("result", "SYNC_SUCCESS");
            span.set_attribute("rtt_count", static_cast<int64_t>(2));
            span.set_ok();
            
            return {true, false, 2, ""};
        } else {
            spdlog::warn("[CurpClient] SYNC_FAILED | rpc_id={} | key='{}'",
                        op.rpc_id, op.key);
            
            span.set_attribute("result", "SYNC_FAILED");
            span.set_error("sync failed");
            
            return {false, false, 1, "sync failed"};
        }
    }
    
    // Master 成功，检查 Witness
    int witness_accepted = witness_future.get();
    int witness_count = static_cast<int>(witness_clients_.size());
    
    if (witness_accepted == witness_count) {
        // 所有 Witness 都接受 -> 快速路径
        spdlog::info("[CurpClient] FAST_PATH_SUCCESS | rpc_id={} | key='{}' | "
                    "witness_accepted={} | witness_count={} | rtt=1",
                    op.rpc_id, op.key, witness_accepted, witness_count);
        
        span.set_attribute("result", "FAST_PATH");
        span.set_attribute("witness_accepted", static_cast<int64_t>(witness_accepted));
        span.set_attribute("rtt_count", static_cast<int64_t>(1));
        span.set_ok();
        
        return {true, true, 1, ""};
    }
    
    // 部分 Witness 未接受 -> 需要同步
    spdlog::info("[CurpClient] PARTIAL_WITNESS | rpc_id={} | key='{}' | "
                "witness_accepted={} | witness_count={} | need_sync=true",
                op.rpc_id, op.key, witness_accepted, witness_count);
    
    bool sync_ok = send_sync_to_master(op.rpc_id);
    
    if (sync_ok) {
        spdlog::info("[CurpClient] SYNC_SUCCESS | rpc_id={} | key='{}' | rtt=2 | "
                    "witness_partial=true", op.rpc_id, op.key);
        
        span.set_attribute("result", "SYNC_AFTER_PARTIAL_WITNESS");
        span.set_attribute("witness_accepted", static_cast<int64_t>(witness_accepted));
        span.set_attribute("rtt_count", static_cast<int64_t>(2));
        span.set_ok();
        
        return {true, false, 2, ""};
    }
    
    spdlog::warn("[CurpClient] SYNC_FAILED_AFTER_PARTIAL | rpc_id={} | key='{}'",
                op.rpc_id, op.key);
    
    span.set_attribute("result", "SYNC_FAILED");
    span.set_error("sync failed after partial witness");
    
    return {false, false, 1, "sync failed"};
}

int CurpClient::record_to_witnesses(uint64_t rpc_id, const std::string& key,
                                     const std::vector<uint8_t>& request_data) {
    int accepted = 0;
    int failed = 0;
    
    for (size_t i = 0; i < witness_clients_.size(); i++) {
        auto& client = witness_clients_[i];
        
        try {
            auto result = client->record("master", rpc_id, key, request_data, config_.client_id);
            
            if (result == RecordResult::ACCEPTED) {
                accepted++;
                spdlog::trace("[CurpClient] WITNESS_ACCEPTED | witness_idx={} | rpc_id={} | key='{}'",
                             i, rpc_id, key);
            } else {
                failed++;
                spdlog::trace("[CurpClient] WITNESS_REJECTED | witness_idx={} | rpc_id={} | "
                             "key='{}' | result={}", i, rpc_id, key, static_cast<int>(result));
            }
        } catch (const std::exception& e) {
            failed++;
            spdlog::warn("[CurpClient] WITNESS_ERROR | witness_idx={} | rpc_id={} | "
                        "error='{}'", i, rpc_id, e.what());
        }
    }
    
    return accepted;
}

bool CurpClient::send_sync_to_master(uint64_t rpc_id) {
    auto span = tracing::Tracer::instance().start_span("CurpClient.send_sync", tracing::SpanKind::CLIENT);
    span.set_attribute("rpc_id", static_cast<int64_t>(rpc_id));
    
    spdlog::debug("[CurpClient] SEND_SYNC | rpc_id={} | master_addr={}", 
                 rpc_id, config_.master_addr);
    
    bool result = master_client_->sync(rpc_id);
    
    spdlog::debug("[CurpClient] SYNC_RESPONSE | rpc_id={} | success={}", rpc_id, result);
    
    span.set_attribute("success", static_cast<bool>(result));
    if (result) {
        span.set_ok();
    } else {
        span.set_error("sync request failed");
    }
    
    return result;
}

CurpClient::ReadResult CurpClient::read(const std::string& key) {
    auto span = tracing::Tracer::instance().start_span("CurpClient.read", tracing::SpanKind::CLIENT);
    span.set_attribute("key", key);
    
    auto start_time = std::chrono::steady_clock::now();
    
    total_reads_++;
    
    spdlog::info("[CurpClient] READ_START | key='{}' | client_id={} | total_reads={}",
                key, config_.client_id, total_reads_);
    
    auto resp = master_client_->read(key);
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    if (resp.success) {
        spdlog::info("[CurpClient] READ_SUCCESS | key='{}' | value_size={} | latency_ms={}",
                    key, resp.value.size(), duration);
        
        span.set_attribute("result", "SUCCESS");
        span.set_attribute("value_size", static_cast<int64_t>(resp.value.size()));
        span.set_attribute("latency_ms", static_cast<int64_t>(duration));
        span.set_ok();
    } else {
        spdlog::warn("[CurpClient] READ_FAILED | key='{}' | error='{}' | latency_ms={}",
                    key, resp.error, duration);
        
        span.set_attribute("result", "FAILED");
        span.set_attribute("error", resp.error);
        span.set_attribute("latency_ms", static_cast<int64_t>(duration));
        span.set_error(resp.error);
    }
    
    return {resp.success, resp.value, resp.error};
}

std::vector<uint8_t> CurpClient::serialize_request(const CurpOp& op) {
    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>(op.type));
    
    uint32_t key_len = op.key.size();
    data.insert(data.end(), (uint8_t*)&key_len, (uint8_t*)&key_len + 4);
    data.insert(data.end(), op.key.begin(), op.key.end());
    
    uint32_t value_len = op.value.size();
    data.insert(data.end(), (uint8_t*)&value_len, (uint8_t*)&value_len + 4);
    data.insert(data.end(), op.value.begin(), op.value.end());
    
    return data;
}

// ========== CurpMasterClient ==========

CurpMasterClient::CurpMasterClient(const std::string& target) : target_(target) {
    spdlog::debug("[CurpMasterClient] INIT | target={}", target);
}

CurpMasterClient::ProposeResponse CurpMasterClient::propose(const CurpOp& op) {
    // 模拟成功响应
    return {true, true, false, {}, ""};
}

CurpMasterClient::ReadResponse CurpMasterClient::read(const std::string& key) {
    return {true, {}, ""};
}

bool CurpMasterClient::sync(uint64_t rpc_id) {
    return true;
}

} // namespace curp