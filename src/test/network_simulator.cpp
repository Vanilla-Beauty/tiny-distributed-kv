#include "test/network_simulator.h"
#include "spdlog/spdlog.h"
#include <algorithm>

namespace curp {
namespace test {

NetworkSimulator::NetworkSimulator(const NetworkConfig& config)
    : config_(config), rng_(config.seed),
      message_queue_([](const SimulatedMessage& a, const SimulatedMessage& b) {
          return a.deliver_time > b.deliver_time;
      }) {
    spdlog::info("NetworkSimulator: 初始化, min_delay={}, max_delay={}, loss_rate={}",
                config.min_delay_ms, config.max_delay_ms, config.packet_loss_rate);
}

NetworkSimulator::~NetworkSimulator() {
    stop();
    spdlog::info("NetworkSimulator: 销毁");
}

void NetworkSimulator::start() {
    if (running_.load()) {
        return;
    }
    
    running_.store(true);
    worker_thread_ = std::thread(&NetworkSimulator::worker_loop, this);
    spdlog::info("NetworkSimulator: 启动");
}

void NetworkSimulator::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    spdlog::info("NetworkSimulator: 停止");
}

void NetworkSimulator::reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    while (!message_queue_.empty()) {
        message_queue_.pop();
    }
    
    isolated_nodes_.clear();
    partitions_.clear();
    delay_config_.clear();
    dropped_count_ = 0;
    delivered_count_ = 0;
    current_time_ = 0;
    
    spdlog::info("NetworkSimulator: 重置");
}

void NetworkSimulator::register_handler(uint64_t node_id,
                                          std::function<void(uint64_t, const std::vector<uint8_t>&, const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(mtx_);
    handlers_[node_id] = handler;
    spdlog::debug("NetworkSimulator: 注册节点 {} 的消息处理器", node_id);
}

void NetworkSimulator::send_message(uint64_t from, uint64_t to,
                                     const std::vector<uint8_t>& data,
                                     const std::string& type) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // 检查是否应该丢包
    if (should_drop(from, to)) {
        dropped_count_++;
        spdlog::debug("NetworkSimulator: 丢包 from={}, to={}, type={}", from, to, type);
        return;
    }
    
    // 检查是否能送达
    if (!can_deliver(from, to)) {
        dropped_count_++;
        spdlog::debug("NetworkSimulator: 消息无法送达（分区） from={}, to={}", from, to);
        return;
    }
    
    // 创建消息
    SimulatedMessage msg;
    msg.id = next_message_id_++;
    msg.from_node = from;
    msg.to_node = to;
    msg.data = data;
    msg.type = type;
    msg.send_time = current_time_;
    msg.deliver_time = current_time_ + get_delay(from, to);
    
    message_queue_.push(msg);
    
    spdlog::debug("NetworkSimulator: 发送消息 id={}, from={}, to={}, delay={}",
                 msg.id, from, to, msg.deliver_time - msg.send_time);
}

void NetworkSimulator::set_delay(uint64_t from, uint64_t to, uint64_t min_ms, uint64_t max_ms) {
    std::lock_guard<std::mutex> lock(mtx_);
    delay_config_[{from, to}] = {min_ms, max_ms};
    spdlog::debug("NetworkSimulator: 设置延迟 from={}, to={}, min={}, max={}",
                 from, to, min_ms, max_ms);
}

void NetworkSimulator::set_packet_loss_rate(double rate) {
    std::lock_guard<std::mutex> lock(mtx_);
    config_.packet_loss_rate = rate;
    spdlog::debug("NetworkSimulator: 设置丢包率 {}", rate);
}

void NetworkSimulator::partition(const std::vector<uint64_t>& group1,
                                  const std::vector<uint64_t>& group2) {
    std::lock_guard<std::mutex> lock(mtx_);
    partitions_.push_back({group1, group2});
    
    spdlog::info("NetworkSimulator: 创建分区 group1.size={}, group2.size={}",
                group1.size(), group2.size());
}

void NetworkSimulator::heal() {
    std::lock_guard<std::mutex> lock(mtx_);
    partitions_.clear();
    isolated_nodes_.clear();
    
    spdlog::info("NetworkSimulator: 恢复网络");
}

void NetworkSimulator::isolate_node(uint64_t node_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    isolated_nodes_.insert(node_id);
    spdlog::info("NetworkSimulator: 隔离节点 {}", node_id);
}

void NetworkSimulator::reconnect_node(uint64_t node_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    isolated_nodes_.erase(node_id);
    spdlog::info("NetworkSimulator: 恢复节点 {} 连接", node_id);
}

uint64_t NetworkSimulator::current_time() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return current_time_;
}

void NetworkSimulator::advance_time(uint64_t ms) {
    std::lock_guard<std::mutex> lock(mtx_);
    current_time_ += ms;
    spdlog::debug("NetworkSimulator: 推进时间 {} ms, current_time={}", ms, current_time_);
}

size_t NetworkSimulator::pending_messages() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return message_queue_.size();
}

size_t NetworkSimulator::dropped_messages() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return dropped_count_;
}

size_t NetworkSimulator::delivered_messages() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return delivered_count_;
}

void NetworkSimulator::worker_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        
        std::lock_guard<std::mutex> lock(mtx_);
        
        // 推进时间
        current_time_++;
        
        // 处理可送达的消息
        while (!message_queue_.empty()) {
            auto msg = message_queue_.top();
            
            if (msg.deliver_time <= current_time_) {
                message_queue_.pop();
                
                // 检查处理器是否存在
                auto it = handlers_.find(msg.to_node);
                if (it != handlers_.end()) {
                    it->second(msg.from_node, msg.data, msg.type);
                    delivered_count_++;
                    spdlog::trace("NetworkSimulator: 送达消息 id={}, to={}", msg.id, msg.to_node);
                }
            } else {
                break;
            }
        }
    }
}

bool NetworkSimulator::should_drop(uint64_t from, uint64_t to) {
    if (config_.packet_loss_rate <= 0) {
        return false;
    }
    
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng_) < config_.packet_loss_rate;
}

uint64_t NetworkSimulator::get_delay(uint64_t from, uint64_t to) {
    auto it = delay_config_.find({from, to});
    if (it != delay_config_.end()) {
        std::uniform_int_distribution<uint64_t> dist(it->second.first, it->second.second);
        return dist(rng_);
    }
    
    std::uniform_int_distribution<uint64_t> dist(config_.min_delay_ms, config_.max_delay_ms);
    return dist(rng_);
}

bool NetworkSimulator::can_deliver(uint64_t from, uint64_t to) {
    // 检查是否被隔离
    if (isolated_nodes_.find(from) != isolated_nodes_.end() ||
        isolated_nodes_.find(to) != isolated_nodes_.end()) {
        return false;
    }
    
    // 检查是否在分区中
    for (const auto& partition : partitions_) {
        bool in_group1 = std::find(partition.first.begin(), partition.first.end(), from) != partition.first.end();
        bool in_group2 = std::find(partition.second.begin(), partition.second.end(), from) != partition.second.end();
        
        bool to_in_group1 = std::find(partition.first.begin(), partition.first.end(), to) != partition.first.end();
        bool to_in_group2 = std::find(partition.second.begin(), partition.second.end(), to) != partition.second.end();
        
        // 如果 from 和 to 在不同的组，不能送达
        if ((in_group1 && to_in_group2) || (in_group2 && to_in_group1)) {
            return false;
        }
    }
    
    return true;
}

} // namespace test
} // namespace curp