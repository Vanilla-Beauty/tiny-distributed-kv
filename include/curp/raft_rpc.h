#pragma once

#include "../../proto/raft.grpc.pb.h"
#include "../../proto/raft.pb.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <thread>

class RaftNode;

class RaftServiceImpl final : public raft::Raft::Service {
private:
  explicit RaftServiceImpl(std::shared_ptr<RaftNode> node);
  static std::mutex s_mutex;
  static std::unordered_map<RaftNode *, std::weak_ptr<RaftServiceImpl>>
      s_instances;

public:
  ~RaftServiceImpl();

  static std::shared_ptr<RaftServiceImpl>
  GetOrCreate(std::shared_ptr<RaftNode> node);

  grpc::Status RequestVote(grpc::ServerContext *context,
                           const raft::RequestVoteArgs *request,
                           raft::RequestVoteReply *reply) override;

  bool GetVoteAnswer(const std::string &address,
                     const raft::RequestVoteArgs &request);

private:
  void StartServer();
  void RunServer(const std::string &address);

private:
  std::shared_ptr<RaftNode> raft_node;
  std::thread server_thread_;
  std::unordered_map<std::string, std::shared_ptr<raft::Raft::Stub>>
      stub_cache_;
  std::mutex stub_cache_mtx_;
};