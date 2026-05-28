#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <functional>
#include <random>
#include <memory>

// 为 pair<uint64_t, uint64_t> 提供哈希函数
namespace std {
template<>
struct hash<std::pair<uint64_t, uint64_t>> {
    size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
        return hash<uint64_t>()(p.first) ^ (hash<uint64_t>()(p.second) << 1);
    }
};
}

namespace curp {
namespace test {

struct SimulatedMessage {
    uint64_t id;
    uint64_t from_node;
    uint64_t to_node;
    std::vector<uint8_t> data;
    uint64_t send_time;
    uint64_t deliver_time;
    std::string type;
};

struct NetworkConfig {
    uint64_t min_delay_ms{1};
    uint64_t max_delay_ms{50};
    double packet_loss_rate{0.0};
    uint64_t seed{42};
};

class NetworkSimulator {
public:
    explicit NetworkSimulator(const NetworkConfig& config);
    ~NetworkSimulator();
    
    void send_message(uint64_t from, uint64_t to, 
                      const std::vector<uint8_t>& data,
                      const std::string& type = "");
    
    void register_handler(uint64_t node_id, 
                          std::function<void(uint64_t, const std::vector<uint8_t>&, const std::string&)> handler);
    
    void set_delay(uint64_t from, uint64_t to, uint64_t min_ms, uint64_t max_ms);
    void set_packet_loss_rate(double rate);
    void partition(const std::vector<uint64_t>& group1, const std::vector<uint64_t>& group2);
    void heal();
    void isolate_node(uint64_t node_id);
    void reconnect_node(uint64_t node_id);
    
    size_t pending_messages() const;
    size_t dropped_messages() const;
    size_t delivered_messages() const;
    
    void start();
    void stop();
    void reset();
    
    uint64_t current_time() const;
    void advance_time(uint64_t ms);
    
private:
    NetworkConfig config_;
    std::mt19937 rng_;
    
    mutable std::mutex mtx_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    
    std::priority_queue<SimulatedMessage, 
                        std::vector<SimulatedMessage>,
                        std::function<bool(const SimulatedMessage&, const SimulatedMessage&)>> message_queue_;
    
    std::unordered_map<uint64_t, 
                       std::function<void(uint64_t, const std::vector<uint8_t>&, const std::string&)>> handlers_;
    
    std::unordered_map<std::pair<uint64_t, uint64_t>, 
                       std::pair<uint64_t, uint64_t>> delay_config_;
    
    std::unordered_set<uint64_t> isolated_nodes_;
    std::vector<std::pair<std::vector<uint64_t>, std::vector<uint64_t>>> partitions_;
    
    uint64_t next_message_id_{1};
    uint64_t current_time_{0};
    size_t dropped_count_{0};
    size_t delivered_count_{0};
    
    void worker_loop();
    bool should_drop(uint64_t from, uint64_t to);
    uint64_t get_delay(uint64_t from, uint64_t to);
    bool can_deliver(uint64_t from, uint64_t to);
};

} // namespace test
} // namespace curp