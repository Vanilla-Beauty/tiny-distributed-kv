#pragma once

#include "../../proto/witness.grpc.pb.h"
#include "curp/witness.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

namespace curp {

/**
 * @brief Witness gRPC 服务实现
 */
class WitnessServiceImpl final : public witness::WitnessService::Service {
public:
    explicit WitnessServiceImpl(std::shared_ptr<Witness> witness);
    ~WitnessServiceImpl() override = default;
    
    grpc::Status Record(grpc::ServerContext* context,
                        const witness::RecordRequest* request,
                        witness::RecordReply* reply) override;
    
    grpc::Status GarbageCollect(grpc::ServerContext* context,
                                const witness::GCRequest* request,
                                witness::GCReply* reply) override;
    
    grpc::Status GetRecoveryData(grpc::ServerContext* context,
                                 const witness::RecoveryRequest* request,
                                 witness::RecoveryData* reply) override;
    
    grpc::Status Stop(grpc::ServerContext* context,
                      const witness::StopRequest* request,
                      witness::StopReply* reply) override;
    
    grpc::Status Reset(grpc::ServerContext* context,
                       const witness::ResetRequest* request,
                       witness::ResetReply* reply) override;
    
    grpc::Status GetStatus(grpc::ServerContext* context,
                           const witness::StatusRequest* request,
                           witness::StatusReply* reply) override;
    
private:
    std::shared_ptr<Witness> witness_;
};

/**
 * @brief Witness 客户端 - 用于远程调用
 */
class WitnessClient {
public:
    explicit WitnessClient(const std::string& target);
    ~WitnessClient() = default;
    
    // 记录请求
    RecordResult record(const std::string& master_id, uint64_t rpc_id,
                        const std::string& key,
                        const std::vector<uint8_t>& request_data,
                        uint64_t client_id);
    
    // 垃圾回收
    uint32_t garbage_collect(const std::string& master_id,
                             const std::vector<GCEntry>& entries);
    
    // 获取恢复数据
    std::vector<WitnessRecord> get_recovery_data(const std::string& master_id);
    
    // 停止接受
    bool stop(const std::string& master_id);
    
    // 重置
    bool reset(const std::string& new_master_id);
    
    // 获取状态
    Witness::Status get_status();
    
private:
    std::unique_ptr<witness::WitnessService::Stub> stub_;
};

} // namespace curp
