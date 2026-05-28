# CURP 实现计划

> 基于 tiny-distributed-kv 现有 Raft 实现扩展为 CURP

---

## 1. 现有代码分析

### 1.1 已实现组件

| 组件 | 文件 | 状态 | 说明 |
|------|------|------|------|
| RaftNode | `src/raft/raft.cpp` | ✓ 完成 | Raft 核心状态机 |
| RaftServiceImpl | `src/raft/raft_rpc.cpp` | ✓ 完成 | Raft RPC 服务 |
| LogVec | `src/storage/log_vec.cpp` | ✓ 完成 | 持久化日志 |
| Timer | `src/utils/timer.cpp` | ✓ 完成 | 定时器 |
| gRPC Server | `src/grpc/grpc_server.cpp` | ✓ 完成 | RPC 服务框架 |

### 1.2 Raft 实现特点

```cpp
// 现有 Raft 状态机
class RaftNode {
    RaftState role;           // LEADER/FOLLOWER/CANDIDATE
    int currentTerm;
    int votedFor;
    LogVec log;               // 持久化日志
    vector<uint64_t> nextIndex;
    vector<uint64_t> matchIndex;
    uint64_t commitIndex;
    
    // 关键流程
    void Elect();             // 选举
    void SendHeartBeats();    // 心跳/日志复制
    void handleAppendEntries();// 处理日志复制响应
};
```

### 1.3 已有 CURP 框架（空壳）

```cpp
// include/curp/curp.h - 需要重新设计
class Witness {
    tiny_lsm::LSM store_engine;  // 需要改为内存结构
};

class CurpNode {
    std::mutex mtx;
    uint64_t cur_node_id;
    Witness witness;  // 需要重新设计
};
```

---

## 2. Raft vs CURP 对比

### 2.1 架构差异

```
Raft 架构:
┌────────┐     ┌────────┐
│ Client │────→│ Leader │────→ Majority
└────────┘     └────────┘     (同步复制)
                   ↓
               2 RTT

CURP 架构:
┌────────┐     ┌────────┐
│ Client │────→│ Master │────→ Backup (异步)
└────────┘     └────────┘
     │              ↑
     └─────→ Witness × f
              (并行)
                   
               1 RTT
```

### 2.2 代码层面的关键差异

| 方面 | Raft | CURP |
|------|------|------|
| **角色** | Leader/Follower/Candidate | Master/Backup/Witness |
| **选举** | 复杂的多轮选举 | 简化的 leader 选举（可复用 Raft） |
| **写路径** | 同步复制到 majority | 异步复制 + Witness 记录 |
| **可交换性检查** | 无 | 核心机制 |
| **推测执行** | 无 | 执行后立即响应 |
| **恢复** | 从最新日志恢复 | Backup + Witness 重放 |

---

## 3. 实现计划

### Phase 1: Witness 实现（优先级最高）

Witness 是 CURP 的核心新组件。

#### 3.1.1 Witness 数据结构

```cpp
// include/curp/witness.h
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "../../proto/raft.pb.h"

// Witness 存储的请求记录
struct WitnessRecord {
    uint64_t rpc_id;        // RIFL 唯一标识
    std::string key;        // 操作的 key（用于可交换性检查）
    std::vector<uint8_t> request_data;  // 序列化的请求
    uint64_t timestamp;     // 用于 GC
};

class Witness {
public:
    Witness();
    
    // RPC 接口
    enum class RecordResult {
        ACCEPTED,
        REJECTED_NOT_COMMUTATIVE,
        REJECTED_NO_SPACE
    };
    
    RecordResult record(const std::string& key, uint64_t rpc_id, 
                        const std::vector<uint8_t>& request_data);
    
    void garbage_collect(const std::vector<std::pair<std::string, uint64_t>>& to_drop);
    
    std::vector<WitnessRecord> get_recovery_data();
    
    void stop_accepting();  // 进入恢复模式
    void reset();           // 重置为新 Master
    
private:
    std::mutex mtx_;
    bool accepting_ = true;
    
    // 按 key hash 分组的 slots
    static constexpr int kNumBuckets = 1024;
    std::unordered_map<std::string, WitnessRecord> records_;
    
    // 可交换性检查
    bool isCommutative(const std::string& key) const;
};
```

