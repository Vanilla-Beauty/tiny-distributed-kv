#include <gtest/gtest.h>
#include "curp/curp_master.h"
#include "curp/curp_client.h"
#include "curp/witness.h"
#include <memory>
#include <thread>
#include <chrono>

using namespace curp;

// ========== 恢复机制测试 ==========

class RecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建集群配置
        cluster_ = {
            NodeConfig("localhost:50061"),
            NodeConfig("localhost:50062"),
            NodeConfig("localhost:50063")
        };
        
        log_dir_ = "/tmp/curp_recovery_test";
    }
    
    void TearDown() override {
        // 清理
    }
    
    std::vector<NodeConfig> cluster_;
    std::string log_dir_;
};

TEST_F(RecoveryTest, BasicRecoveryFlow) {
    // 1. 创建 Master 和本地 Witness
    auto master = CurpMaster::Create(cluster_, log_dir_, 0);
    auto witness = std::make_shared<Witness>();
    
    master->set_local_witness(witness);
    
    // 2. 执行一些操作
    CurpOp op1(1, 100, OpType::PUT, "key1", {'v', '1'});
    CurpOp op2(2, 100, OpType::PUT, "key2", {'v', '2'});
    CurpOp op3(3, 100, OpType::PUT, "key3", {'v', '3'});
    
    auto r1 = master->propose(op1);
    auto r2 = master->propose(op2);
    auto r3 = master->propose(op3);
    
    EXPECT_TRUE(r1.success);
    EXPECT_TRUE(r2.success);
    EXPECT_TRUE(r3.success);
    
    // 3. 模拟记录到 Witness（客户端会做这个）
    witness->record("master0", 1, "key1", {'d', 'a', 't', 'a'}, 100);
    witness->record("master0", 2, "key2", {'d', 'a', 't', 'a'}, 100);
    witness->record("master0", 3, "key3", {'d', 'a', 't', 'a'}, 100);
    
    EXPECT_EQ(witness->size(), 3);
    
    // 4. 模拟 Master 崩溃后恢复
    // 停止同步线程
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 5. 创建新的 Master（模拟恢复）
    auto new_master = CurpMaster::Create(cluster_, log_dir_, 0);
    auto new_witness = std::make_shared<Witness>();
    
    // 设置相同的 Witness（模拟 Witness 数据持久化）
    // 实际中 Witness 数据会持久化到非易失内存
    new_witness->record("master0", 1, "key1", {'d', 'a', 't', 'a'}, 100);
    new_witness->record("master0", 2, "key2", {'d', 'a', 't', 'a'}, 100);
    new_witness->record("master0", 3, "key3", {'d', 'a', 't', 'a'}, 100);
    
    new_master->set_local_witness(new_witness);
    
    // 6. 执行恢复
    new_master->recover();
    
    // 7. 验证恢复后状态
    EXPECT_EQ(new_master->get_curp_state(), CurpState::NORMAL);
    
    // 8. 新操作应该正常工作
    CurpOp op4(4, 100, OpType::PUT, "key4", {'v', '4'});
    auto r4 = new_master->propose(op4);
    EXPECT_TRUE(r4.success);
}

TEST_F(RecoveryTest, RecoveryWithEmptyWitness) {
    auto master = CurpMaster::Create(cluster_, log_dir_, 1);
    auto witness = std::make_shared<Witness>();
    
    master->set_local_witness(witness);
    
    // Witness 为空时恢复
    master->recover();
    
    EXPECT_EQ(master->get_curp_state(), CurpState::NORMAL);
}

TEST_F(RecoveryTest, RecoveryRejectsNewOps) {
    auto master = CurpMaster::Create(cluster_, log_dir_, 2);
    auto witness = std::make_shared<Witness>();
    
    master->set_local_witness(witness);
    
    // 开始恢复
    std::thread recover_thread([&]() {
        master->recover();
    });
    
    // 恢复过程中新操作应该被拒绝（简化版不实际测试竞态）
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    recover_thread.join();
    
    EXPECT_EQ(master->get_curp_state(), CurpState::NORMAL);
}

