#pragma once

#include "../../3rd_party/tiny-lsm/include/utils/files.h"
#include "../../proto/raft.grpc.pb.h"

#include <mutex>
#include <string>
#include <vector>

std::string get_data_path(const std::string &dir);
std::string get_meta_path(const std::string &dir);

class LogVec;
class LogVecIterator {
public:
  using value_type = raft::Entry;
  using difference_type = std::ptrdiff_t;
  using pointer = const raft::Entry *;
  using reference = const raft::Entry &;
  using iterator_category = std::bidirectional_iterator_tag; // 支持双向

  LogVecIterator(LogVec *logvec, size_t idx, bool reverse = false);

  reference operator*() const;
  pointer operator->() const;

  LogVecIterator &operator++();
  LogVecIterator operator++(int);

  LogVecIterator &operator--();   // 新增前置--
  LogVecIterator operator--(int); // 新增后置--

  bool operator==(const LogVecIterator &other) const;
  bool operator!=(const LogVecIterator &other) const;

private:
  LogVec *logvec_;
  int idx_;
  bool is_reverse_;
  mutable raft::Entry cache_;
  mutable bool cache_valid_;

  void fetch() const;
};

class LogVec {
public:
  LogVec(const std::string &dir);
  ~LogVec();

  // vector-like API
  void push_back(const raft::Entry &value);
  raft::Entry operator[](size_t idx);
  size_t size() const;

  // 强制同步持久化
  void sync();

  LogVecIterator begin();
  LogVecIterator end();

  LogVecIterator rbegin();
  LogVecIterator rend();

private:
  // 编码/解码
  static std::vector<uint8_t> encode_entry(const raft::Entry &entry);
  static raft::Entry decode_entry(const std::vector<uint8_t> &buf);

  // 获取第idx个entry的offset
  uint64_t get_offset(size_t idx) const;

private:
  std::string dir_;
  std::string data_path_;
  std::string meta_path_;
  std::unique_ptr<tiny_lsm::FileObj> data_file_;
  std::unique_ptr<tiny_lsm::FileObj> meta_file_;
  size_t entry_count_;
  mutable std::mutex mtx_;
};