#### 3.1.2 Witness RPC 服务

```protobuf
// proto/witness.proto (新增)
syntax = "proto3";

package witness;

service WitnessService {
    rpc Record(RecordRequest) returns (RecordReply);
    rpc GarbageCollect(GCRequest) returns (GCReply);
    rpc GetRecoveryData(Empty) returns (RecoveryData);
    rpc Stop(Empty) returns (StopReply);
    rpc Reset(Empty) returns (ResetReply);
}

message RecordRequest {
    string master_id = 1;
    string key = 2;
    uint64 rpc_id = 3;
    bytes request_data = 4;
}

message RecordReply {
    bool accepted = 1;
}
```

#### 3.1.3 可交换性检查实现

```cpp
// src/curp/witness.cpp
Witness::RecordResult Witness::record(const std::string& key, uint64_t rpc_id,
                                       const std::vector<uint8_t>& request_data) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (!accepting_) {
        return RecordResult::REJECTED_NOT_COMMUTATIVE;
    }
    
    // 检查是否与已存储的请求冲突
    if (records_.find(key) != records_.end()) {
        return RecordResult::REJECTED_NOT_COMMUTATIVE;
    }
    
    // 检查空间
    if (records_.size() >= kMaxRecords) {
        return RecordResult::REJECTED_NO_SPACE;
    }
    
    // 存储记录
    records_[key] = WitnessRecord{
        .rpc_id = rpc_id,
        .key = key,
        .request_data = request_data,
        .timestamp = getCurrentTime()
    };
    
    return RecordResult::ACCEPTED;
}
```

---

### Phase 2: CurpMaster 实现

#### 3.2.1 CurpMaster 数据结构

```cpp
// include/curp/curp_master.h
#pragma once

#include "../raft/raft.h"
#include "witness.h"
#include <unordered_set>
#include <atomic>

enum class CurpState {
    NORMAL,
    RECOVERING
};

class CurpMaster : public RaftNode {
    friend class CurpMasterServiceImpl;
    
public:
    static std::shared_ptr<CurpMaster> Create(
        std::vector<NodeConfig> cluster_configs,
        std::vector<NodeConfig> witness_configs,
        std::string log_dir,
        uint64_t cur_node_id);
    
    // Curp 特有的接口
    bool proposeUpdate(const std::string& key, const std::vector<uint8_t>& value);
    std::string read(const std::string& key);
    
private:
    CurpMaster(std::vector<NodeConfig> cluster_configs,
               std::vector<NodeConfig> witness_configs,
               std::string log_dir,
               uint64_t cur_node_id);
    
    // Witness 配置
    std::vector<NodeConfig> witness_configs_;
    std::vector<std::shared_ptr<Witness>> witnesses_;
    
    // 未同步的 keys（用于可交换性检查）
    std::unordered_set<std::string> unsynced_keys_;
    std::mutex unsynced_mtx_;
    
    // 状态
    std::atomic<CurpState> state_{CurpState::NORMAL};
    uint64_t last_sync_index_{0};
    
    // 核心方法
    bool canSpeculativeExecute(const std::string& key);
    void markUnsynced(const std::string& key);
    void asyncSyncToBackup();
    void garbageCollectWitnesses();
    
    // 恢复
    void recover();
    void replayFromWitness(std::shared_ptr<Witness> witness);
};
```

#### 3.2.2 写操作流程

```cpp
// src/curp/curp_master.cpp
bool CurpMaster::proposeUpdate(const std::string& key, 
                                 const std::vector<uint8_t>& value) {
    // 1. 检查是否可以推测执行
    if (!canSpeculativeExecute(key)) {
        // 慢路径：先 sync，再执行
        syncToBackup();
        executeAndRespond(key, value);
        return true;
    }
    
    // 2. 推测执行（快速路径）
    markUnsynced(key);
    speculativeExecute(key, value);
    
    // 3. 客户端负责记录到 Witness（并行）
    // 这里只是准备响应，不等待 Witness
    
    return true;
}

bool CurpMaster::canSpeculativeExecute(const std::string& key) {
    std::lock_guard<std::mutex> lock(unsynced_mtx_);
    // 如果 key 在未同步集合中，不能推测执行
    return unsynced_keys_.find(key) == unsynced_keys_.end();
}

void CurpMaster::markUnsynced(const std::string& key) {
    std::lock_guard<std::mutex> lock(unsynced_mtx_);
    unsynced_keys_.insert(key);
}

void CurpMaster::asyncSyncToBackup() {
    // 后台线程定期批量同步
    std::thread([this]() {
        while (!killed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            syncBatch();
        }
    }).detach();
}
```

