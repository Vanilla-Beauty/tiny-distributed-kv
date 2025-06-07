#include "../include/storage/log_vec.h"
#include <filesystem>
#include <gtest/gtest.h>

class LogVecIteratorTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir = "./logvec_test_dir";
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
    std::filesystem::create_directory(test_dir);
  }

  void TearDown() override {
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
  }

  std::string test_dir;
};

// 构造测试日志条目
raft::Entry make_entry(int64_t seq, int32_t term, const std::string &key,
                       const std::string &value) {
  raft::Entry entry;
  entry.set_seq(seq);
  entry.set_term(term);
  entry.set_key(key);
  entry.set_value(value);
  return entry;
}

TEST_F(LogVecIteratorTest, ForwardIteration) {
  LogVec log(test_dir);

  // 写入5条日志
  for (int i = 0; i < 5; ++i) {
    log.push_back(make_entry(i, 100 + i, "key" + std::to_string(i),
                             "val" + std::to_string(i)));
  }
  log.sync();

  // 使用迭代器遍历并验证数据
  int index = 0;
  for (auto it = log.begin(); it != log.end(); ++it) {
    EXPECT_EQ(it->seq(), index);
    EXPECT_EQ(it->term(), 100 + index);
    EXPECT_EQ(it->key(), "key" + std::to_string(index));
    EXPECT_EQ(it->value(), "val" + std::to_string(index));
    ++index;
  }

  EXPECT_EQ(index, 5); // 确保遍历了全部5个元素
}

TEST_F(LogVecIteratorTest, ReverseIteration) {
  LogVec log(test_dir);

  // 写入5条日志
  for (int i = 0; i < 5; ++i) {
    log.push_back(make_entry(i, 100 + i, "key" + std::to_string(i),
                             "val" + std::to_string(i)));
  }
  log.sync();

  // 使用反向迭代器从尾到头读取
  int index = 4;
  for (auto rit = log.rbegin(); rit != log.rend(); ++rit) {
    EXPECT_EQ(rit->seq(), index);
    EXPECT_EQ(rit->term(), 100 + index);
    EXPECT_EQ(rit->key(), "key" + std::to_string(index));
    EXPECT_EQ(rit->value(), "val" + std::to_string(index));
    --index;
  }

  EXPECT_EQ(index, -1); // 确保遍历了全部5个元素（从4到0）
}

TEST_F(LogVecIteratorTest, IteratorDecrementBoundary) {
  LogVec log(test_dir);

  // 写入1条日志
  log.push_back(make_entry(0, 100, "key0", "val0"));
  log.sync();

  auto it = log.begin();
  --it;
  EXPECT_THROW(*it, std::out_of_range); // 不允许减到 begin() 之前
}

TEST_F(LogVecIteratorTest, EmptyLogIteration) {
  LogVec log(test_dir);

  // 日志为空时 begin() == end()
  EXPECT_EQ(log.begin(), log.end());

  // 反向迭代也应该是空
  EXPECT_EQ(log.rbegin(), log.rend());
}

TEST_F(LogVecIteratorTest, RangeForIteration) {
  LogVec log(test_dir);

  // 写入3条日志
  for (int i = 0; i < 3; ++i) {
    log.push_back(make_entry(i, 200 + i, "k" + std::to_string(i),
                             "v" + std::to_string(i)));
  }
  log.sync();

  int index = 0;
  for (const auto &entry : log) {
    EXPECT_EQ(entry.seq(), index);
    EXPECT_EQ(entry.term(), 200 + index);
    EXPECT_EQ(entry.key(), "k" + std::to_string(index));
    EXPECT_EQ(entry.value(), "v" + std::to_string(index));
    ++index;
  }

  EXPECT_EQ(index, 3);
}