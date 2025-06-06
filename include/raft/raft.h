#pragma once

// #include "../../3rd_party/tiny-lsm/include/lsm/engine.h"
#include "../../include/raft/raft_rpc.h"
#include "../../include/utils/timer.h"
#include "config.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class RaftState { FOLLOWER, CANDIDATE, LEADER };

std::string RaftStateToString(const RaftState &state);

class RaftServiceImpl;

class RaftNode : public std::enable_shared_from_this<RaftNode> {
  friend class RaftServiceImpl;

  std::shared_ptr<RaftServiceImpl> raft_service_impl;
  std::mutex mtx;
  std::vector<NodeConfig> cluster_configs;
  // std::shared_ptr<tiny_lsm::LSM> store_engine;
  std::string store_path;
  uint64_t cur_node_id;
  std::atomic<bool> is_dead;

  int currentTerm;              // 当前节点的Term
  int votedFor;                 // 当前节点投票给了哪个节点
  std::vector<raft::Entry> log; // 日志条目

  std::vector<uint64_t> nextIndex;
  std::vector<uint64_t> matchIndex;

  enum RaftState role;

  uint64_t commitIndex;
  uint64_t lastApplied;
  uint64_t lastIncludedIndex; // 日志中的最高索引
  uint64_t lastIncludedTerm;  // 日志中的最高Term

  Timer voteTimer;
  Timer heartTimer;

public:
  ~RaftNode();

  static std::shared_ptr<RaftNode>
  Create(std::vector<NodeConfig> cluster_configs, std::string store_path,
         uint64_t cur_node_id);

  enum RaftState getRole();
  void kill();

private:
  RaftNode(std::vector<NodeConfig> cluster_configs, std::string store_path,
           uint64_t cur_node_id);

  void checkVote();
  void ticker();
  void Elect();
  void persist();
  void SendHeartBeats();
  void handleInstallSnapshot(int config_id);
  void handleAppendEntries(int target_node, raft::AppendEntriesArgs args);

  void start_ticker();

  void ResetVoteTimer();
  void ResetHeartTimer(int timeout);
  void initVoteTimer();

  void collectVote(int config_id, raft::RequestVoteArgs args,
                   std::shared_ptr<std::mutex> voteMtx,
                   std::shared_ptr<int> voteCount);
  bool killed();

  uint64_t VirtualLogIdx(uint64_t physical_idx);
  uint64_t RealLogIdx(uint64_t virtual_idx);
};
