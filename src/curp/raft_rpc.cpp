#include "../../include/curp/raft_rpc.h"
#include "../../include/curp/raft.h"
#include <cstdio>
#include <mutex>

RaftServiceImpl::RaftServiceImpl(std::shared_ptr<RaftNode> node)
    : raft_node(std::move(node)) {}

void RaftServiceImpl::RunServer(const std::string &address) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(this);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << address << std::endl;
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
    printf("server %lu 拒绝向 server %d 投票: 旧的term: %d\n",
           raft_node->cur_node_id, request->candidateid(), request->term());

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
             raft_node->virtualLogIdx(raft_node->log.size() - 1))) {
      // 2. If votedFor is null or candidateId, and candidate’s log is least as
      // up-to-date as receiver’s log, grant vote (§5.2, §5.4)
      raft_node->currentTerm = request->term();
      reply->set_term(raft_node->currentTerm);
      raft_node->votedFor = request->candidateid();
      raft_node->role = RaftState::FOLLOWER;
      raft_node->resetVoteTimer();
      raft_node->persist(); // 持久化状态

      reply->set_votegranted(true);
      printf("server %lu 同意向 server %d 投票: term: %d\n",
             raft_node->cur_node_id, request->candidateid(), request->term());
    } else {
      if (request->lastlogterm() < raft_node->log.rbegin()->get_term()) {
        printf("server %lu 拒绝向 server %d 投票: 旧的日志term: %d, "
               "当前日志term: %lu\n",
               raft_node->cur_node_id, request->candidateid(),
               request->lastlogterm(), raft_node->log.rbegin()->get_term());
      } else {
        printf("server %lu 拒绝向 server %d 投票: 旧的日志index: %d, "
               "当前日志index: %lu\n",
               raft_node->cur_node_id, request->candidateid(),
               request->lastlogindex(),
               raft_node->virtualLogIdx(raft_node->log.size() - 1));
      }
    }
  } else {
    printf("server %lu 拒绝向 server %d 投票: 已投票\n", raft_node->cur_node_id,
           request->candidateid());
  }

  reply->set_term(raft_node->currentTerm);
  reply->set_votegranted(false);

  return grpc::Status::OK;
}

bool RaftServiceImpl::GetVoteAnswer(const std::string &address,
                                    const raft::RequestVoteArgs &request,
                                    raft::RequestVoteReply *reply) {
  auto stub = raft::Raft::NewStub(
      grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;

  auto status = stub->RequestVote(&context, request, reply);

  if (!status.ok()) {
    return false;
  }

  std::unique_lock<std::mutex> lock(raft_node->mtx);

  if (raft_node->role != RaftState::CANDIDATE ||
      request.term() != raft_node->currentTerm) {
    // 易错点: 函数调用的间隙被修改了
    reply->set_votegranted(false);
    reply->set_term(raft_node->currentTerm);
    return false;
  }

  if (reply->term() > raft_node->currentTerm) {
    // 已经是过时的term了
    raft_node->currentTerm = reply->term();
    raft_node->role = RaftState::FOLLOWER;
    raft_node->votedFor = -1; // 重置投票状态
    raft_node->persist();     // 持久化状态
  }

  return reply->votegranted();
}
