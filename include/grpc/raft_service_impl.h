// raft_service_impl.h
#pragma once
#include "../../proto/raft.grpc.pb.h"
#include <grpcpp/grpcpp.h>

class RaftServiceImpl final : public raft::Raft::Service {
public:
  grpc::Status RequestVote(grpc::ServerContext *context,
                           const raft::RequestVoteArgs *request,
                           raft::RequestVoteReply *reply) override;
};
