#include <gtest/gtest.h>
#include "curp/witness.h"
#include "grpc/witness_service_impl.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <thread>

using namespace curp;

// ========== Witness 类单元测试 ==========

class WitnessTest : public ::testing::Test {
protected:
    void SetUp() override {
        witness_ = std::make_shared<Witness>();
    }
    
    std::shared_ptr<Witness> witness_;
};

TEST_F(WitnessTest, InitialState) {
    auto status = witness_->get_status();
    EXPECT_TRUE(status.master_id.empty());
    EXPECT_TRUE(status.accepting);
    EXPECT_EQ(status.request_count, 0);
    EXPECT_FALSE(status.oldest_timestamp.has_value());
}

TEST_F(WitnessTest, RecordAccept) {
    auto result = witness_->record("master1", 1, "key1", {}, 100);
    EXPECT_EQ(result, RecordResult::ACCEPTED);
    
    auto status = witness_->get_status();
    EXPECT_EQ(status.master_id, "master1");
    EXPECT_EQ(status.request_count, 1);
}

TEST_F(WitnessTest, RecordCommutativeKeys) {
    // 不同 key 应该都能接受
    auto r1 = witness_->record("master1", 1, "key1", {}, 100);
    EXPECT_EQ(r1, RecordResult::ACCEPTED);
    
    auto r2 = witness_->record("master1", 2, "key2", {}, 100);
    EXPECT_EQ(r2, RecordResult::ACCEPTED);
    
    auto r3 = witness_->record("master1", 3, "key3", {}, 100);
    EXPECT_EQ(r3, RecordResult::ACCEPTED);
    
    EXPECT_EQ(witness_->size(), 3);
}

TEST_F(WitnessTest, RecordNonCommutativeReject) {
    // 相同 key 应该被拒绝
    auto r1 = witness_->record("master1", 1, "key1", {1, 2, 3}, 100);
    EXPECT_EQ(r1, RecordResult::ACCEPTED);
    
    auto r2 = witness_->record("master1", 2, "key1", {4, 5, 6}, 100);
    EXPECT_EQ(r2, RecordResult::REJECTED_NOT_COMMUTATIVE);
    
    // 原记录应该保留
    EXPECT_TRUE(witness_->has_key_conflict("key1"));
}

TEST_F(WitnessTest, RecordDuplicateRpcId) {
    auto r1 = witness_->record("master1", 1, "key1", {}, 100);
    EXPECT_EQ(r1, RecordResult::ACCEPTED);
    
    // 相同 RPC ID 应该被视为已接受
    auto r2 = witness_->record("master1", 1, "key2", {}, 100);
    EXPECT_EQ(r2, RecordResult::ACCEPTED);
    
    // 不应该增加存储数量
    EXPECT_EQ(witness_->size(), 1);
}

TEST_F(WitnessTest, RecordWrongMaster) {
    witness_->record("master1", 1, "key1", {}, 100);
    
    // 不同 Master 的请求应该被拒绝
    auto r2 = witness_->record("master2", 2, "key2", {}, 100);
    EXPECT_EQ(r2, RecordResult::REJECTED_WRONG_MASTER);
}

TEST_F(WitnessTest, GarbageCollect) {
    witness_->record("master1", 1, "key1", {}, 100);
    witness_->record("master1", 2, "key2", {}, 100);
    witness_->record("master1", 3, "key3", {}, 100);
    
    EXPECT_EQ(witness_->size(), 3);
    
    // GC
    std::vector<GCEntry> entries = {
        {"key1", 1},
        {"key2", 2}
    };
    
    uint32_t dropped = witness_->garbage_collect("master1", entries);
    EXPECT_EQ(dropped, 2);
    EXPECT_EQ(witness_->size(), 1);
    
    // key3 应该还在
    EXPECT_FALSE(witness_->has_key_conflict("key1"));
    EXPECT_FALSE(witness_->has_key_conflict("key2"));
    EXPECT_TRUE(witness_->has_key_conflict("key3"));
}

TEST_F(WitnessTest, StopAccepting) {
    witness_->stop_accepting();
    
    auto result = witness_->record("master1", 1, "key1", {}, 100);
    EXPECT_EQ(result, RecordResult::REJECTED_STOPPED);
}

TEST_F(WitnessTest, Reset) {
    witness_->record("master1", 1, "key1", {}, 100);
    witness_->record("master1", 2, "key2", {}, 100);
    
    EXPECT_EQ(witness_->size(), 2);
    
    witness_->reset("master2");
    
    auto status = witness_->get_status();
    EXPECT_EQ(status.master_id, "master2");
    EXPECT_TRUE(status.accepting);
    EXPECT_EQ(status.request_count, 0);
}

TEST_F(WitnessTest, RecoveryData) {
    witness_->record("master1", 1, "key1", {1, 2, 3}, 100);
    witness_->record("master1", 2, "key2", {4, 5, 6}, 100);
    witness_->record("master1", 3, "key3", {7, 8, 9}, 100);
    
    auto records = witness_->get_recovery_data("master1");
    EXPECT_EQ(records.size(), 3);
    
    // 应该进入停止状态
    EXPECT_FALSE(witness_->is_accepting());
    
    // 验证记录内容
    std::unordered_map<std::string, WitnessRecord> record_map;
    for (const auto& r : records) {
        record_map[r.key] = r;
    }
    
    EXPECT_EQ(record_map["key1"].rpc_id, 1);
    EXPECT_EQ(record_map["key2"].rpc_id, 2);
    EXPECT_EQ(record_map["key3"].rpc_id, 3);
}

