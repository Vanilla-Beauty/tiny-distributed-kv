#include "../../include/storage/log_vec.h"
#include <cassert>
#include <cstring>

std::string get_data_path(const std::string &dir) {
  std::filesystem::path path(dir);
  if (dir.empty()) {
    return "log.data";
  }
  return (path / "log.data").string();
}

std::string get_meta_path(const std::string &dir) {
  std::filesystem::path path(dir);
  if (dir.empty()) {
    return "meta.data";
  }
  return (path / "meta.data").string();
}

LogVec::LogVec(const std::string &dir) : dir_(dir) {
  // 如果文件夹不存在就创建
  if (!std::filesystem::exists(dir)) {
    std::filesystem::create_directories(dir);
  }

  data_path_ = get_data_path(dir);
  meta_path_ = get_meta_path(dir);

  bool need_create = (!std::filesystem::exists(data_path_) ||
                      !std::filesystem::exists(meta_path_));

  data_file_ = std::make_unique<tiny_lsm::FileObj>(
      tiny_lsm::FileObj::open(data_path_, need_create));
  meta_file_ = std::make_unique<tiny_lsm::FileObj>(
      tiny_lsm::FileObj::open(meta_path_, need_create));

  entry_count_ = meta_file_->size() / sizeof(uint64_t);
}

LogVec::~LogVec() { sync(); }

LogVecIterator::LogVecIterator(LogVec *logvec, size_t idx, bool reverse)
    : logvec_(logvec), idx_(idx), cache_valid_(false), is_reverse_(reverse) {}

void LogVecIterator::fetch() const {
  if (idx_ >= logvec_->size() || idx_ < 0) {
    throw std::out_of_range("LogVecIterator::fetch()");
  }
  if (!cache_valid_) {
    if (logvec_ && idx_ < logvec_->size()) {
      cache_ = (*logvec_)[idx_];
      cache_valid_ = true;
    } else {
      cache_valid_ = false;
    }
  }
}

LogVecIterator::reference LogVecIterator::operator*() const {
  fetch();
  return cache_;
}

LogVecIterator::pointer LogVecIterator::operator->() const {
  fetch();
  return &cache_;
}

LogVecIterator::difference_type
LogVecIterator::operator-(const LogVecIterator &other) const {
  return idx_ - other.idx_;
}

LogVecIterator &LogVecIterator::operator--() {
  if (is_reverse_) {
    ++idx_;
  } else {
    --idx_;
  }
  cache_valid_ = false;
  return *this;
}

LogVecIterator LogVecIterator::operator--(int) {
  LogVecIterator tmp = *this;
  if (is_reverse_) {
    ++(*this);
  } else {
    --(*this);
  }
  return tmp;
}

LogVecIterator &LogVecIterator::operator++() {
  if (is_reverse_) {
    --idx_;
  } else {
    ++idx_;
  }
  cache_valid_ = false;
  return *this;
}

LogVecIterator LogVecIterator::operator++(int) {
  LogVecIterator tmp = *this;
  if (is_reverse_) {
    --(*this);
  } else {
    ++(*this);
  }
  return tmp;
}

LogVecIterator LogVecIterator::operator+(int n) const {
  LogVecIterator tmp = *this;
  tmp.idx_ += n;
  tmp.cache_valid_ = false;
  return tmp;
}
LogVecIterator LogVecIterator::operator-(int n) const {
  LogVecIterator tmp = *this;
  tmp.idx_ -= n;
  tmp.cache_valid_ = false;
  return tmp;
}

LogVecIterator &LogVecIterator::operator+=(int n) {
  idx_ += n;
  cache_valid_ = false;
  return *this;
}
LogVecIterator &LogVecIterator::operator-=(int n) {
  idx_ -= n;
  cache_valid_ = false;
  return *this;
}

bool LogVecIterator::operator==(const LogVecIterator &other) const {
  return logvec_ == other.logvec_ && idx_ == other.idx_;
}

bool LogVecIterator::operator!=(const LogVecIterator &other) const {
  return !(*this == other);
}

void LogVec::push_back(const raft::Entry &value) {
  std::lock_guard<std::mutex> lock(mtx_);
  // 1. 编码 entry
  auto buf = encode_entry(value);
  // 2. 获取当前 data 文件大小作为 offset
  uint64_t offset = data_file_->size();
  // 3. 追加 entry 到 data 文件
  data_file_->append(buf);
  // 4. 追加 offset 到 meta 文件
  std::vector<uint8_t> offset_bytes(sizeof(uint64_t));
  std::memcpy(offset_bytes.data(), &offset, sizeof(uint64_t));
  meta_file_->append(offset_bytes);
  ++entry_count_;
}

raft::Entry LogVec::operator[](size_t idx) {
  std::lock_guard<std::mutex> lock(mtx_);
  assert(idx < entry_count_);
  uint64_t offset = get_offset(idx);
  uint64_t next_offset =
      (idx + 1 < entry_count_) ? get_offset(idx + 1) : data_file_->size();
  size_t len = next_offset - offset;
  auto buf = data_file_->read_to_slice(offset, len);
  return decode_entry(buf);
}

size_t LogVec::size() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return entry_count_;
}

void LogVec::sync() {
  std::lock_guard<std::mutex> lock(mtx_);
  data_file_->sync();
  meta_file_->sync();
}

uint64_t LogVec::get_offset(size_t idx) const {
  auto off = meta_file_->read_uint64(idx * sizeof(uint64_t));
  return off;
}

LogVecIterator LogVec::begin() { return LogVecIterator(this, 0); }

LogVecIterator LogVec::end() { return LogVecIterator(this, size()); }

// 新增反向迭代器接口
LogVecIterator LogVec::rbegin() {
  return LogVecIterator(this, size() - 1, true);
}
LogVecIterator LogVec::rend() { return LogVecIterator(this, -1, true); }

void LogVec::truncate_from(size_t idx) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (idx >= entry_count_)
    return;
  // 只保留 [0, idx)，截断 data 和 meta 文件
  uint64_t new_data_size = 0;
  if (idx > 0) {
    new_data_size = get_offset(idx);
  }
  data_file_->truncate(new_data_size);
  meta_file_->truncate(idx * sizeof(uint64_t));
  entry_count_ = idx;
}

// 编码/解码实现（简单示例，实际可用protobuf序列化或自定义二进制格式）
std::vector<uint8_t> LogVec::encode_entry(const raft::Entry &entry) {
  std::string s;
  entry.SerializeToString(&s);
  return std::vector<uint8_t>(s.begin(), s.end());
}

raft::Entry LogVec::decode_entry(const std::vector<uint8_t> &buf) {
  raft::Entry entry;
  entry.ParseFromArray(buf.data(), buf.size());
  return entry;
}