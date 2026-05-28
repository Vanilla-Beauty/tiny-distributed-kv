#include <gtest/gtest.h>
#include "test/network_simulator.h"
#include "test/node_controller.h"
#include "test/consistency_checker.h"
#include "curp/curp_master.h"
#include "curp/witness.h"
#include "spdlog/spdlog.h"
#include <thread>
#include <chrono>
#include <atomic>

using namespace curp;
using namespace curp::test;

// ========== 测试框架基类 ==========

class CurpTestBase : public ::testing::Test {
protected:
    void SetUp() override {
        NetworkConfig net_config;
        net_config.min_delay_ms = 1;
        net_config.max_delay_ms = 10;
        net_config.packet_loss_rate = 0.0;
        network_ = std::make_unique<NetworkSimulator>(net_config);
        network_->start();
        
        nodes_ = std::make_unique<NodeController>();
        checker_ = std::make_unique<ConsistencyChecker>();
        
        log_dir_ = "/tmp/curp_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    
    void TearDown() override {
        nodes_.reset();
        network_->stop();
        network_.reset();
        checker_.reset();
    }
    
    std::unique_ptr<NetworkSimulator> network_;
    std::unique_ptr<NodeController> nodes_;
    std::unique_ptr<ConsistencyChecker> checker_;
    std::string log_dir_;
};

// ========== 基本正确性测试 ==========

class BasicCorrectnessTest : public CurpTestBase {};

TEST_F(BasicCorrectnessTest, SingleWriteRead) {
    std::vector<NodeConfig> cluster = {
        NodeConfig("localhost:7001"),
        NodeConfig("localhost:7002"),
        NodeConfig("localhost:7003")
    };
    
    nodes_->create_master(0, cluster, log_dir_ + "/master0");
    nodes_->create_witness(100);
    nodes_->bind_witness_to_master(0, 100);
    nodes_->start_node(0);
    nodes_->start_node(100);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    CurpOp op(1, 1000, OpType::PUT, "test_key", {'t', 'e', 's', 't'});
    auto result = nodes_->master_propose(0, op);
    
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.fast_path);
    
    auto stats = nodes_->get_stats(0);
    EXPECT_EQ(stats.total_ops, 1);
    EXPECT_EQ(stats.fast_path_ops, 1);
    
    EXPECT_TRUE(checker_->check_linearizability());
}

TEST_F(BasicCorrectnessTest, MultipleWritesDifferentKeys) {
    std::vector<NodeConfig> cluster = {
        NodeConfig("localhost:7011"),
        NodeConfig("localhost:7012"),
        NodeConfig("localhost:7013")
    };
    
    nodes_->create_master(1, cluster, log_dir_ + "/master1");
    nodes_->create_witness(101);
    nodes_->bind_witness_to_master(1, 101);
    nodes_->start_node(1);
    nodes_->start_node(101);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    for (int i = 0; i < 10; i++) {
        CurpOp op(i + 1, 1000, OpType::PUT, 
                  "key" + std::to_string(i), {'v'});
        auto result = nodes_->master_propose(1, op);
        EXPECT_TRUE(result.success);
    }
    
    auto stats = nodes_->get_stats(1);
    EXPECT_EQ(stats.fast_path_ops, 10);
    EXPECT_EQ(stats.slow_path_ops, 0);
    
    EXPECT_TRUE(checker_->check_linearizability());
}

TEST_F(BasicCorrectnessTest, ConflictingWrites) {
    std::vector<NodeConfig> cluster = {
        NodeConfig("localhost:7021"),
        NodeConfig("localhost:7022"),
        NodeConfig("localhost:7023")
    };
    
    nodes_->create_master(2, cluster, log_dir_ + "/master2");
    nodes_->create_witness(102);
    nodes_->bind_witness_to_master(2, 102);
    nodes_->start_node(2);
    nodes_->start_node(102);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    CurpOp op1(1, 1000, OpType::PUT, "conflict_key", {'v', '1'});
    auto result1 = nodes_->master_propose(2, op1);
    EXPECT_TRUE(result1.success);
    EXPECT_TRUE(result1.fast_path);
    
    CurpOp op2(2, 1000, OpType::PUT, "conflict_key", {'v', '2'});
    auto result2 = nodes_->master_propose(2, op2);
    EXPECT_FALSE(result2.success);
    EXPECT_TRUE(result2.need_sync);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    CurpOp op3(3, 1000, OpType::PUT, "conflict_key", {'v', '3'});
    auto result3 = nodes_->master_propose(2, op3);
    EXPECT_TRUE(result3.success);
    EXPECT_TRUE(result3.fast_path);
}

