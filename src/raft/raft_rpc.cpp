#include "../../include/raft/raft_rpc.h"
#include "../../include/raft/raft.h"
#include "spdlog/spdlog.h"
#include <cstdio>
#include <mutex>

raft::Entry Entry2GrpcEntry(const raft::Entry &entry) {
  raft::Entry grpc_entry;

  return grpc_entry;
}

std::mutex RaftServiceImpl::s_mutex;
std::unordered_map<RaftNode *, std::weak_ptr<RaftServiceImpl>>
    RaftServiceImpl::s_instances;

RaftServiceImpl::RaftServiceImpl(std::shared_ptr<RaftNode> node)
    : raft_node(std::move(node)) {}

RaftServiceImpl::~RaftServiceImpl() {
  if (server_)
    server_->Shutdown();
  if (server_thread_.joinable())
    server_thread_.join();
}
void RaftServiceImpl::RunServer(const std::string &address) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(this);
  server_ = builder.BuildAndStart();
  spdlog::info("Server {}  listening on {}", this->raft_node->cur_node_id,
               address.c_str());
  server_->Wait();
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
    if (request->lastlogterm() > raft_node->log.rbegin()->term() ||
        (request->lastlogterm() == raft_node->log.rbegin()->term() &&
         request->lastlogindex() >=
             raft_node->VirtualLogIdx(raft_node->log.size() - 1))) {
      // 2. If votedFor is null or candidateId, and candidate’s log is least as
      // up-to-date as receiver’s log, grant vote (§5.2, §5.4)
      raft_node->currentTerm = request->term();
      reply->set_term(raft_node->currentTerm);
      raft_node->votedFor = request->candidateid();
      raft_node->role = RaftState::FOLLOWER;
      raft_node->ResetVoteTimer(); // 投票后重置投票定时器
      raft_node->persist();        // 持久化状态

      reply->set_votegranted(true);
      spdlog::info("server {} 同意向 server {} 投票: term: {}",
                   raft_node->cur_node_id, request->candidateid(),
                   request->term());
      return grpc::Status::OK;
    } else {
      if (request->lastlogterm() < raft_node->log.rbegin()->term()) {
        spdlog::info("server {} 拒绝向 server {} 投票: 旧的日志term: {}, "
                     "当前日志term: {}",
                     raft_node->cur_node_id, request->candidateid(),
                     request->lastlogterm(), raft_node->log.rbegin()->term());
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

grpc::Status
RaftServiceImpl::AppendEntries(grpc::ServerContext *context,
                               const raft::AppendEntriesArgs *request,
                               raft::AppendEntriesReply *reply) {
  std::unique_lock<std::mutex> lock(raft_node->mtx);

  if (request->term() < raft_node->currentTerm) {
    // 1. Reply false if term < currentTerm (§5.1)
    // 有2种情况:
    // - 这是真正的来自旧的leader的消息
    // - 当前节点是一个孤立节点, 因为持续增加 currentTerm 进行选举,
    // 因此真正的leader返回了更旧的term
    reply->set_term(raft_node->currentTerm);
    reply->set_success(false);

    spdlog::info("RaftServiceImpl::AppendEntries: server {} 收到来自 server {} "
                 "的AppendEntries请求, "
                 "拒绝: 旧的term: {}, 当前term: {}",
                 raft_node->cur_node_id, request->leaderid(), request->term(),
                 raft_node->currentTerm);
    return grpc::Status::OK;
  }

  // 代码执行到这里就是 args.Term >= rf.currentTerm 的情况

  // 收到心跳后需要重置定时器
  raft_node->ResetVoteTimer();

  if (request->term() > raft_node->currentTerm) {
    // 新leader的第一个消息
    raft_node->currentTerm = request->term(); // 更新到更新的term
    raft_node->votedFor = -1;                 // 重置投票状态
    raft_node->role = RaftState::FOLLOWER;    // 转为follower角色
    raft_node->persist();                     // 持久化状态
  }

  if (request->entries().empty()) {
    // 单纯的心跳
    spdlog::info("RaftServiceImpl::AppendEntries: server {} 接收到 leader {} "
                 "的心跳, 自身lastIncludedIndex={}, "
                 "PrevLogIndex={}, len(Entries) = {}",
                 raft_node->cur_node_id, request->leaderid(),
                 raft_node->lastIncludedIndex, request->prevlogindex(),
                 request->entries().size());
  } else {
    spdlog::info("RaftServiceImpl::AppendEntries: server {} 收到 leader {} "
                 "的AppendEntries, "
                 "自身lastIncludedIndex={}, PrevLogIndex={}, len(Entries)= {}",
                 raft_node->cur_node_id, request->leaderid(),
                 raft_node->lastIncludedIndex, request->prevlogindex(),
                 request->entries().size());
  }

  bool isConflict = false;

  // 校验PrevLogIndex和PrevLogTerm不合法
  // 2. Reply false if log doesn’t contain an entry at prevLogIndex whose term
  // matches prevLogTerm (§5.3)
  if (request->prevlogindex() < raft_node->lastIncludedIndex) {
    // 过时的RPC, 其 PrevLogIndex 甚至在lastIncludedIndex之前
    reply->set_term(raft_node->currentTerm);
    reply->set_success(true);
    return grpc::Status::OK;
  } else if (request->prevlogindex() >=
             raft_node->VirtualLogIdx(raft_node->log.size())) {
    // PrevLogIndex位置不存在日志项
    reply->set_xterm(-1);
    reply->set_xlen(raft_node->VirtualLogIdx(
        raft_node->log.size())); // Log长度, 包括了已经snapShot的部分
    isConflict = true;

    spdlog::info("RaftServiceImpl::AppendEntries: server {} "
                 "的log在PrevLogIndex: {} 位置不存在日志项, Log长度为{}",
                 raft_node->cur_node_id, request->prevlogindex(),
                 reply->xlen());
  } else if (raft_node->log[raft_node->RealLogIdx(request->prevlogindex())]
                 .term() != request->prevlogterm()) {
    // PrevLogIndex位置的日志项存在, 但term不匹配
    reply->set_xterm(
        raft_node->log[raft_node->RealLogIdx(request->prevlogindex())].term());
    int i = request->prevlogindex();
    while (i > raft_node->commitIndex &&
           raft_node->log[raft_node->RealLogIdx(i)].term() == reply->xterm()) {
      i--; // 向前查找直到找到term不匹配的日志项
    }
    reply->set_xindex(i + 1); // 返回第一个term不匹配的日志项的索引
    reply->set_xlen(raft_node->VirtualLogIdx(
        raft_node->log.size())); // Log长度, 包括了已经snapShot的部分
    isConflict = true;

    spdlog::info("RaftServiceImpl::AppendEntries: server {} "
                 "的log在PrevLogIndex: {} 位置Term不匹配, args.Term={}, "
                 "实际的term={}\n",
                 raft_node->cur_node_id, request->prevlogindex(),
                 request->prevlogterm(), reply->xterm());
  }
  if (isConflict) {
    // 如果有冲突, 则需要回复冲突信息
    reply->set_term(raft_node->currentTerm);
    reply->set_success(false);

    return grpc::Status::OK;
  }

  // 3. If an existing entry conflicts with a new one (same index
  // but different terms), delete the existing entry and all that
  // follow it (§5.3)

  for (int idx = 0; idx < request->entries().size(); ++idx) {
    const raft::Entry &log = request->entries()[idx];
    int ridx = raft_node->RealLogIdx(request->prevlogindex()) + 1 + idx;
    if (ridx < raft_node->log.size() &&
        raft_node->log[ridx].term() != log.term()) {
      // 某位置发生了冲突, 覆盖这个位置开始的所有内容
      // ! TODO 更新api
      raft_node->log.truncate_from(ridx);
      // raft_node->log.erase(raft_node->log.begin() + ridx,
      // raft_node->log.end()); 追加剩余的 entries
      for (int j = idx; j < request->entries().size(); ++j) {
        raft_node->log.push_back(request->entries()[j]);
      }
      break;
    } else if (ridx == raft_node->log.size()) {
      // 没有发生冲突但长度更长了, 直接拼接
      for (int j = idx; j < request->entries().size(); ++j) {
        raft_node->log.push_back(request->entries()[j]);
      }
      break;
    }
  }

  if (!request->entries().empty()) {
    spdlog::info("RaftServiceImpl::AppendEntries: server {} 成功进行apeend, "
                 "lastApplied={}, len(log)={}\n",
                 raft_node->cur_node_id, raft_node->lastApplied,
                 raft_node->log.size());
  }

  // 4. Append any new entries not already in the log
  // 补充apeend的业务	// 4. Append any new entries not already in the log
  // 补充apeend的业务
  raft_node->persist();

  reply->set_success(true);
  reply->set_term(raft_node->currentTerm);

  if (request->leadercommit() > raft_node->commitIndex) {
    // 5.If leaderCommit > commitIndex, set commitIndex = min(leaderCommit,
    // index of last new entry)
    if (request->leadercommit() >
        raft_node->VirtualLogIdx(raft_node->log.size() - 1)) {
      raft_node->commitIndex =
          raft_node->VirtualLogIdx(raft_node->log.size() - 1);
    } else {
      raft_node->commitIndex = request->leadercommit();
    }
    spdlog::info("RaftServiceImpl::AppendEntries: server %v "
                 "唤醒检查commit的协程, commitIndex=%v, len(log)=%v\n",
                 raft_node->cur_node_id, raft_node->commitIndex,
                 raft_node->log.size());

    //  TODO: 唤醒 commit 的线程
  }
  return grpc::Status::OK;
}

bool RaftServiceImpl::GetVoteAnswer(const std::string &address,
                                    const raft::RequestVoteArgs &ori_request) {
  spdlog::info(
      "RaftServiceImpl::GetVoteAnswer: server {} Send vote request to {}",
      raft_node->cur_node_id, address.c_str());

  std::shared_ptr<raft::Raft::Stub> stub = get_unique_stub(address);

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

bool RaftServiceImpl::sendAppendEntries(const std::string &address,
                                        const raft::AppendEntriesArgs &request,
                                        raft::AppendEntriesReply *reply) {
  std::shared_ptr<raft::Raft::Stub> stub = get_unique_stub(address);

  grpc::ClientContext context;

  spdlog::info("RaftServiceImpl::sendAppendEntries: server {} Send "
               "AppendEntries request to {}",
               raft_node->cur_node_id, address.c_str());
  auto status = stub->AppendEntries(&context, request, reply);

  return status.ok();
}

std::shared_ptr<raft::Raft::Stub>
RaftServiceImpl::get_unique_stub(const std::string &address) {
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
  return stub;
}