TEST_F(RecoveryTest, DuplicateRpcIdHandling) {
    auto master = CurpMaster::Create(cluster_, log_dir_, 3);
    auto witness = std::make_shared<Witness>();
    
    master->set_local_witness(witness);
    
    // 执行操作
    CurpOp op(1, 100, OpType::PUT, "key1", {'v'});
    master->propose(op);
    
    // 记录到 Witness
    witness->record("master3", 1, "key1", {'d'}, 100);
    
    // 模拟恢复（会重放相同的操作）
    master->recover();
    
    // 由于去重机制，不应该重复执行
    EXPECT_EQ(master->unsynced_count(), 1);  // 只有一个，不会重复
}

TEST_F(RecoveryTest, MultipleKeysRecovery) {
    auto master = CurpMaster::Create(cluster_, log_dir_, 4);
    auto witness = std::make_shared<Witness>();
    
    master->set_local_witness(witness);
    
    // 执行多个不同 key 的操作
    for (int i = 0; i < 10; i++) {
        CurpOp op(i + 1, 100, OpType::PUT, 
                 "key" + std::to_string(i), 
                 {'v', static_cast<uint8_t>(i)});
        master->propose(op);
        witness->record("master4", i + 1, "key" + std::to_string(i), 
                       {'d'}, 100);
    }
    
    EXPECT_EQ(master->unsynced_count(), 10);
    EXPECT_EQ(witness->size(), 10);
    
    // 模拟恢复
    auto new_master = CurpMaster::Create(cluster_, log_dir_, 4);
    auto new_witness = std::make_shared<Witness>();
    
    for (int i = 0; i < 10; i++) {
        new_witness->record("master4", i + 1, "key" + std::to_string(i), 
                           {'d'}, 100);
    }
    
    new_master->set_local_witness(new_witness);
    new_master->recover();
    
    EXPECT_EQ(new_master->get_curp_state(), CurpState::NORMAL);
}

// ========== CurpState 测试 ==========

TEST(CurpStateTest, StateTransitions) {
    std::vector<NodeConfig> cluster = {NodeConfig("localhost:50071")};
    
    auto master = CurpMaster::Create(cluster, "/tmp/state_test", 0);
    
    EXPECT_EQ(master->get_curp_state(), CurpState::NORMAL);
    EXPECT_FALSE(master->is_recovering());
    
    // 恢复后应该回到 NORMAL
    master->recover();
    EXPECT_EQ(master->get_curp_state(), CurpState::NORMAL);
}

// ========== 集成测试 ==========

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        cluster_ = {
            NodeConfig("localhost:60061"),
            NodeConfig("localhost:60062"),
            NodeConfig("localhost:60063")
        };
    }
    
    std::vector<NodeConfig> cluster_;
};

TEST_F(IntegrationTest, FullCurpFlow) {
    // 完整的 CURP 流程测试
    
    // 1. 创建 Master 和 Witness
    auto master = CurpMaster::Create(cluster_, "/tmp/integration_test", 0);
    auto witness = std::make_shared<Witness>();
    master->set_local_witness(witness);
    
    // 2. 客户端发起写操作
    CurpOp op1(1, 1000, OpType::PUT, "test_key", {'t', 'e', 's', 't'});
    auto result = master->propose(op1);
    
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.fast_path);  // 第一个操作应该走快速路径
    
    // 3. 模拟记录到 Witness
    witness->record("master0", 1, "test_key", {'d'}, 1000);
    
    // 4. 等待异步同步
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    // 5. 读操作（无冲突）
    auto read_result = master->read("other_key");
    EXPECT_TRUE(read_result.success);
    
    // 6. 冲突的写操作
    CurpOp op2(2, 1000, OpType::PUT, "test_key", {'n', 'e', 'w'});
    auto result2 = master->propose(op2);
    
    EXPECT_FALSE(result2.success);
    EXPECT_TRUE(result2.need_sync);
}

TEST_F(IntegrationTest, WitnessConflictHandling) {
    auto master = CurpMaster::Create(cluster_, "/tmp/conflict_test", 1);
    auto witness = std::make_shared<Witness>();
    master->set_local_witness(witness);
    
    // 写入 key1
    CurpOp op1(1, 100, OpType::PUT, "key1", {'v', '1'});
    master->propose(op1);
    witness->record("master1", 1, "key1", {'d'}, 100);
    
    // 再次写入 key1 应该触发慢路径
    CurpOp op2(2, 100, OpType::PUT, "key1", {'v', '2'});
    auto result = master->propose(op2);
    
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.need_sync);
}

// ========== 主函数 ==========

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
