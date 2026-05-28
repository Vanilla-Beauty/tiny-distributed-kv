#pragma once

#include "../raft/raft.h"
#include "../raft/config.h"
#include "witness.h"  // 包含 WitnessRecord 定义
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <memory>

namespace curp {

// 前向声明
class WitnessClient;

/**
 * @brief CurpMaster 状态
 */
enum class CurpState {
    NORMAL,      // 正常运行
    RECOVERING,  // 正在恢复
    STOPPED      // 已停止
};

/**
 * @brief 操作类型
 */
enum class OpType {
    PUT,         // 写操作
    GET,         // 读操作
    DELETE_OP    // 删除操作
};

/**
 * @brief CURP 操作请求
 */
struct CurpOp {
    uint64_t rpc_id;
    uint64_t client_id;
    OpType type;
    std::string key;
    std::vector<uint8_t> value;
    
    CurpOp() = default;
    CurpOp(uint64_t rpc_id, uint64_t client_id, OpType type, 
           std::string key, std::vector<uint8_t> value = {})
        : rpc_id(rpc_id), client_id(client_id), type(type),
          key(std::move(key)), value(std::move(value)) {}
};

/**
 * @brief CurpMaster - CURP 协议的 Master 实现
 */
class CurpMaster : public RaftNode {
    friend class CurpServiceImpl;
    
public:
    static std::shared_ptr<CurpMaster> Create(
        std::vector<NodeConfig> cluster_configs,
        std::string log_dir,
        uint64_t cur_node_id);
    
    ~CurpMaster();
    
    // ========== CURP 核心接口 ==========
    
    struct ProposeResult {
        bool success;
        bool fast_path;
        bool need_sync;
        std::string error;
    };
    ProposeResult propose(const CurpOp& op);
    
    struct ReadResult {
        bool success;
        std::vector<uint8_t> value;
    };
    ReadResult read(const std::string& key);
    
    void sync();
    
    // ========== Witness 管理 ==========
    
    void set_witness_clients(std::vector<std::shared_ptr<WitnessClient>> clients);
    void set_local_witness(std::shared_ptr<Witness> witness);
    
    // ========== 恢复机制 ==========
    
    void recover();
    size_t replay_from_witness(std::shared_ptr<WitnessClient> witness_client);
    size_t replay_from_local_witness();
    
    // ========== 状态查询 ==========
    
    CurpState get_curp_state() const;
    size_t unsynced_count() const;
    bool has_unsynced_key(const std::string& key) const;
    bool is_recovering() const;
    
private:
    CurpMaster(std::vector<NodeConfig> cluster_configs,
               std::string log_dir,
               uint64_t cur_node_id);
    
    std::mutex curp_mtx_;
    std::atomic<CurpState> curp_state_{CurpState::NORMAL};
    
    std::vector<std::shared_ptr<WitnessClient>> witness_clients_;
    std::shared_ptr<Witness> local_witness_;
    
    std::unordered_set<std::string> unsynced_keys_;
    std::unordered_map<uint64_t, CurpOp> unsynced_ops_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> executed_ops_;
    
    uint64_t last_sync_index_{0};
    std::atomic<bool> syncing_{false};
    
    std::thread sync_thread_;
    std::condition_variable sync_cv_;
    std::atomic<bool> sync_running_{true};
    
    uint64_t total_proposes_{0};
    uint64_t total_fast_path_{0};
    uint64_t total_slow_path_{0};
    uint64_t total_recovered_{0};
    
    bool can_speculative_execute(const CurpOp& op);
    void mark_unsynced(const CurpOp& op);
    std::vector<uint8_t> execute_op(const CurpOp& op);
    void sync_worker();
    void do_sync();
    bool is_executed(uint64_t rpc_id) const;
    void record_executed(uint64_t rpc_id, const std::vector<uint8_t>& result);
    
    void recover_from_backup();
    void replay_ops(const std::vector<WitnessRecord>& records);
};

struct CurpConfig {
    std::vector<NodeConfig> cluster_configs;
    std::string log_dir;
    uint64_t node_id;
};

} // namespace curp