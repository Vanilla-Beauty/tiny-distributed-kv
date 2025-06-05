#include "../../include/curp/raft.h"
#include "../../include/consts.h"
#include "../../include/curp/raft_rpc.h"
#include "../../include/utils/utils.h"

#include "../../include/curp/raft.h"
#include "../../include/curp/raft_rpc.h"
#include "spdlog/spdlog.h"
#include <cstdio>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <thread>

std::string RaftStateToString(const RaftState &state) {
  switch (state) {
  case RaftState::FOLLOWER:
    return "FOLLOWER";
  case RaftState::CANDIDATE:
    return "CANDIDATE";
  case RaftState::LEADER:
    return "LEADER";
  default:
    return "UNKNOWN";
  }
}
std::shared_ptr<RaftNode>
RaftNode::Create(std::vector<NodeConfig> cluster_configs,
                 std::string store_path, uint64_t cur_node_id) {
  auto node = std::shared_ptr<RaftNode>(
      new RaftNode(cluster_configs, store_path, cur_node_id));
  node->raft_service_impl = RaftServiceImpl::GetOrCreate(node);
  // 在一个线程中运行 start_ticker
  std::thread(&RaftNode::start_ticker, node.get()).detach();

  return node;
}

void RaftNode::start_ticker() {
  std::this_thread::sleep_for(std::chrono::seconds(1));
  spdlog::info("server {} 开始选举定时器", cur_node_id);
  std::thread(&RaftNode::ticker, this).detach();
}

RaftNode::RaftNode(std::vector<NodeConfig> cluster_configs,
                   std::string store_path, uint64_t cur_node_id)
    : cluster_configs(cluster_configs), store_path(store_path),
      cur_node_id(cur_node_id), is_dead(false), currentTerm(0), votedFor(-1),
      commitIndex(0), lastApplied(0), lastIncludedIndex(0), lastIncludedTerm(0),
      role(RaftState::FOLLOWER) {
  // 初始化LSM存储引擎
  // store_engine = std::make_shared<tiny_lsm::LSM>(store_path);
  nextIndex.resize(cluster_configs.size(), 0);
  matchIndex.resize(cluster_configs.size(), 0);
  log.push_back(Entry{"", "", 0, 0}); // 添加一个空的初始日志条目
  // TODO: 初始化 apply 相关的通信组件
}

void RaftNode::resetVoteTimer() {
  int rdTimeout = GetRandomElectTimeOut(MinElectTimeOut, MaxElectTimeOut);
  voteTimer.reset(std::chrono::milliseconds(rdTimeout));
}

void RaftNode::collectVote(int config_id, raft::RequestVoteArgs args,
                           std::mutex &voteMtx, int &voteCount) {
  NodeConfig &target_config = this->cluster_configs[config_id];
  bool get_vote =
      this->raft_service_impl->GetVoteAnswer(target_config.addr, args);

  if (!get_vote) {
    spdlog::info("server {} 收到来自 server {} 的投票回复, 但未获得投票",
                 cur_node_id, config_id);
    return;
  }

  voteMtx.lock();
  if (voteCount > cluster_configs.size() / 2) {
    // 已经获得大多数投票, 不需要再继续投票
    voteMtx.unlock();
    return;
  }

  voteCount += 1;

  spdlog::info(
      "server {} 收到来自 server {} 的投票回复, 获得投票, 当前选票数: {}",
      cur_node_id, config_id, voteCount);

  if (voteCount > cluster_configs.size() / 2) {
    // 成功获得大多数投票
    std::unique_lock<std::mutex> lock(mtx);
    if (role != RaftState::CANDIDATE || currentTerm != args.term()) {
      // 有另外一个投票的线程收到了更新的term而更改了自身状态为Follower
      // 或者自己的term已经过期了, 也就是被新一轮的选举追上了
      spdlog::info(
          "server {} 在选举过程中发现状态已被更改, 可能是因为有新的选举发生, "
          "当前状态 : {}, 当前term : {}, args.term : {} ",
          cur_node_id, RaftStateToString(role), currentTerm, args.term());

      lock.unlock();
      voteMtx.unlock();
      return;
    }
    spdlog::info("server {} 成为了新的 leader", config_id);
    role = RaftState::LEADER;
    // 需要重新初始化nextIndex和matchIndex
    for (int i = 0; i < nextIndex.size(); i++) {
      nextIndex[i] = VirtualLogIdx(log.size());
      matchIndex[i] =
          lastIncludedIndex; // 由于matchIndex初始化为lastIncludedIndex,
                             // 因此在崩溃恢复后,
                             // 大概率触发InstallSnapshot RPC
    }
    lock.unlock();

    // 成为leader后, 需要启动发送心跳包的线程
    // TODO: 线程池优化
    std::thread(&RaftNode::SendHeartBeats, this).detach();
  }
  voteMtx.unlock();
}

void RaftNode::Elect() {
  std::unique_lock<std::mutex> lock(mtx);

  currentTerm += 1;            // 自增term
  role = RaftState::CANDIDATE; // 成为候选人
  votedFor = cur_node_id;      // 投票给自己
  persist();                   // 持久化当前状态

  int voteCount = 1;  // 自己有一票
  std::mutex voteMtx; // 针对此次选举的互斥锁

  spdlog::info(
      "RaftNode::Elect: server {} 开始发起新一轮投票, 新一轮的 term为: {}",
      cur_node_id, currentTerm);

  raft::RequestVoteArgs args;
  args.set_term(currentTerm);
  args.set_candidateid(this->cur_node_id);
  args.set_lastlogindex(VirtualLogIdx(log.size() - 1));
  args.set_lastlogterm(log.rbegin()->get_term());

  for (int i = 0; i < cluster_configs.size(); i++) {
    if (i == cur_node_id) {
      continue;
    }
    // TODO: 直接临时创建一个线程, 后续可以考虑使用线程池来优化
    std::thread(&RaftNode::collectVote, this, i, args, std::ref(voteMtx),
                std::ref(voteCount))
        .detach();
  }
}

void RaftNode::ticker() {
  while (!is_dead.load()) {
    voteTimer.wait();

    std::unique_lock<std::mutex> lock(mtx);
    if (role != RaftState::LEADER) {
      // 将elect单独作为一个线程运行
      std::thread(&RaftNode::Elect, this).detach();
    }
    resetVoteTimer();
  }
}

void RaftNode::persist() {
  // TODO: 实现状态持久化逻辑
}

uint64_t RaftNode::VirtualLogIdx(uint64_t physical_idx) {
  // 调用该函数需要是加锁的状态
  return physical_idx + lastIncludedIndex;
}

enum RaftState RaftNode::getRole() {
  std::unique_lock<std::mutex> lock(mtx);
  return role;
}

void RaftNode::SendHeartBeats() {
  spdlog::info("server {} 开始发送心跳包", cur_node_id);

  while (!killed()) {
    // TODO: 实现心跳包发送逻辑
  }
}

bool RaftNode::killed() { return is_dead.load(); }