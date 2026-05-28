#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>

namespace curp {
namespace test {

/**
 * @brief 操作记录
 */
struct OperationRecord {
    uint64_t id;
    uint64_t client_id;
    uint64_t rpc_id;
    std::string key;
    std::vector<uint8_t> value;
    uint64_t start_time;
    uint64_t end_time;
    bool success;
    bool fast_path;
    int rtt_count;
};

/**
 * @brief 线性一致性检查器
 * 
 * 使用并发算法验证线性一致性：
 * 1. 记录所有操作及其时间戳
 * 2. 构建操作间的序关系
 * 3. 检测是否存在循环依赖（违反线性一致性）
 */
class ConsistencyChecker {
public:
    ConsistencyChecker();
    ~ConsistencyChecker() = default;
    
    // ========== 记录操作 ==========
    
    /**
     * @brief 记录写操作开始
     */
    uint64_t record_write_start(uint64_t client_id, uint64_t rpc_id,
                                  const std::string& key,
                                  const std::vector<uint8_t>& value);
    
    /**
     * @brief 记录写操作结束
     */
    void record_write_end(uint64_t op_id, bool success, bool fast_path, int rtt_count);
    
    /**
     * @brief 记录读操作
     */
    uint64_t record_read(uint64_t client_id, const std::string& key,
                          const std::vector<uint8_t>& value_returned,
                          bool success);
    
    // ========== 一致性检查 ==========
    
    /**
     * @brief 检查线性一致性
     * @return 是否违反线性一致性
     */
    bool check_linearizability();
    
    /**
     * @brief 检查数据完整性
     * 
     * 验证所有成功的写操作都被持久化
     */
    bool check_data_integrity();
    
    /**
     * @brief 获取违反一致性的操作对
     */
    std::vector<std::pair<uint64_t, uint64_t>> get_violations() const;
    
    // ========== 统计 ==========
    
    struct Statistics {
        size_t total_operations{0};
        size_t successful_writes{0};
        size_t successful_reads{0};
        size_t failed_operations{0};
        size_t fast_path_count{0};
        size_t slow_path_count{0};
        size_t linearizability_violations{0};
        double fast_path_ratio{0.0};
        double avg_rtt_count{0.0};
        double avg_latency_ms{0.0};
    };
    
    Statistics get_statistics() const;
    
    // ========== 控制 ==========
    
    void reset();
    void enable();
    void disable();
    
private:
    std::mutex mtx_;
    bool enabled_{true};
    
    // 操作记录
    std::vector<OperationRecord> operations_;
    uint64_t next_op_id_{1};
    
    // 索引
    std::unordered_map<uint64_t, size_t> op_id_to_index_;
    
    // 按key分组的操作
    std::unordered_map<std::string, std::vector<size_t>> key_operations_;
    
    // 违反记录
    std::vector<std::pair<uint64_t, uint64_t>> violations_;
    
    // 时间戳转换
    uint64_t get_current_time() const;
};

} // namespace test
} // namespace curp