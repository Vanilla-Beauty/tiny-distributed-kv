#pragma once

#include <atomic>
#include <string>
#include <vector>

class RaftNode;
class RaftServiceImpl;
class NodeConfig {
  friend class RaftNode;
  friend class RaftServiceImpl;

  std::string addr;
  std::atomic<bool> is_running;

public:
  NodeConfig(std::string addr, bool is_running);
  NodeConfig(const NodeConfig &other)
      : addr(other.addr), is_running(other.is_running.load()) {}

  void set_status();
};