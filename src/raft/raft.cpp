#include "../../include/raft/raft.h"
#include "../../include/consts.h"
#include "../../include/raft/raft_rpc.h"
#include "../../include/utils/utils.h"

#include "spdlog/spdlog.h"
#include <cstdio>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

RaftNode::~RaftNode() {
  kill(); // 设置 is_dead
          // 等待所有线程退出（如 ticker、SendHeartBeats、gRPC server）
          // 可以用 joinable thread 或条件变量实现
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
  log.push_back(raft::Entry{}); // 添加一个空的初始日志条目
  // TODO: 初始化 apply 相关的通信组件
}

void RaftNode::ResetVoteTimer() {
  int rdTimeout = GetRandomElectTimeOut(MinElectTimeOut, MaxElectTimeOut);
  voteTimer.reset(std::chrono::milliseconds(rdTimeout));
}

void RaftNode::ResetHeartTimer(int timeout) {
  voteTimer.reset(std::chrono::milliseconds(timeout));
}

void RaftNode::collectVote(int config_id, raft::RequestVoteArgs args,
                           std::shared_ptr<std::mutex> voteMtx,
                           std::shared_ptr<int> voteCount) {
  NodeConfig &target_config = this->cluster_configs[config_id];
  bool get_vote =
      this->raft_service_impl->GetVoteAnswer(target_config.addr, args);

  if (!get_vote) {
    spdlog::info("server {} 收到来自 server {} 的投票回复, 但未获得投票",
                 cur_node_id, config_id);
    return;
  }

  voteMtx->lock();
  if (*voteCount > cluster_configs.size() / 2) {
    // 已经获得大多数投票, 不需要再继续投票
    voteMtx->unlock();
    return;
  }

  *voteCount += 1;

  spdlog::info(
      "server {} 收到来自 server {} 的投票回复, 获得投票, 当前选票数: {}",
      cur_node_id, config_id, *voteCount);

  if (*voteCount > cluster_configs.size() / 2) {
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
      voteMtx->unlock();
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
  voteMtx->unlock();
}

void RaftNode::Elect() {
  std::unique_lock<std::mutex> lock(mtx);

  currentTerm += 1;            // 自增term
  role = RaftState::CANDIDATE; // 成为候选人
  votedFor = cur_node_id;      // 投票给自己
  persist();                   // 持久化当前状态

  std::shared_ptr<int> voteCount = std::make_shared<int>(1); // 自己有一票
  std::shared_ptr<std::mutex> voteMtx =
      std::make_shared<std::mutex>(); // 针对此次选举的互斥锁

  spdlog::info(
      "RaftNode::Elect: server {} 开始发起新一轮投票, 新一轮的 term为: {}",
      cur_node_id, currentTerm);

  raft::RequestVoteArgs args;
  args.set_term(currentTerm);
  args.set_candidateid(this->cur_node_id);
  args.set_lastlogindex(VirtualLogIdx(log.size() - 1));
  args.set_lastlogterm(log.rbegin()->term());

  for (int i = 0; i < cluster_configs.size(); i++) {
    if (i == cur_node_id) {
      continue;
    }
    // TODO: 直接临时创建一个线程, 后续可以考虑使用线程池来优化
    std::thread(&RaftNode::collectVote, this, i, args, voteMtx, voteCount)
        .detach();
  }
}

void RaftNode::ticker() {
  while (!killed()) {
    voteTimer.wait();

    std::unique_lock<std::mutex> lock(mtx);
    if (role != RaftState::LEADER) {
      // 将elect单独作为一个线程运行
      std::thread(&RaftNode::Elect, this).detach();
    }
    ResetVoteTimer();
  }
}

void RaftNode::persist() {
  // TODO: 实现状态持久化逻辑
}

uint64_t RaftNode::VirtualLogIdx(uint64_t physical_idx) {
  // 调用该函数需要是加锁的状态
  return physical_idx + lastIncludedIndex;
}

uint64_t RaftNode::RealLogIdx(uint64_t virtual_idx) {
  // 调用该函数需要是加锁的状态
  return virtual_idx - lastIncludedIndex;
}

enum RaftState RaftNode::getRole() {
  std::unique_lock<std::mutex> lock(mtx);
  return role;
}

void RaftNode::handleAppendEntries(int target_node,
                                   raft::AppendEntriesArgs args) {
  raft::AppendEntriesReply reply;
  NodeConfig &target_config = cluster_configs[target_node];

  bool rpc_success =
      raft_service_impl->sendAppendEntries(target_config.addr, args, &reply);

  if (!rpc_success) {
    return;
  }

  std::unique_lock<std::mutex> lock(mtx);

  if (role != RaftState::LEADER || args.term() != currentTerm) {
    // 函数调用间隙值变了, 已经不是发起这个调用时的term了
    // 要先判断term是否改变, 否则后续的更改matchIndex等是不安全的
    return;
  }

  if (reply.success()) {
    int newMatchIdx = args.prevlogindex() + args.entries().size();
    if (newMatchIdx > matchIndex[target_node]) {
      // 有可能在此期间让follower安装了快照, 导致 rf.matchIndex[serverTo]
      // 本来就更大
      matchIndex[target_node] = newMatchIdx;
    }
    nextIndex[target_node] = matchIndex[target_node] + 1;

    // 需要判断是否可以commit
    int N = VirtualLogIdx(log.size() - 1);

    spdlog::info("RaftNode::handleAppendEntries: leader {} "
                 "确定N以决定新的commitIndex, lastIncludedIndex={}, "
                 "commitIndex={}",
                 cur_node_id, lastIncludedIndex, commitIndex);

    while (N > commitIndex) {
      int count = 1; // 包括leader自己
      for (int i = 0; i < cluster_configs.size(); ++i) {
        if (i == cur_node_id)
          continue;
        if (matchIndex[i] >= N && log[RealLogIdx(N)].term() == currentTerm) {
          count += 1;
        }
      }
      if (count > cluster_configs.size() / 2) {
        // 如果至少一半的follower回复了成功, 更新commitIndex
        break;
      }
      N--;
    }

    commitIndex = N;
    // TODO: 唤醒检查commit的线程或协程

    return;
  }

  if (reply.term() > currentTerm) {
    spdlog::info("RaftNode::handleAppendEntries: server {} "
                 "旧的leader收到了来自 server {} "
                 "的心跳函数中更新的term: {}, 转化为Follower",
                 cur_node_id, target_node, reply.term());

    currentTerm = reply.term();
    role = RaftState::FOLLOWER;
    votedFor = -1;
    ResetVoteTimer();
    persist();
    return;
  }

  if (reply.term() == currentTerm && role == RaftState::LEADER) {
    // term仍然相同, 且自己还是leader,
    // 表明对应的follower在prevLogIndex位置没有与prevLogTerm匹配的项
    // 快速回退的处理

    if (reply.xterm() == -1) {
      // PrevLogIndex这个位置在Follower中不存在
      spdlog::info("leader {} 收到 server {} 的回退请求, 原因是log过短, "
                   "回退前的nextIndex[{}]={}, 回退后的nextIndex[{}]={}",
                   cur_node_id, target_node, target_node,
                   nextIndex[target_node], target_node, reply.xlen());

      if (lastIncludedIndex >= reply.xlen()) {
        // 由于 snapshot 被截断，下次发送 InstallSnapshot
        nextIndex[target_node] = lastIncludedIndex;
      } else {
        nextIndex[target_node] = reply.xlen();
      }
      return;
    }

    int i = nextIndex[target_node] - 1;
    if (i < lastIncludedIndex) {
      i = lastIncludedIndex;
    }

    while (i > lastIncludedIndex && log[RealLogIdx(i)].term() > reply.xterm()) {
      i--;
    }

    if (i == lastIncludedIndex && log[RealLogIdx(i)].term() > reply.xterm()) {
      // 要找的位置已经由于 snapshot 被截断，下次发送 InstallSnapshot
      nextIndex[target_node] = lastIncludedIndex;
    } else if (log[RealLogIdx(i)].term() == reply.xterm()) {
      // 之前PrevLogIndex发生冲突位置时, Follower的Term自己也有
      spdlog::info("leader {} 收到 server {} 的回退请求, 冲突位置的Term为{}, "
                   "server的这个Term从索引{}开始, "
                   "而leader对应的最后一个XTerm索引为{}, "
                   "回退前的nextIndex[{}]={}, 回退后的nextIndex[{}]={}",
                   cur_node_id, target_node, reply.xterm(), reply.xindex(), i,
                   target_node, nextIndex[target_node], target_node, i + 1);
      nextIndex[target_node] = i + 1;
    } else {
      // 之前PrevLogIndex发生冲突位置时, Follower的Term自己没有
      spdlog::info("leader {} 收到 server {} 的回退请求, 冲突位置的Term为{}, "
                   "server的这个Term从索引{}开始, "
                   "而leader对应的XTerm不存在, 回退前的nextIndex[{}]={}, "
                   "回退后的nextIndex[{}]={}",
                   cur_node_id, target_node, reply.xterm(), reply.xindex(),
                   target_node, nextIndex[target_node], target_node,
                   reply.xindex());
      if (reply.xindex() <= lastIncludedIndex) {
        // XIndex位置也被截断了
        // 添加InstallSnapshot的处理
        nextIndex[target_node] = lastIncludedIndex;
      } else {
        nextIndex[target_node] = reply.xindex();
      }
    }
    return;
  }
}

void RaftNode::SendHeartBeats() {
  spdlog::info("server {} 开始发送心跳包", cur_node_id);

  while (!killed()) {
    // TODO: 实现心跳包发送逻辑
    heartTimer.wait();
    std::unique_lock<std::mutex> lock(mtx);

    if (role != RaftState::LEADER) {
      // 如果不是leader, 则不发送心跳包
      spdlog::info("server {} 不是leader, 不发送心跳包", cur_node_id);
      return;
    }

    for (int i = 0; i < cluster_configs.size(); i++) {
      if (i == cur_node_id) {
        continue; // 不向自己发送心跳包
      }
      NodeConfig &target_config = cluster_configs[i];

      raft::AppendEntriesArgs args;
      args.set_term(currentTerm);
      args.set_leaderid(cur_node_id);
      args.set_prevlogindex(nextIndex[i] - 1);
      args.set_leadercommit(commitIndex);

      bool sendInstallSnapshot = false;

      if (args.prevlogindex() < lastIncludedIndex) {
        spdlog::info("RaftNode::SendHeartBeats: leader {} 取消向 server {} "
                     "广播新的心跳, "
                     "改为发送sendInstallSnapshot, lastIncludedIndex={}, "
                     "nextIndex[{}]={}",
                     cur_node_id, i, lastIncludedIndex, i, nextIndex[i]);

        sendInstallSnapshot = true;
      } else if (VirtualLogIdx(log.size() - 1) > args.prevlogindex()) {
        // 如果有新的log需要发送, 则就是一个真正的AppendEntries而不是心跳
        int startIndex = RealLogIdx(args.prevlogindex() + 1);
        args.mutable_entries()->Assign(log.begin() + startIndex, log.end());

        spdlog::info("leader {} 开始向 server {} 广播新的AppendEntries, "
                     "lastIncludedIndex={}, nextIndex[{}]={}, PrevLogIndex={}, "
                     "len(Entries) = {}",
                     cur_node_id, i, lastIncludedIndex, i, nextIndex[i],
                     args.prevlogindex(), args.entries().size());
      }

      if (sendInstallSnapshot) {
        // TODO: 启动 InstallSnapshot 线程
      } else {
        args.set_prevlogterm(log[RealLogIdx(args.prevlogindex())].term());
        // 启动 handleAppendEntries 线程
        // TODO: 线程池优化
        std::thread(&RaftNode::handleAppendEntries, this, i, args).detach();
      }
    }
    ResetHeartTimer(HeartBeatTimeOut);
  }
}

void RaftNode::handleInstallSnapshot(int config_id) {
  // TODO: 实现InstallSnapshot逻辑
}

bool RaftNode::killed() { return is_dead.load(); }
void RaftNode::kill() { is_dead.store(true); }
