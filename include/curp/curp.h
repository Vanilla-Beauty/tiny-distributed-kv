#pragma once

#include "../../3rd_party/tiny-lsm/include/lsm/engine.h"
#include "../raft/raft.h"

class Witness {
  tiny_lsm::LSM store_engine;
};

class CurpNode {
  std::mutex mtx;
  uint64_t cur_node_id;

  Witness witness;
};