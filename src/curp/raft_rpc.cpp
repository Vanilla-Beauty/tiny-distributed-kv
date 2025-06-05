#include "../../include/curp/raft_rpc.h"
#include "../../include/curp/raft.h"
#include "spdlog/spdlog.h"
#include <cstdio>
#include <mutex>

std::mutex RaftServiceImpl::s_mutex;
std::unordered_map<RaftNode *, std::weak_ptr<RaftServiceImpl>>
    RaftServiceImpl::s_instances;

RaftServiceImpl::RaftServiceImpl(std::shared_ptr<RaftNode> node)
    : raft_node(std::move(node)) {}

void RaftServiceImpl::RunServer(const std::string &address) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(this);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  spdlog::info("Server {}  listening on {}", this->raft_node->cur_node_id,
               address.c_str());
  server->Wait();
}

void RaftServiceImpl::StartServer() {
  server_thread_ = std::thread([this]() {
    RunServer(raft_node->cluster_configs[raft_node->cur_node_id].addr);
  });
}

std::shared_ptr<RaftServiceImpl>
RaftServiceImpl::GetOrCreate(std::shared_ptr<RaftNode> node) {
  std::lock_guard<std::mutex> lock(s_mutex);
  auto it = s_instances.find(node.get());
  if (it != s_instances.end()) {
    if (auto inst = it->second.lock()) {
      return inst;
    }
  }
  auto inst = std::shared_ptr<RaftServiceImpl>(new RaftServiceImpl(node));
  s_instances[node.get()] = inst;
  inst->StartServer();
  return inst;
}

RaftServiceImpl::~RaftServiceImpl() {
  if (server_thread_.joinable())
    server_thread_.join();
}
grpc::Status RaftServiceImpl::RequestVote(grpc::ServerContext *context,
                                          const raft::RequestVoteArgs *request,
                                          raft::RequestVoteReply *reply) {
  std::unique_lock<std::mutex> lock(raft_node->mtx);

  if (request->term() < raft_node->currentTerm) {
    // 旧的term
    // 1. Reply false if term < currentTerm (§5.1)
    reply->set_term(raft_node->currentTerm);
    reply->set_votegranted(false);
    spdlog::info("server {} 拒绝向 server {} 投票: 旧的term: {}",
                 raft_node->cur_node_id, request->candidateid(),
                 request->term());

    return grpc::Status::OK;
  }

  // 代码到这里时, request.Term >= currentTerm

  if (request->term() > raft_node->currentTerm) {
    // 已经是新一轮的term, 之前的投票记录作废
    raft_node->currentTerm = request->term(); // 更新到更新的term
    raft_node->votedFor = -1;                 // 重置投票状态
    raft_node->role = RaftState::FOLLOWER;    // 转为follower角色
    raft_node->persist();                     // 持久化状态
  }

  // at least as up-to-date as receiver’s log, grant vote (§5.2, §5.4)
  if (raft_node->votedFor == -1 ||
      raft_node->votedFor == request->candidateid()) {
    // 首先确保是没投过票的
    if (request->lastlogterm() > raft_node->log.rbegin()->get_term() ||
        (request->lastlogterm() == raft_node->log.rbegin()->get_term() &&
         request->lastlogindex() >=
             raft_node->VirtualLogIdx(raft_node->log.size() - 1))) {
      // 2. If votedFor is null or candidateId, and candidate’s log is least as
      // up-to-date as receiver’s log, grant vote (§5.2, §5.4)
      raft_node->currentTerm = request->term();
      reply->set_term(raft_node->currentTerm);
      raft_node->votedFor = request->candidateid();
      raft_node->role = RaftState::FOLLOWER;
      raft_node->resetVoteTimer(); // 投票后重置投票定时器
      raft_node->persist();        // 持久化状态

      reply->set_votegranted(true);
      spdlog::info("server {} 同意向 server {} 投票: term: {}",
                   raft_node->cur_node_id, request->candidateid(),
                   request->term());
      return grpc::Status::OK;
    } else {
      if (request->lastlogterm() < raft_node->log.rbegin()->get_term()) {
        spdlog::info("server {} 拒绝向 server {} 投票: 旧的日志term: {}, "
                     "当前日志term: {}",
                     raft_node->cur_node_id, request->candidateid(),
                     request->lastlogterm(),
                     raft_node->log.rbegin()->get_term());
      } else {
        spdlog::info("server {} 拒绝向 server {} 投票: 旧的日志index: {}, "
                     "当前日志index: {}",
                     raft_node->cur_node_id, request->candidateid(),
                     request->lastlogindex(),
                     raft_node->VirtualLogIdx(raft_node->log.size() - 1));
      }
    }
  } else {
    spdlog::info("server {} 拒绝向 server {} 投票: 已投票",
                 raft_node->cur_node_id, request->candidateid());
  }

  reply->set_term(raft_node->currentTerm);
  reply->set_votegranted(false);

  return grpc::Status::OK;
}

bool RaftServiceImpl::GetVoteAnswer(const std::string &address,
                                    const raft::RequestVoteArgs &ori_request) {
  spdlog::info("server {} Send vote request to {}", raft_node->cur_node_id,
               address.c_str());

  std::shared_ptr<raft::Raft::Stub> stub;
  {
    std::lock_guard<std::mutex> lock(stub_cache_mtx_);
    auto it = stub_cache_.find(address);
    if (it == stub_cache_.end()) {
      auto channel =
          grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      stub = raft::Raft::NewStub(channel);
      stub_cache_[address] = stub;
    } else {
      stub = it->second;
    }
  }

  grpc::ClientContext context;
  raft::RequestVoteArgs new_request(
      ori_request); // 不修改原本的参数, 每次发送时复制一份
  raft::RequestVoteReply reply{};

  auto status = stub->RequestVote(&context, new_request, &reply);

  if (!status.ok()) {
    spdlog::info("GetVoteAnswer: server {} Send vote request to {} failed: {}",
                 raft_node->cur_node_id, address.c_str(),
                 status.error_message().c_str());

    return false;
  }

  std::unique_lock<std::mutex> lock(raft_node->mtx);

  if (raft_node->role != RaftState::CANDIDATE ||
      new_request.term() != raft_node->currentTerm) {
    spdlog::info("GetVoteAnswer: server {} 收到来自 {} 的投票回复, "
                 "但当前状态在rpc调用的间隙被修改了, 投票无效",
                 raft_node->cur_node_id, address.c_str());
    return false;
  }

  if (reply.term() > raft_node->currentTerm) {
    raft_node->currentTerm = reply.term();
    raft_node->role = RaftState::FOLLOWER;
    raft_node->votedFor = -1;
    raft_node->persist();

    spdlog::info(
        "GetVoteAnswer: server {} 收到来自 {} 的投票回复, term更新为 {}, "
        "未获取投票并转为FOLLOWER角色",
        raft_node->cur_node_id, address.c_str(), raft_node->currentTerm);
  }

  spdlog::info("server {} 收到来自 {} 的投票回复, reply.votegranted() = {}",
               raft_node->cur_node_id, address.c_str(), reply.votegranted());

  return reply.votegranted();
}
