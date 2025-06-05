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

public:
  NodeConfig() = default;
  NodeConfig(std::string addr) : addr(std::move(addr)) {};
  NodeConfig(const NodeConfig &other) : addr(other.addr) {}

  void set_status();
};