#include "test/node_controller.h"
#include "spdlog/spdlog.h"

namespace curp {
namespace test {

NodeController::NodeController() {
    spdlog::info("NodeController: 初始化");
}

NodeController::~NodeController() {
    spdlog::info("NodeController: 销毁");
}

void NodeController::create_master(uint64_t node_id,
                                    const std::vector<NodeConfig>& cluster_configs,
                                    const std::string& log_dir) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto master = CurpMaster::Create(cluster_configs, log_dir, node_id);
    masters_[node_id] = master;
    node_states_[node_id] = NodeState::STOPPED;
    node_types_[node_id] = NodeType::MASTER;
    log_dirs_[node_id] = log_dir;
    
    spdlog::info("NodeController: 创建 Master node_id={}", node_id);
}

void NodeController::create_witness(uint64_t witness_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto witness = std::make_shared<Witness>();
    witnesses_[witness_id] = witness;
    node_states_[witness_id] = NodeState::STOPPED;
    node_types_[witness_id] = NodeType::WITNESS;
    
    spdlog::info("NodeController: 创建 Witness witness_id={}", witness_id);
}

void NodeController::create_client(uint64_t client_id, const CurpClientConfig& config) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto client = std::make_unique<CurpClient>(config);
    clients_[client_id] = std::move(client);
    node_states_[client_id] = NodeState::RUNNING;
    node_types_[client_id] = NodeType::CLIENT;
    
    spdlog::info("NodeController: 创建 Client client_id={}", client_id);
}

void NodeController::bind_witness_to_master(uint64_t master_id, uint64_t witness_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto master_it = masters_.find(master_id);
    auto witness_it = witnesses_.find(witness_id);
    
    if (master_it != masters_.end() && witness_it != witnesses_.end()) {
        master_it->second->set_local_witness(witness_it->second);
        witness_bindings_[witness_id] = master_id;
        spdlog::info("NodeController: 绑定 Witness {} 到 Master {}", witness_id, master_id);
    }
}

void NodeController::start_node(uint64_t node_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    node_states_[node_id] = NodeState::RUNNING;
    spdlog::info("NodeController: 启动节点 {}", node_id);
}

void NodeController::stop_node(uint64_t node_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = masters_.find(node_id);
    if (it != masters_.end()) {
        it->second->kill();
    }
    
    node_states_[node_id] = NodeState::STOPPED;
    spdlog::info("NodeController: 停止节点 {}", node_id);
}

void NodeController::crash_node(uint64_t node_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = masters_.find(node_id);
    if (it != masters_.end()) {
        it->second->kill();
    }
    
    node_states_[node_id] = NodeState::CRASHED;
    spdlog::info("NodeController: 崩溃节点 {}", node_id);
}

void NodeController::recover_node(uint64_t node_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = node_types_.find(node_id);
    if (it == node_types_.end()) {
        return;
    }
    
    if (it->second == NodeType::MASTER) {
        auto master_it = masters_.find(node_id);
        if (master_it != masters_.end()) {
            master_it->second->recover();
        }
    }
    
    node_states_[node_id] = NodeState::RUNNING;
    spdlog::info("NodeController: 恢复节点 {}", node_id);
}

void NodeController::crash_and_recover(uint64_t node_id, uint64_t delay_ms) {
    crash_node(node_id);
    
    if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    
    recover_node(node_id);
}

CurpClient::WriteResult NodeController::client_write(uint64_t client_id,
                                                      const std::string& key,
                                                      const std::vector<uint8_t>& value) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        return {false, false, 0, "client not found"};
    }
    
    auto result = it->second->write(key, value);
    
    node_stats_[client_id].total_ops++;
    if (result.success) {
        if (result.fast_path) {
            node_stats_[client_id].fast_path_ops++;
        } else {
            node_stats_[client_id].slow_path_ops++;
        }
    } else {
        node_stats_[client_id].failed_ops++;
    }
    
    return result;
}

CurpClient::ReadResult NodeController::client_read(uint64_t client_id, const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        return {false, {}, "client not found"};
    }
    
    return it->second->read(key);
}

CurpMaster::ProposeResult NodeController::master_propose(uint64_t master_id, const CurpOp& op) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = masters_.find(master_id);
    if (it == masters_.end()) {
        return {false, false, false, "master not found"};
    }
    
    auto result = it->second->propose(op);
    
    node_stats_[master_id].total_ops++;
    if (result.success) {
        if (result.fast_path) {
            node_stats_[master_id].fast_path_ops++;
        } else {
            node_stats_[master_id].slow_path_ops++;
        }
    } else {
        node_stats_[master_id].failed_ops++;
    }
    
    return result;
}

NodeState NodeController::get_node_state(uint64_t node_id) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx_));
    
    auto it = node_states_.find(node_id);
    if (it != node_states_.end()) {
        return it->second;
    }
    return NodeState::STOPPED;
}

bool NodeController::is_node_running(uint64_t node_id) const {
    return get_node_state(node_id) == NodeState::RUNNING;
}

size_t NodeController::unsynced_count(uint64_t master_id) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx_));
    
    auto it = masters_.find(master_id);
    if (it != masters_.end()) {
        return it->second->unsynced_count();
    }
    return 0;
}

NodeController::Stats NodeController::get_stats(uint64_t node_id) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx_));
    
    auto it = node_stats_.find(node_id);
    if (it != node_stats_.end()) {
        return it->second;
    }
    return {};
}

} // namespace test
} // namespace curp