---

### Phase 3: CurpClient 实现

#### 3.3.1 Client 接口

```cpp
// include/curp/curp_client.h
#pragma once

#include <string>
#include <vector>
#include <memory>

struct WitnessConfig {
    std::string addr;
};

struct CurpConfig {
    std::string master_addr;
    std::vector<WitnessConfig> witness_configs;
};

class CurpClient {
public:
    CurpClient(CurpConfig config);
    
    // 1 RTT 写操作
    enum class WriteResult {
        SUCCESS,        // 1 RTT 完成
        SLOW_SUCCESS,   // 2 RTT 完成（需要 sync）
        FAILED
    };
    
    WriteResult write(const std::string& key, const std::vector<uint8_t>& value);
    std::vector<uint8_t> read(const std::string& key);
    
private:
    CurpConfig config_;
    uint64_t witness_list_version_{0};
    uint64_t next_rpc_id_{1};
    
    // 并行发送到 Master 和 Witness
    WriteResult parallelWrite(const std::string& key, 
                               const std::vector<uint8_t>& value);
    bool recordToWitnesses(const std::string& key, uint64_t rpc_id,
                           const std::vector<uint8_t>& request_data);
};
```

#### 3.3.2 并行写流程

```cpp
// src/curp/curp_client.cpp
CurpClient::WriteResult CurpClient::write(const std::string& key,
                                          const std::vector<uint8_t>& value) {
    uint64_t rpc_id = next_rpc_id_++;
    
    // 并行发送
    auto master_future = std::async([&]() {
        return sendToMaster(key, value, rpc_id);
    });
    
    auto witness_future = std::async([&]() {
        return recordToWitnesses(key, rpc_id, serializeRequest(key, value));
    });
    
    // 等待 Master 响应
    auto master_reply = master_future.get();
    
    // 等待 Witness 记录
    bool all_witness_accepted = witness_future.get();
    
    if (master_reply.success) {
        if (all_witness_accepted) {
            return WriteResult::SUCCESS;  // 1 RTT
        } else if (master_reply.synced) {
            return WriteResult::SLOW_SUCCESS;  // 2 RTT（Master 已 sync）
        } else {
            // 需要 sync
            sendSyncToMaster(rpc_id);
            return WriteResult::SLOW_SUCCESS;
        }
    }
    
    return WriteResult::FAILED;
}
```

---

### Phase 4: 恢复机制

#### 3.4.1 恢复流程

```cpp
// src/curp/curp_recovery.cpp
void CurpMaster::recover() {
    state_ = CurpState::RECOVERING;
    
    // Phase 1: 从 Backup 恢复有序数据（复用 Raft 恢复）
    spdlog::info("CurpMaster: 从 Backup 恢复数据");
    recoverFromBackup();
    
    // Phase 2: 从 Witness 重放无序请求
    spdlog::info("CurpMaster: 从 Witness 重放请求");
    
    // 选择一个可用的 Witness
    for (auto& witness : witnesses_) {
        if (witness->isAlive()) {
            replayFromWitness(witness);
            break;
        }
    }
    
    // Phase 3: 同步到 Backup，完成恢复
    syncToBackup();
    
    // Phase 4: 重置 Witness
    for (auto& witness : witnesses_) {
        witness->reset();
    }
    
    state_ = CurpState::NORMAL;
    spdlog::info("CurpMaster: 恢复完成");
}

void CurpMaster::replayFromWitness(std::shared_ptr<Witness> witness) {
    // 停止接受新请求
    witness->stop_accepting();
    
    // 获取所有记录的请求
    auto records = witness->get_recovery_data();
    
    // 重放（顺序不重要，因为可交换）
    for (const auto& record : records) {
        if (!alreadyExecuted(record.rpc_id)) {  // RIFL 去重
            replayRequest(record);
        }
    }
}
```

