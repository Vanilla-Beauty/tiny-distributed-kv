#include "../include/raft/raft.h"
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

// 构造一个简单的3节点集群，测试选举功能
TEST(RaftElectionTest, BasicElection) {
  // 构造3个节点配置
  std::vector<NodeConfig> configs = {
      {"localhost:8745"}, {"localhost:8746"}, {"localhost:8747"}};

  // 创建3个RaftNode实例（用shared_ptr管理）
  std::vector<std::shared_ptr<RaftNode>> nodes;
  for (int i = 0; i < 3; ++i) {
    nodes.push_back(RaftNode::Create(configs, "store_" + std::to_string(i), i));
  }

  // 等待一段时间让选举发生
  std::this_thread::sleep_for(std::chrono::seconds(5));

  // 检查是否有且只有一个leader
  int leader_count = 0;
  int leader_id = -1;
  for (int i = 0; i < 3; ++i) {
    if (nodes[i]->getRole() == RaftState::LEADER) {
      leader_count++;
      leader_id = i;
    }
  }
  EXPECT_EQ(leader_count, 1) << "应该有且只有一个leader";
  EXPECT_NE(leader_id, -1) << "必须有一个leader";

  for (int i = 0; i < 3; ++i) {
    nodes[i]->kill();
  }

  std::cout << "测试结束" << std::endl;
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}