// ========== 故障恢复测试 ==========

class FailureRecoveryTest : public CurpTestBase {};

TEST_F(FailureRecoveryTest, MasterCrashRecovery) {
    std::vector<NodeConfig> cluster = {
        NodeConfig("localhost:7031"),
        NodeConfig("localhost:7032"),
        NodeConfig("localhost:7033")
    };
    
    nodes_->create_master(3, cluster, log_dir_ + "/master3");
    nodes_->create_witness(103);
    nodes_->bind_witness_to_master(3, 103);
    nodes_->start_node(3);
    nodes_->start_node(103);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    for (int i = 0; i < 5; i++) {
        CurpOp op(i + 1, 1000, OpType::PUT, 
                  "key" + std::to_string(i), {'v'});
        nodes_->master_propose(3, op);
    }
    
    EXPECT_EQ(nodes_->unsynced_count(3), 5);
    
    nodes_->crash_node(3);
    EXPECT_FALSE(nodes_->is_node_running(3));
    
    nodes_->recover_node(3);
    EXPECT_TRUE(nodes_->is_node_running(3));
    
    CurpOp op_after(100, 1000, OpType::PUT, "after_recovery", {'v'});
    auto result = nodes_->master_propose(3, op_after);
    EXPECT_TRUE(result.success);
}

// ========== 并发测试 ==========

class ConcurrencyTest : public CurpTestBase {};

TEST_F(ConcurrencyTest, ConcurrentWrites) {
    std::vector<NodeConfig> cluster = {
        NodeConfig("localhost:7081"),
        NodeConfig("localhost:7082"),
        NodeConfig("localhost:7083")
    };
    
    nodes_->create_master(8, cluster, log_dir_ + "/master8");
    nodes_->create_witness(108);
    nodes_->bind_witness_to_master(8, 108);
    nodes_->start_node(8);
    nodes_->start_node(108);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    const int num_threads = 4;
    const int ops_per_thread = 25;
    
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> fast_path_count{0};
    
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; i++) {
                std::string key = "thread" + std::to_string(t) + "_key" + std::to_string(i);
                CurpOp op(t * 100 + i + 1, t + 1, OpType::PUT, key, {'v'});
                auto result = nodes_->master_propose(8, op);
                
                if (result.success) {
                    success_count++;
                    if (result.fast_path) {
                        fast_path_count++;
                    }
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    int total_ops = num_threads * ops_per_thread;
    EXPECT_EQ(success_count.load(), total_ops);
    EXPECT_EQ(fast_path_count.load(), total_ops);
    
    EXPECT_TRUE(checker_->check_linearizability());
    
    auto stats = nodes_->get_stats(8);
    spdlog::info("并发测试: total={}, fast_path={}, slow_path={}", 
                stats.total_ops, stats.fast_path_ops, stats.slow_path_ops);
}

// ========== 性能测试 ==========

class PerformanceTest : public CurpTestBase {};

TEST_F(PerformanceTest, FastPathRatio) {
    std::vector<NodeConfig> cluster = {
        NodeConfig("localhost:7091"),
        NodeConfig("localhost:7092"),
        NodeConfig("localhost:7093")
    };
    
    nodes_->create_master(9, cluster, log_dir_ + "/master9");
    nodes_->create_witness(109);
    nodes_->bind_witness_to_master(9, 109);
    nodes_->start_node(9);
    nodes_->start_node(109);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    const int num_ops = 100;
    
    for (int i = 0; i < num_ops; i++) {
        CurpOp op(i + 1, 1000, OpType::PUT, 
                  "perf_key" + std::to_string(i), {'v'});
        nodes_->master_propose(9, op);
    }
    
    auto stats = nodes_->get_stats(9);
    
    double fast_ratio = static_cast<double>(stats.fast_path_ops) / num_ops;
    EXPECT_GE(fast_ratio, 0.99);
    
    spdlog::info("性能测试: fast_path_ratio={:.2f}%", fast_ratio * 100);
}

// ========== 主函数 ==========

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