---

## 4. 文件结构规划

```
tiny-distributed-kv/
├── proto/
│   ├── raft.proto          # 已有
│   ├── node.proto          # 已有
│   └── witness.proto       # 新增：Witness RPC
│
├── include/
│   ├── raft/               # 已有，保持不变
│   ├── curp/
│   │   ├── curp.h          # 重写
│   │   ├── witness.h       # 新增
│   │   ├── curp_master.h   # 新增
│   │   └── curp_client.h   # 新增
│   └── grpc/
│       └── witness_service_impl.h  # 新增
│
├── src/
│   ├── raft/               # 已有
│   ├── curp/
│   │   ├── witness.cpp     # 新增
│   │   ├── curp_master.cpp # 新增
│   │   └── curp_client.cpp # 新增
│   └── grpc/
│       └── witness_service_impl.cpp  # 新增
│
└── test/
    ├── dtest_curp.cpp      # 新增
    └── dtest_witness.cpp   # 新增
```

---

## 5. 实现顺序

```
Phase 1: Witness（基础）
├── [1] proto/witness.proto          # Witness RPC 定义
├── [2] include/curp/witness.h       # Witness 数据结构
├── [3] src/curp/witness.cpp         # Witness 实现
├── [4] grpc/witness_service_impl    # Witness RPC 服务
└── [5] test/dtest_witness.cpp       # Witness 单元测试

Phase 2: CurpMaster（核心）
├── [6] include/curp/curp_master.h   # CurpMaster 定义
├── [7] src/curp/curp_master.cpp     # CurpMaster 实现
│   ├── 可交换性检查
│   ├── 推测执行
│   └── 异步同步
└── [8] 扩展 Raft -> CurpMaster 继承

Phase 3: CurpClient（接口）
├── [9] include/curp/curp_client.h   # Client 定义
└── [10] src/curp/curp_client.cpp    # 并行写实现

Phase 4: 恢复与完善
├── [11] 恢复机制
├── [12] GC 机制
├── [13] 集成测试
└── [14] 性能测试
```

---

## 6. 教学版简化策略

作为教学版实现，可以做以下简化：

| 方面 | 完整实现 | 教学简化 |
|------|----------|----------|
| **RIFL** | 完整的去重机制 | 简化的 RPC ID 检查 |
| **Witness 数量** | f 个独立 | 单个 Witness 即可演示 |
| **批量同步** | 自适应批大小 | 固定周期同步 |
| **GC** | 复杂的 GC 策略 | 简单的超时清理 |
| **配置管理** | 动态配置变更 | 静态配置 |

---

## 7. 验证计划

### 7.1 单元测试

```cpp
// test/dtest_witness.cpp
TEST(WitnessTest, RecordCommutative) {
    Witness w;
    auto r1 = w.record("key1", 1, {});
    EXPECT_EQ(r1, Witness::RecordResult::ACCEPTED);
    
    auto r2 = w.record("key2", 2, {});
    EXPECT_EQ(r2, Witness::RecordResult::ACCEPTED);
}

TEST(WitnessTest, RejectNonCommutative) {
    Witness w;
    w.record("key1", 1, {});
    auto r = w.record("key1", 2, {});
    EXPECT_EQ(r, Witness::RecordResult::REJECTED_NOT_COMMUTATIVE);
}
```

### 7.2 集成测试

```cpp
// test/dtest_curp.cpp
TEST(CurpTest, OneRoundTripWrite) {
    CurpClient client(config);
    auto result = client.write("key", value);
    EXPECT_EQ(result, CurpClient::WriteResult::SUCCESS);
}

TEST(CurpTest, RecoveryAfterCrash) {
    // 1. 启动 Master + Witness
    // 2. 写入数据（快速路径）
    // 3. 杀死 Master
    // 4. 重启，验证数据恢复
}
```

---

## 8. 下一步行动

建议从 **Phase 1: Witness** 开始：

1. 创建 `proto/witness.proto`
2. 实现 `Witness` 类和可交换性检查
3. 实现 `WitnessServiceImpl`
4. 编写单元测试验证

需要我开始实现吗？