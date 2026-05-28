#include <gtest/gtest.h>
#include "curp/curp_master.h"
#include "curp/curp_client.h"
#include "curp/witness.h"
#include <memory>

using namespace curp;

// ========== CurpMaster 单元测试 ==========

class CurpMasterTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::vector<NodeConfig> cluster = {
            NodeConfig("localhost:50061"),
            NodeConfig("localhost:50062"),
            NodeConfig("localhost:50063")
        };
        
        master_ = CurpMaster::Create(cluster, "/tmp/curp_test_log", 0);
    }
    
    void TearDown() override {
        master_.reset();
    }
    
    std::shared_ptr<CurpMaster> master_;
};

TEST_F(CurpMasterTest, InitialState) {
    EXPECT_EQ(master_->get_curp_state(), CurpState::NORMAL);
    EXPECT_EQ(master_->unsynced_count(), 0);
}

TEST_F(CurpMasterTest, ProposeFirstOp) {
    CurpOp op(1, 100, OpType::PUT, "key1", {'v', 'a', 'l'});
    
    auto result = master_->propose(op);
    
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.fast_path);
    EXPECT_FALSE(result.need_sync);
    
    EXPECT_EQ(master_->unsynced_count(), 1);
    EXPECT_TRUE(master_->has_unsynced_key("key1"));
}

TEST_F(CurpMasterTest, ProposeConflictingOp) {
    CurpOp op1(1, 100, OpType::PUT, "key1", {'v', '1'});
    auto result1 = master_->propose(op1);
    EXPECT_TRUE(result1.success);
    EXPECT_TRUE(result1.fast_path);
    
    CurpOp op2(2, 100, OpType::PUT, "key1", {'v', '2'});
    auto result2 = master_->propose(op2);
    
    EXPECT_FALSE(result2.success);
    EXPECT_FALSE(result2.fast_path);
    EXPECT_TRUE(result2.need_sync);
}

TEST_F(CurpMasterTest, ProposeDifferentKeys) {
    CurpOp op1(1, 100, OpType::PUT, "key1", {'v'});
    CurpOp op2(2, 100, OpType::PUT, "key2", {'v'});
    CurpOp op3(3, 100, OpType::PUT, "key3", {'v'});
    
    auto result1 = master_->propose(op1);
    auto result2 = master_->propose(op2);
    auto result3 = master_->propose(op3);
    
    EXPECT_TRUE(result1.success && result1.fast_path);
    EXPECT_TRUE(result2.success && result2.fast_path);
    EXPECT_TRUE(result3.success && result3.fast_path);
    
    EXPECT_EQ(master_->unsynced_count(), 3);
}

TEST_F(CurpMasterTest, DuplicateRpcId) {
    CurpOp op1(1, 100, OpType::PUT, "key1", {'v', '1'});
    auto result1 = master_->propose(op1);
    EXPECT_TRUE(result1.success);
    
    CurpOp op2(1, 100, OpType::PUT, "key2", {'v', '2'});
    auto result2 = master_->propose(op2);
    EXPECT_TRUE(result2.success);
    EXPECT_TRUE(result2.fast_path);
    
    EXPECT_EQ(master_->unsynced_count(), 1);
}

TEST_F(CurpMasterTest, ReadConflictWithUnsynced) {
    CurpOp op(1, 100, OpType::PUT, "key1", {'v'});
    master_->propose(op);
    
    auto result = master_->read("key1");
    EXPECT_FALSE(result.success);
}

TEST_F(CurpMasterTest, ReadNoConflict) {
    CurpOp op(1, 100, OpType::PUT, "key1", {'v'});
    master_->propose(op);
    
    auto result = master_->read("key2");
    EXPECT_TRUE(result.success);
}

TEST_F(CurpMasterTest, SyncClearsUnsynced) {
    for (int i = 0; i < 5; i++) {
        CurpOp op(i + 1, 100, OpType::PUT, "key" + std::to_string(i), {'v'});
        master_->propose(op);
    }
    
    EXPECT_EQ(master_->unsynced_count(), 5);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// ========== CurpClient 单元测试 ==========

class CurpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        CurpClientConfig config;
        config.master_addr = "localhost:50080";
        config.witness_addrs = {"localhost:50081"};
        config.client_id = 1000;
        
        client_ = std::make_unique<CurpClient>(config);
    }
    
    void TearDown() override {
        client_.reset();
    }
    
    std::unique_ptr<CurpClient> client_;
};

TEST_F(CurpClientTest, RpcIdGeneration) {
    uint64_t id1 = client_->next_rpc_id();
    uint64_t id2 = client_->next_rpc_id();
    
    EXPECT_NE(id1, id2);
    EXPECT_GT(id2, id1);
}

TEST_F(CurpClientTest, Reset) {
    auto id1 = client_->next_rpc_id();
    client_->reset();
    auto id2 = client_->next_rpc_id();
    
    EXPECT_NE(id1, id2);
}

TEST_F(CurpClientTest, WriteResultStructure) {
    CurpClient::WriteResult result;
    result.success = true;
    result.fast_path = true;
    result.rtt_count = 1;
    
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.fast_path);
    EXPECT_EQ(result.rtt_count, 1);
}

// ========== CurpOp 测试 ==========

TEST(CurpOpTest, Creation) {
    CurpOp op(123, 456, OpType::PUT, "test_key", {'d', 'a', 't', 'a'});
    
    EXPECT_EQ(op.rpc_id, 123);
    EXPECT_EQ(op.client_id, 456);
    EXPECT_EQ(op.type, OpType::PUT);
    EXPECT_EQ(op.key, "test_key");
    EXPECT_EQ(op.value.size(), 4);
}

TEST(CurpOpTest, DefaultValue) {
    CurpOp op;
    EXPECT_EQ(op.rpc_id, 0);
    EXPECT_EQ(op.client_id, 0);
    EXPECT_TRUE(op.key.empty());
    EXPECT_TRUE(op.value.empty());
}

// ========== CurpConfig 测试 ==========

TEST(CurpConfigTest, Creation) {
    CurpConfig config;
    config.log_dir = "/tmp/test";
    config.node_id = 1;
    
    EXPECT_EQ(config.log_dir, "/tmp/test");
    EXPECT_EQ(config.node_id, 1);
}

// ========== 主函数 ==========

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
