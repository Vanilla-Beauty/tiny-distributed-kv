#pragma once

#include "../../3rd_party/tiny-lsm/include/lsm/engine.h"
#include "../../include/curp/raft_rpc.h"
#include "../../include/utils/timer.h"
#include "config.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class RaftState { FOLLOWER, CANDIDATE, LEADER };

class Entry {
  std::string key;
  std::string value;
  uint64_t term;  // 日志条目的Term
  uint64_t index; // 日志条目的索引
public:
  Entry(std::string key, std::string value, uint64_t term, uint64_t index)
      : key(key), value(value), term(term), index(index) {}

  // Getter methods for Entry
  std::string get_key() const { return key; }
  std::string get_value() const { return value; }
  uint64_t get_term() const { return term; }
  uint64_t get_index() const { return index; }
};

class RaftServiceImpl;

class RaftNode : public std::enable_shared_from_this<RaftNode> {
  friend class RaftServiceImpl;

  std::shared_ptr<RaftServiceImpl> raft_service_impl;
  std::mutex mtx;
  std::vector<NodeConfig> cluster_configs;
  std::shared_ptr<tiny_lsm::LSM> store_engine;
  std::string store_path;
  uint64_t cur_node_id;
  std::atomic<bool> is_dead;

  int currentTerm;        // 当前节点的Term
  int votedFor;           // 当前节点投票给了哪个节点
  std::vector<Entry> log; // 日志条目

  std::vector<uint64_t> nextIndex;
  std::vector<uint64_t> matchIndex;

  enum RaftState role;

  uint64_t commitIndex;
  uint64_t lastApplied;
  uint64_t lastIncludedIndex; // 日志中的最高索引
  uint64_t lastIncludedTerm;  // 日志中的最高Term

  Timer voteTimer;

public:
  RaftNode(std::vector<NodeConfig> cluster_configs, std::string store_path,
           uint64_t cur_node_id);

private:
  void checkVote();
  void ticker();
  void elect();
  void persist();
  void resetVoteTimer();
  void initVoteTimer();
  void collectVote(int config_id, raft::RequestVoteArgs args,
                   std::mutex &voteMtx, int &voteCount);

  uint64_t virtualLogIdx(uint64_t physical_idx);
};
