#include "../../include/curp/raft.h"
#include "../../include/consts.h"
#include "../../include/curp/raft_rpc.h"
#include "../../include/utils/utils.h"

#include "../../include/curp/raft.h"
#include "../../include/curp/raft_rpc.h"
#include <cstdio>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <thread>

RaftNode::RaftNode(std::vector<NodeConfig> cluster_configs,
                   std::string store_path, uint64_t cur_node_id)
    : cluster_configs(cluster_configs), store_path(store_path),
      cur_node_id(cur_node_id), is_dead(false), currentTerm(0), votedFor(-1),
      commitIndex(0), lastApplied(0), lastIncludedIndex(0), lastIncludedTerm(0),
      role(RaftState::FOLLOWER) {
  // 初始化LSM存储引擎
  store_engine = std::make_shared<tiny_lsm::LSM>(store_path);
  nextIndex.resize(cluster_configs.size(), 0);
  matchIndex.resize(cluster_configs.size(), 0);
  log.push_back(Entry{"", "", 0, 0}); // 添加一个空的初始日志条目

  raft_service_impl = RaftServiceImpl::GetOrCreate(shared_from_this());

  // 启动 ticker 线程
  std::thread(&RaftNode::ticker, this);
}

void RaftNode::ticker() {
  while (!is_dead.load()) {
    while (voteTimer.wait()) {
      std::unique_lock<std::mutex> lock(mtx);
      if (role != RaftState::LEADER) {
        // 将elect单独作为一个线程运行
        std::thread(&RaftNode::elect, this).detach();
      }
      resetVoteTimer();
    }
  }
}

void RaftNode::resetVoteTimer() {
  int rdTimeout = GetRandomElectTimeOut(MinElectTimeOut, MaxElectTimeOut);
  voteTimer.reset(std::chrono::milliseconds(rdTimeout));
}

void RaftNode::elect() {
  std::unique_lock<std::mutex> lock(mtx);
  currentTerm += 1;            // 自增term
  role = RaftState::CANDIDATE; // 成为候选人

  persist(); // 持久化当前状态

  int voteCount = 1;  // 自己投票给自己
  std::mutex voteMtx; // 针对此次选举的互斥锁

  printf("server %lu 开始发起新一轮投票, 新一轮的 term为: %d\n", cur_node_id,
         currentTerm);

  raft::RequestVoteArgs args;
  args.set_term(currentTerm);
  args.set_candidateid(this->cur_node_id);
  args.set_lastlogindex(virtualLogIdx(log.size() - 1));
  args.set_lastlogterm(log.rbegin()->get_term());

  for (int i = 0; i < cluster_configs.size(); i++) {
    if (i == cur_node_id) {
      continue;
    }
    auto &config = cluster_configs[i];
    std::thread(&RaftNode::collectVote, this, i, args, std::ref(voteMtx),
                std::ref(voteCount))
        .detach();
  }
}

void RaftNode::persist() {
  // TODO: 实现状态持久化逻辑
}

uint64_t RaftNode::virtualLogIdx(uint64_t physical_idx) {
  // 调用该函数需要是加锁的状态
  return physical_idx + lastIncludedIndex;
}

void RaftNode::collectVote(int config_id, raft::RequestVoteArgs args,
                           std::mutex &voteMtx, int &voteCount) {
  // TODO: 实现收集投票的逻辑
}