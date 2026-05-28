#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <atomic>
#include <optional>

namespace curp {

/**
 * @brief Witness 存储的请求记录
 * 
 * Witness 只存储请求，不执行。用于保证 CURP 中操作的持久性。
 */
struct WitnessRecord {
    uint64_t rpc_id;              // RIFL 唯一请求 ID
    std::string key;              // 操作的 key
    std::vector<uint8_t> request_data;  // 序列化的请求数据
    uint64_t client_id;           // 客户端 ID
    uint64_t timestamp;           // 记录时间戳（毫秒）
    
    WitnessRecord() = default;
    WitnessRecord(uint64_t rpc_id, std::string key, 
                  std::vector<uint8_t> data, uint64_t client_id)
        : rpc_id(rpc_id), key(std::move(key)), request_data(std::move(data)),
          client_id(client_id), timestamp(currentTimeMs()) {}
    
    static uint64_t currentTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

/**
 * @brief 记录请求的结果
 */
enum class RecordResult {
    ACCEPTED,                      // 接受成功
    REJECTED_NOT_COMMUTATIVE,      // 拒绝：不可交换（key 冲突）
    REJECTED_NO_SPACE,            // 拒绝：空间不足
    REJECTED_STOPPED,             // 拒绝：已停止接受请求
    REJECTED_WRONG_MASTER         // 拒绝：Master ID 不匹配
};

/**
 * @brief GC 条目
 */
struct GCEntry {
    std::string key;
    uint64_t rpc_id;
};

/**
 * @brief Witness - CURP 协议的核心组件
 * 
 * Witness 是轻量级的临时持久化存储，用于在请求被排序之前保证其持久性。
 * 
 * 核心特性：
 * 1. 只存储请求，不执行
 * 2. 强制可交换性：同一个 key 只能有一个未同步的请求
 * 3. 存储在非易失内存中（简化版：内存 + 可选持久化）
 * 
 * 教学简化：
 * - 单 Witness 实例即可演示
 * - 简化的 GC 策略
 */
class Witness {
public:
    // 最大存储请求数（教学简化）
    static constexpr size_t kMaxRecords = 10000;
    
    // GC 阈值时间（毫秒）
    static constexpr uint64_t kGCThresholdMs = 60000;  // 60秒
    
    explicit Witness(const std::string& storage_dir = "");
    ~Witness() = default;
    
    // ========== RPC 接口 ==========
    
    /**
     * @brief 记录客户端请求
     * 
     * @param master_id 目标 Master ID
     * @param rpc_id 唯一请求 ID
     * @param key 操作的 key
     * @param request_data 序列化的请求数据
     * @param client_id 客户端 ID
     * @return RecordResult 接受或拒绝原因
     */
    RecordResult record(const std::string& master_id, uint64_t rpc_id,
                        const std::string& key,
                        const std::vector<uint8_t>& request_data,
                        uint64_t client_id);
    
    /**
     * @brief 垃圾回收已同步的请求
     * 
     * @param master_id Master ID
     * @param entries 要清理的请求列表
     * @return 清理的请求数量
     */
    uint32_t garbage_collect(const std::string& master_id,
                             const std::vector<GCEntry>& entries);
    
    /**
     * @brief 获取恢复数据
     * 
     * 获取所有存储的请求，用于 Master 崩溃恢复。
     * 调用此方法后，Witness 进入恢复模式，不再接受新请求。
     * 
     * @param master_id Master ID
     * @return 所有存储的请求
     */
    std::vector<WitnessRecord> get_recovery_data(const std::string& master_id);
    
    /**
     * @brief 停止接受新请求
     */
    void stop_accepting();
    
    /**
     * @brief 重置 Witness 为新 Master
     */
    void reset(const std::string& new_master_id = "");
    
    /**
     * @brief 获取当前状态
     */
    struct Status {
        std::string master_id;
        bool accepting;
        uint32_t request_count;
        std::optional<uint64_t> oldest_timestamp;
    };
    Status get_status() const;
    
    // ========== 辅助方法 ==========
    
    /**
     * @brief 检查 key 是否有冲突
     */
    bool has_key_conflict(const std::string& key) const;
    
    /**
     * @brief 获取当前存储的请求数量
     */
    size_t size() const;
    
    /**
     * @brief 检查是否正在接受请求
     */
    bool is_accepting() const { return accepting_.load(); }
    
private:
    mutable std::mutex mtx_;
    
    // 绑定的 Master ID
    std::string master_id_;
    
    // 是否接受新请求
    std::atomic<bool> accepting_{true};
    
    // 存储的请求：key -> record
    // 注意：一个 key 只能有一个未同步的请求（可交换性保证）
    std::unordered_map<std::string, WitnessRecord> records_;
    
    // RPC ID 集合，用于快速查重
    std::unordered_set<uint64_t> rpc_ids_;
    
    // 存储目录（可选持久化）
    std::string storage_dir_;
    
    // 统计信息
    uint64_t total_recorded_{0};
    uint64_t total_rejected_{0};
    uint64_t total_gc_{0};
};

} // namespace curp
