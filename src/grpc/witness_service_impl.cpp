#include "grpc/witness_service_impl.h"
#include "spdlog/spdlog.h"

namespace curp {

// ========== WitnessServiceImpl ==========

WitnessServiceImpl::WitnessServiceImpl(std::shared_ptr<Witness> witness)
    : witness_(std::move(witness)) {
    spdlog::info("WitnessServiceImpl: 初始化完成");
}

grpc::Status WitnessServiceImpl::Record(grpc::ServerContext* context,
                                          const witness::RecordRequest* request,
                                          witness::RecordReply* reply) {
    spdlog::debug("WitnessServiceImpl::Record: master_id={}, rpc_id={}, key='{}'",
                 request->master_id(), request->rpc_id(), request->key());
    
    // 转换请求数据
    std::vector<uint8_t> data(request->request_data().begin(),
                               request->request_data().end());
    
    // 调用 Witness
    auto result = witness_->record(
        request->master_id(),
        request->rpc_id(),
        request->key(),
        data,
        request->client_id()
    );
    
    // 填充响应
    reply->set_accepted(result == RecordResult::ACCEPTED);
    
    switch (result) {
        case RecordResult::ACCEPTED:
            reply->set_reject_reason(0);
            reply->set_message("accepted");
            break;
        case RecordResult::REJECTED_NOT_COMMUTATIVE:
            reply->set_reject_reason(1);
            reply->set_message("key conflict");
            break;
        case RecordResult::REJECTED_NO_SPACE:
            reply->set_reject_reason(2);
            reply->set_message("no space");
            break;
        case RecordResult::REJECTED_STOPPED:
            reply->set_reject_reason(3);
            reply->set_message("stopped");
            break;
        case RecordResult::REJECTED_WRONG_MASTER:
            reply->set_reject_reason(4);
            reply->set_message("wrong master");
            break;
    }
    
    return grpc::Status::OK;
}

grpc::Status WitnessServiceImpl::GarbageCollect(grpc::ServerContext* context,
                                                 const witness::GCRequest* request,
                                                 witness::GCReply* reply) {
    spdlog::debug("WitnessServiceImpl::GarbageCollect: master_id={}, entries={}",
                 request->master_id(), request->entries().size());
    
    // 转换条目
    std::vector<GCEntry> entries;
    entries.reserve(request->entries().size());
    
    for (const auto& entry : request->entries()) {
        entries.push_back({entry.key(), entry.rpc_id()});
    }
    
    // 调用 Witness
    uint32_t dropped = witness_->garbage_collect(request->master_id(), entries);
    
    reply->set_success(true);
    reply->set_dropped_count(dropped);
    
    return grpc::Status::OK;
}

grpc::Status WitnessServiceImpl::GetRecoveryData(grpc::ServerContext* context,
                                                  const witness::RecoveryRequest* request,
                                                  witness::RecoveryData* reply) {
    spdlog::info("WitnessServiceImpl::GetRecoveryData: master_id={}",
                request->master_id());
    
    auto records = witness_->get_recovery_data(request->master_id());
    
    reply->set_success(true);
    
    for (const auto& record : records) {
        auto* req = reply->add_requests();
        req->set_rpc_id(record.rpc_id);
        req->set_key(record.key);
        req->set_request_data(record.request_data.data(), record.request_data.size());
        req->set_client_id(record.client_id);
        req->set_timestamp(record.timestamp);
    }
    
    return grpc::Status::OK;
}

grpc::Status WitnessServiceImpl::Stop(grpc::ServerContext* context,
                                       const witness::StopRequest* request,
                                       witness::StopReply* reply) {
    spdlog::info("WitnessServiceImpl::Stop: master_id={}", request->master_id());
    
    witness_->stop_accepting();
    
    auto status = witness_->get_status();
    reply->set_success(true);
    reply->set_request_count(status.request_count);
    
    return grpc::Status::OK;
}

grpc::Status WitnessServiceImpl::Reset(grpc::ServerContext* context,
                                        const witness::ResetRequest* request,
                                        witness::ResetReply* reply) {
    spdlog::info("WitnessServiceImpl::Reset: master_id={}, new_master_id={}",
                request->master_id(), request->new_master_id());
    
    witness_->reset(request->new_master_id());
    
    reply->set_success(true);
    
    return grpc::Status::OK;
}

grpc::Status WitnessServiceImpl::GetStatus(grpc::ServerContext* context,
                                            const witness::StatusRequest* request,
                                            witness::StatusReply* reply) {
    auto status = witness_->get_status();
    
    reply->set_master_id(status.master_id);
    reply->set_accepting(status.accepting);
    reply->set_request_count(status.request_count);
    
    if (status.oldest_timestamp.has_value()) {
        reply->set_oldest_timestamp(status.oldest_timestamp.value());
    }
    
    return grpc::Status::OK;
}

// ========== WitnessClient ==========

WitnessClient::WitnessClient(const std::string& target)
    : stub_(witness::WitnessService::NewStub(
          grpc::CreateChannel(target, grpc::InsecureChannelCredentials()))) {
    spdlog::info("WitnessClient: 连接到 {}", target);
}

RecordResult WitnessClient::record(const std::string& master_id, uint64_t rpc_id,
                                    const std::string& key,
                                    const std::vector<uint8_t>& request_data,
                                    uint64_t client_id) {
    witness::RecordRequest request;
    request.set_master_id(master_id);
    request.set_rpc_id(rpc_id);
    request.set_key(key);
    request.set_request_data(request_data.data(), request_data.size());
    request.set_client_id(client_id);
    
    witness::RecordReply reply;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->Record(&context, request, &reply);
    
    if (!status.ok()) {
        spdlog::error("WitnessClient::Record RPC 失败: {}", status.error_message());
        return RecordResult::REJECTED_STOPPED;
    }
    
    if (reply.accepted()) {
        return RecordResult::ACCEPTED;
    }
    
    switch (reply.reject_reason()) {
        case 1: return RecordResult::REJECTED_NOT_COMMUTATIVE;
        case 2: return RecordResult::REJECTED_NO_SPACE;
        case 3: return RecordResult::REJECTED_STOPPED;
        case 4: return RecordResult::REJECTED_WRONG_MASTER;
        default: return RecordResult::REJECTED_STOPPED;
    }
}

uint32_t WitnessClient::garbage_collect(const std::string& master_id,
                                         const std::vector<GCEntry>& entries) {
    witness::GCRequest request;
    request.set_master_id(master_id);
    
    for (const auto& entry : entries) {
        auto* e = request.add_entries();
        e->set_key(entry.key);
        e->set_rpc_id(entry.rpc_id);
    }
    
    witness::GCReply reply;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->GarbageCollect(&context, request, &reply);
    
    if (!status.ok()) {
        spdlog::error("WitnessClient::GarbageCollect RPC 失败: {}", status.error_message());
        return 0;
    }
    
    return reply.dropped_count();
}

std::vector<WitnessRecord> WitnessClient::get_recovery_data(const std::string& master_id) {
    witness::RecoveryRequest request;
    request.set_master_id(master_id);
    
    witness::RecoveryData reply;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->GetRecoveryData(&context, request, &reply);
    
    if (!status.ok()) {
        spdlog::error("WitnessClient::GetRecoveryData RPC 失败: {}", status.error_message());
        return {};
    }
    
    std::vector<WitnessRecord> records;
    records.reserve(reply.requests().size());
    
    for (const auto& req : reply.requests()) {
        WitnessRecord record;
        record.rpc_id = req.rpc_id();
        record.key = req.key();
        record.request_data.assign(req.request_data().begin(), req.request_data().end());
        record.client_id = req.client_id();
        record.timestamp = req.timestamp();
        records.push_back(record);
    }
    
    return records;
}

bool WitnessClient::stop(const std::string& master_id) {
    witness::StopRequest request;
    request.set_master_id(master_id);
    
    witness::StopReply reply;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->Stop(&context, request, &reply);
    
    return status.ok() && reply.success();
}

bool WitnessClient::reset(const std::string& new_master_id) {
    witness::ResetRequest request;
    request.set_new_master_id(new_master_id);
    
    witness::ResetReply reply;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->Reset(&context, request, &reply);
    
    return status.ok() && reply.success();
}

Witness::Status WitnessClient::get_status() {
    witness::StatusRequest request;
    witness::StatusReply reply;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->GetStatus(&context, request, &reply);
    
    if (!status.ok()) {
        return {};
    }
    
    Witness::Status result;
    result.master_id = reply.master_id();
    result.accepting = reply.accepting();
    result.request_count = reply.request_count();
    
    // proto3 scalar types don't have has_* methods, check for non-zero
    if (reply.oldest_timestamp() != 0) {
        result.oldest_timestamp = reply.oldest_timestamp();
    }
    
    return result;
}

} // namespace curp
