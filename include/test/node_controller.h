#pragma once

#include "curp/curp_master.h"
#include "curp/curp_client.h"
#include "curp/witness.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace curp {
namespace test {

/**
 * @brief 模拟节点类型
 */
enum class NodeType {
    MASTER,
    BACKUP,
    WITNESS,
    CLIENT
};

/**
 * @brief 模拟节点状态
 */
enum class NodeState {
    RUNNING,
    STOPPED,
    CRASHED,
    RECOVERING
};

/**
 * @brief 模拟节点配置
 */
struct SimulatedNodeConfig {
    uint64_t id;
    NodeType type;
    std::string addr;
};

/**
 * @brief 节点控制器
 * 
 * 功能：
 * - 启动/停止节点
 * - 模拟崩溃和恢复
 * - 控制时钟
 * - 监控节点状态
 */
class NodeController {
public:
    explicit NodeController();
    ~NodeController();
    
    // ========== 节点管理 ==========
    
    /**
     * @brief 创建 Master 节点
     */
    void create_master(uint64_t node_id, 
                       const std::vector<NodeConfig>& cluster_configs,
                       const std::string& log_dir);
    
    /**
     * @brief 创建 Witness 节点
     */
    void create_witness(uint64_t witness_id);
    
    /**
     * @brief 创建 Client
     */
    void create_client(uint64_t client_id, const CurpClientConfig& config);
    
    /**
     * @brief 绑定 Witness 到 Master
     */
    void bind_witness_to_master(uint64_t master_id, uint64_t witness_id);
    
    // ========== 节点控制 ==========
    
    /**
     * @brief 启动节点
     */
    void start_node(uint64_t node_id);
    
    /**
     * @brief 停止节点（正常停止）
     */
    void stop_node(uint64_t node_id);
    
    /**
     * @brief 崩溃节点（模拟崩溃，丢失内存状态）
     */
    void crash_node(uint64_t node_id);
    
    /**
     * @brief 恢复节点
     */
    void recover_node(uint64_t node_id);
    
    /**
     * @brief 杀死并立即重启（模拟崩溃恢复）
     */
    void crash_and_recover(uint64_t node_id, uint64_t delay_ms = 0);
    
    // ========== 操作接口 ==========
    
    /**
     * @brief 客户端发起写操作
     */
    CurpClient::WriteResult client_write(uint64_t client_id, 
                                          const std::string& key,
                                          const std::vector<uint8_t>& value);
    
    /**
     * @brief 客户端发起读操作
     */
    CurpClient::ReadResult client_read(uint64_t client_id, const std::string& key);
    
    /**
     * @brief Master 直接接收操作（用于测试）
     */
    CurpMaster::ProposeResult master_propose(uint64_t master_id, const CurpOp& op);
    
    // ========== 状态查询 ==========
    
    NodeState get_node_state(uint64_t node_id) const;
    bool is_node_running(uint64_t node_id) const;
    size_t unsynced_count(uint64_t master_id) const;
    
    // ========== 统计 ==========
    
    struct Stats {
        size_t total_ops{0};
        size_t fast_path_ops{0};
        size_t slow_path_ops{0};
        size_t failed_ops{0};
    };
    
    Stats get_stats(uint64_t node_id) const;
    
private:
    std::mutex mtx_;
    
    // 节点实例
    std::unordered_map<uint64_t, std::shared_ptr<CurpMaster>> masters_;
    std::unordered_map<uint64_t, std::shared_ptr<Witness>> witnesses_;
    std::unordered_map<uint64_t, std::unique_ptr<CurpClient>> clients_;
    
    // 节点状态
    std::unordered_map<uint64_t, NodeState> node_states_;
    std::unordered_map<uint64_t, NodeType> node_types_;
    
    // 统计信息
    std::unordered_map<uint64_t, Stats> node_stats_;
    
    // Witness 到 Master 的绑定
    std::unordered_map<uint64_t, uint64_t> witness_bindings_;
    
    // 存储路径（用于恢复）
    std::unordered_map<uint64_t, std::string> log_dirs_;
};

} // namespace test
} // namespace curp