TEST_F(WitnessTest, MaxRecords) {
    // 测试空间限制
    for (int i = 0; i < Witness::kMaxRecords + 100; i++) {
        auto result = witness_->record("master1", i, "key" + std::to_string(i), {}, 100);
        
        if (i < Witness::kMaxRecords) {
            EXPECT_EQ(result, RecordResult::ACCEPTED);
        } else {
            EXPECT_EQ(result, RecordResult::REJECTED_NO_SPACE);
        }
    }
}

// ========== Witness gRPC 服务测试 ==========

class WitnessRpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        witness_ = std::make_shared<Witness>();
        service_ = std::make_unique<WitnessServiceImpl>(witness_);
        
        // 启动服务器
        server_address_ = "localhost:50051";
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address_, grpc::InsecureServerCredentials());
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        
        // 创建客户端
        channel_ = grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials());
        stub_ = witness::WitnessService::NewStub(channel_);
    }
    
    void TearDown() override {
        server_->Shutdown();
    }
    
    std::shared_ptr<Witness> witness_;
    std::unique_ptr<WitnessServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
    std::string server_address_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<witness::WitnessService::Stub> stub_;
};

TEST_F(WitnessRpcTest, RecordRpc) {
    witness::RecordRequest request;
    request.set_master_id("master1");
    request.set_rpc_id(1);
    request.set_key("key1");
    request.set_request_data({1, 2, 3});
    request.set_client_id(100);
    
    witness::RecordReply reply;
    grpc::ClientContext context;
    
    auto status = stub_->Record(&context, request, &reply);
    
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(reply.accepted());
}

TEST_F(WitnessRpcTest, GarbageCollectRpc) {
    // 先记录
    witness::RecordRequest req1;
    req1.set_master_id("master1");
    req1.set_rpc_id(1);
    req1.set_key("key1");
    
    witness::RecordReply reply1;
    grpc::ClientContext ctx1;
    stub_->Record(&ctx1, req1, &reply1);
    
    // GC
    witness::GCRequest gc_req;
    gc_req.set_master_id("master1");
    auto* entry = gc_req.add_entries();
    entry->set_key("key1");
    entry->set_rpc_id(1);
    
    witness::GCReply gc_reply;
    grpc::ClientContext ctx2;
    
    auto status = stub_->GarbageCollect(&ctx2, gc_req, &gc_reply);
    
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(gc_reply.success());
    EXPECT_EQ(gc_reply.dropped_count(), 1);
}

TEST_F(WitnessRpcTest, GetStatusRpc) {
    witness::StatusRequest request;
    witness::StatusReply reply;
    grpc::ClientContext context;
    
    auto status = stub_->GetStatus(&context, request, &reply);
    
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(reply.accepting());
}

// ========== WitnessClient 测试 ==========

class WitnessClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        witness_ = std::make_shared<Witness>();
        service_ = std::make_unique<WitnessServiceImpl>(witness_);
        
        server_address_ = "localhost:50052";
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address_, grpc::InsecureServerCredentials());
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        
        client_ = std::make_unique<WitnessClient>(server_address_);
    }
    
    void TearDown() override {
        server_->Shutdown();
    }
    
    std::shared_ptr<Witness> witness_;
    std::unique_ptr<WitnessServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
    std::string server_address_;
    std::unique_ptr<WitnessClient> client_;
};

TEST_F(WitnessClientTest, ClientRecord) {
    auto result = client_->record("master1", 1, "key1", {1, 2, 3}, 100);
    EXPECT_EQ(result, RecordResult::ACCEPTED);
    
    auto status = client_->get_status();
    EXPECT_EQ(status.master_id, "master1");
    EXPECT_EQ(status.request_count, 1);
}

TEST_F(WitnessClientTest, ClientGC) {
    client_->record("master1", 1, "key1", {}, 100);
    client_->record("master1", 2, "key2", {}, 100);
    
    auto status = client_->get_status();
    EXPECT_EQ(status.request_count, 2);
    
    std::vector<GCEntry> entries = {{"key1", 1}};
    uint32_t dropped = client_->garbage_collect("master1", entries);
    
    EXPECT_EQ(dropped, 1);
    
    status = client_->get_status();
    EXPECT_EQ(status.request_count, 1);
}

TEST_F(WitnessClientTest, ClientRecovery) {
    client_->record("master1", 1, "key1", {1, 2, 3}, 100);
    client_->record("master1", 2, "key2", {4, 5, 6}, 100);
    
    auto records = client_->get_recovery_data("master1");
    
    EXPECT_EQ(records.size(), 2);
    
    // 验证服务端已停止接受
    auto status = client_->get_status();
    EXPECT_FALSE(status.accepting);
}

// ========== 主函数 ==========

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
