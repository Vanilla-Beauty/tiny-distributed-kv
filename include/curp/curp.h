#pragma once

#include "raft.h"

class Witness {
  tiny_lsm::LSM store_engine;
};

class CurpNode {
  std::mutex mtx;
  uint64_t cur_node_id;

  Witness witness;
};