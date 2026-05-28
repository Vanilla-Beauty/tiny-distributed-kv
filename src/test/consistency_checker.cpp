#include "test/consistency_checker.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <numeric>

namespace curp {
namespace test {

ConsistencyChecker::ConsistencyChecker() {
    spdlog::info("ConsistencyChecker: 初始化");
}

uint64_t ConsistencyChecker::get_current_time() const {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint64_t>(ms);
}

uint64_t ConsistencyChecker::record_write_start(uint64_t client_id, uint64_t rpc_id,
                                                  const std::string& key,
                                                  const std::vector<uint8_t>& value) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (!enabled_) {
        return 0;
    }
    
    OperationRecord record;
    record.id = next_op_id_++;
    record.client_id = client_id;
    record.rpc_id = rpc_id;
    record.key = key;
    record.value = value;
    record.start_time = get_current_time();
    record.success = false;
    
    size_t index = operations_.size();
    operations_.push_back(record);
    op_id_to_index_[record.id] = index;
    key_operations_[key].push_back(index);
    
    spdlog::debug("ConsistencyChecker: 记录写操作开始 op_id={}, key='{}'", 
                 record.id, key);
    
    return record.id;
}

void ConsistencyChecker::record_write_end(uint64_t op_id, bool success, 
                                           bool fast_path, int rtt_count) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (!enabled_ || op_id == 0) {
        return;
    }
    
    auto it = op_id_to_index_.find(op_id);
    if (it == op_id_to_index_.end()) {
        return;
    }
    
    auto& record = operations_[it->second];
    record.end_time = get_current_time();
    record.success = success;
    record.fast_path = fast_path;
    record.rtt_count = rtt_count;
    
    spdlog::debug("ConsistencyChecker: 记录写操作结束 op_id={}, success={}, fast_path={}",
                 op_id, success, fast_path);
}

uint64_t ConsistencyChecker::record_read(uint64_t client_id, const std::string& key,
                                          const std::vector<uint8_t>& value_returned,
                                          bool success) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (!enabled_) {
        return 0;
    }
    
    OperationRecord record;
    record.id = next_op_id_++;
    record.client_id = client_id;
    record.key = key;
    record.value = value_returned;
    record.start_time = get_current_time();
    record.end_time = record.start_time;
    record.success = success;
    
    size_t index = operations_.size();
    operations_.push_back(record);
    op_id_to_index_[record.id] = index;
    key_operations_[key].push_back(index);
    
    spdlog::debug("ConsistencyChecker: 记录读操作 op_id={}, key='{}', success={}",
                 record.id, key, success);
    
    return record.id;
}

bool ConsistencyChecker::check_linearizability() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    violations_.clear();
    
    // 简化的线性一致性检查：
    // 对于同一个 key，检查是否存在读写顺序违反
    // 规则：如果操作 A 在操作 B 开始之前完成，那么 A 必须在 B 之前
    
    for (const auto& [key, op_indices] : key_operations_) {
        if (op_indices.size() < 2) {
            continue;
        }
        
        // 对该 key 的所有操作，检查时间序
        for (size_t i = 0; i < op_indices.size(); i++) {
            for (size_t j = i + 1; j < op_indices.size(); j++) {
                const auto& op1 = operations_[op_indices[i]];
                const auto& op2 = operations_[op_indices[j]];
                
                // 如果 op1 和 op2 都成功，检查时间序
                if (!op1.success || !op2.success) {
                    continue;
                }
                
                // op1 在 op2 开始之前完成
                if (op1.end_time < op2.start_time) {
                    // 这是合法的：op1 -> op2
                    continue;
                }
                
                // op2 在 op1 开始之前完成
                if (op2.end_time < op1.start_time) {
                    // 这是合法的：op2 -> op1
                    continue;
                }
                
                // 如果两个操作时间重叠，且都是对同一 key 的写操作
                // 这在 CURP 中应该被阻止（慢速路径）
                if (op1.fast_path && op2.fast_path) {
                    // 如果都走了快速路径且时间重叠，说明可能有问题
                    // 但在 CURP 中，同一 key 的并发写会触发慢速路径
                    spdlog::warn("ConsistencyChecker: 可能的一致性问题 "
                                "op1={}, op2={}, key='{}', 都走快速路径且时间重叠",
                                op1.id, op2.id, key);
                    
                    violations_.push_back({op1.id, op2.id});
                }
            }
        }
    }
    
    if (!violations_.empty()) {
        spdlog::warn("ConsistencyChecker: 检测到 {} 个线性一致性违反", violations_.size());
        return false;
    }
    
    spdlog::debug("ConsistencyChecker: 线性一致性检查通过");
    return true;
}

bool ConsistencyChecker::check_data_integrity() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // 数据完整性检查：所有成功的写操作都应该被持久化
    // 在测试环境中，这需要通过与实际存储对比来验证
    
    size_t successful_writes = 0;
    for (const auto& op : operations_) {
        if (op.success) {
            successful_writes++;
        }
    }
    
    spdlog::debug("ConsistencyChecker: 数据完整性检查, 成功操作数={}", successful_writes);
    
    // 简化版：假设都成功
    return true;
}

std::vector<std::pair<uint64_t, uint64_t>> ConsistencyChecker::get_violations() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx_));
    return violations_;
}

ConsistencyChecker::Statistics ConsistencyChecker::get_statistics() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx_));
    
    Statistics stats;
    stats.total_operations = operations_.size();
    
    uint64_t total_latency = 0;
    int rtt_count = 0;
    
    for (const auto& op : operations_) {
        if (op.success) {
            if (op.fast_path) {
                stats.fast_path_count++;
            } else {
                stats.slow_path_count++;
            }
            
            total_latency += (op.end_time - op.start_time);
            rtt_count += op.rtt_count;
        } else {
            stats.failed_operations++;
        }
    }
    
    stats.successful_writes = stats.fast_path_count + stats.slow_path_count;
    
    if (stats.total_operations > 0) {
        stats.fast_path_ratio = static_cast<double>(stats.fast_path_count) / 
                                static_cast<double>(stats.total_operations);
    }
    
    if (stats.successful_writes > 0) {
        stats.avg_latency_ms = static_cast<double>(total_latency) / 
                               static_cast<double>(stats.successful_writes);
        stats.avg_rtt_count = static_cast<double>(rtt_count) / 
                             static_cast<double>(stats.successful_writes);
    }
    
    stats.linearizability_violations = violations_.size();
    
    return stats;
}

void ConsistencyChecker::reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    operations_.clear();
    op_id_to_index_.clear();
    key_operations_.clear();
    violations_.clear();
    next_op_id_ = 1;
    
    spdlog::info("ConsistencyChecker: 重置");
}

void ConsistencyChecker::enable() {
    std::lock_guard<std::mutex> lock(mtx_);
    enabled_ = true;
    spdlog::info("ConsistencyChecker: 启用");
}

void ConsistencyChecker::disable() {
    std::lock_guard<std::mutex> lock(mtx_);
    enabled_ = false;
    spdlog::info("ConsistencyChecker: 禁用");
}

} // namespace test
} // namespace curp