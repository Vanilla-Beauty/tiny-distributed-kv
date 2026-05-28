# CURP 共识算法详解

> **论文来源**: *Exploiting Commutativity For Practical Fast Replication*  
> **作者**: Seo Jin Park, John Ousterhout (Stanford University)  
> **会议**: NSDI '19  
> **论文路径**: `doc/curp-nsdi19.pdf`

---

## 1. 概述

CURP（Consistent Unordered Replication Protocol）是一种创新的复制协议，**将大多数写操作的延迟从 2 RTT 降低到 1 RTT**。

### 核心目标
- **线性一致性**：保证强一致性
- **低延迟**：正常情况下 1 RTT 完成
- **容错性**：支持 f 个节点故障
- **通用性**：可应用于 primary-backup 和 consensus 系统

### 性能提升（论文实测）
| 系统 | 无复制 | 传统复制 | CURP 复制 |
|------|--------|----------|-----------|
| RAMCloud 写延迟 | 6.1 µs | 14 µs | **7.1 µs** |
| Redis | 非持久化 | 高开销持久化 | **低开销持久化** |

---

## 2. 核心思想：分离持久性与排序

### 传统复制的问题

传统复制协议（Raft、Paxos、primary-backup）必须：
1. **先排序**：确定操作的执行顺序
2. **再持久化**：将有序操作复制到副本

这导致 **至少 2 RTT**：
```
Client → Master (1 RTT)
Master → Backup (1 RTT)
```

### CURP 的创新

CURP 发现：**如果操作是可交换的（commutative），它们的执行顺序就不重要！**

因此：
- **持久性可以独立完成**（无需等待排序）
- **排序可以延迟执行**（异步进行）

```
Client → Master + Witness (并行，1 RTT)
Master → Backup (异步，不影响客户端)
```

---

## 3. 架构组件

CURP 在传统 primary-backup 基础上引入 **Witness（见证者）**。

### 节点角色

| 角色 | 数量 | 功能 |
|------|------|------|
| **Master** | 1 | 接收请求、执行操作、协调复制 |
| **Backup** | f | 存储有序数据，用于恢复 |
| **Witness** | f | 临时存储未排序请求，保证持久性 |

### 容错能力
- **f + 1 replicas**（Master + f Backups）
- **f witnesses**
- 系统可容忍 **f 个任意节点故障**

```
            ┌─────────────┐
            │   Witness   │ × f  (无序持久化)
            └─────────────┘
                  ↓
┌─────────────────────────────────────┐
│              Client                 │
└─────────────────────────────────────┘
          ↓               ↓
    ┌──────────┐    ┌──────────┐
    │  Master  │ →  │  Backup  │ × f  (有序持久化)
    └──────────┘    └──────────┘
```

---

## 4. 正常操作流程

### 4.1 快速路径（Fast Path）- 1 RTT

**条件**：操作与所有未同步操作可交换

```
时间线:
─────────────────────────────────────────────────→

Client                    Master                   Witness
  │                         │                         │
  │──── update request ────→│                         │
  │                         │                         │
  │───── record request ───────────────────────────→│
  │                         │                         │
  │                         │── execute (speculative) │
  │                         │                         │
  │←───── response ─────────│                         │
  │                         │                         │
  │←─── ACCEPTED ───────────────────────────────────│
  │                         │                         │
  │  ✓ 操作完成             │                         │
  │                         │─── async sync ─────────→│ Backup
```

**步骤**：
1. Client **并行发送**请求到 Master 和所有 Witness
2. Witness 检查可交换性，接受则返回 ACCEPTED
3. Master **推测执行**（speculative execution），立即响应
4. Client 收到 Master 响应 + 所有 Witness ACCEPTED → **完成**
5. Master **异步同步**到 Backup

### 4.2 慢速路径（Slow Path）- 2 RTT

**触发条件**：
- Witness 拒绝（可交换性冲突或空间不足）
- Master 发现冲突（操作与未同步操作不可交换）

```
时间线:
─────────────────────────────────────────────────→

Client                    Master                   Backup
  │                         │                         │
  │──── update request ────→│                         │
  │                         │                         │
  │←───── CONFLICT ─────────│                         │
  │                         │                         │
  │────── sync request ────→│                         │
  │                         │──── sync ──────────────→│
  │                         │←─── ack ───────────────│
  │                         │                         │
  │←───── synced response ──│                         │
  │                         │                         │
  │  ✓ 操作完成             │                         │
```

---

## 5. Witness 详解

### 5.1 Witness 的职责

Witness 是 **轻量级的临时持久化存储**：

| 操作 | 说明 |
|------|------|
| **record** | 接收并存储客户端请求（检查可交换性） |
| **gc** | 清理已同步到 Backup 的请求 |
| **getRecoveryData** | 恢复时返回所有存储的请求 |

### 5.2 可交换性检查

**Witness 只接受与已存储操作可交换的新操作**。

对于 KV 存储：
- 操作不同 key → 可交换 ✓
- 操作相同 key → 不可交换 ✗

```cpp
// 示例：Witness 检查逻辑
bool Witness::canAccept(const Request& new_req) {
    for (const auto& saved : saved_requests) {
        if (!isCommutative(new_req, saved)) {
            return false;  // 拒绝
        }
    }
    return true;  // 接受
}

bool isCommutative(const Request& a, const Request& b) {
    // 不同 key 的写操作可交换
    return a.keys != b.keys;
}
```

### 5.3 数据结构

Witness 使用 **类似 Cache 的结构**：
- 用 key hash 定位 slot set
- 每个 set 有多个 slot
- 冲突检测：同 hash → 不同 slot，同 key → 拒绝

---

## 6. Master 详解

### 6.1 推测执行（Speculative Execution）

Master 可以 **在同步 Backup 之前响应客户端**。

**安全条件**：
- 该操作与所有未同步（unsynced）操作可交换
- 否则必须先 sync

```
未同步操作队列（unsynced）:
[op1, op2, op3, ..., op_n] ← 尾部是未同步的

新操作 op_new:
- 如果 op_new 与 [op1...op_n] 全部可交换 → 推测执行 ✓
- 否则 → 先 sync，再执行
```

### 6.2 异步同步（Async Sync）

Master **批量同步**到 Backup，提高吞吐量：
- 减少 RPC 数量
- 避免 CPU 空转等待

---

## 7. 恢复机制

### 7.1 恢复流程

Master 崩溃后，新 Master 恢复：

```
Phase 1: 从 Backup 恢复有序数据
───────────────────────────────────
New Master ← Backup (传统恢复机制)

Phase 2: 从 Witness 重放无序请求
───────────────────────────────────
1. 选择一个可用 Witness
2. 停止该 Witness 接收新请求
3. 获取所有存储的请求
4. 重放请求（任意顺序，因为可交换）
5. 同步到 Backup
6. 重置 Witness
```

### 7.2 防止重复执行

问题：部分请求可能已同步到 Backup，但也在 Witness 中。

解决方案：**RIFL（Exactly-Once 语义）**
- 每个请求有唯一 RPC ID
- 检测并过滤已执行的请求

---

## 8. 读操作

### 8.1 从 Master 读

- 检查读操作是否与未同步操作可交换
- 冲突 → 先 sync，再读
- 无冲突 → 直接读

### 8.2 从 Backup 读（Consistent Read）

允许从 Backup 读以降低延迟，但需检查 Witness：

```
Client:
1. 查询 Witness：读操作是否与存储请求可交换？
   
   可交换 → 从 Backup 读 ✓
   不可交换 → 从 Master 读
```

---

## 9. 与 Raft 的对比

| 特性 | Raft | CURP |
|------|------|------|
| **延迟** | 2 RTT | 1 RTT（快速路径） |
| **排序时机** | 先排序 | 排序延迟/异步 |
| **持久性** | 排序后持久化 | 排序前持久化 |
| **可交换性** | 不利用 | 核心依赖 |
| **额外组件** | 无 | Witness |
| **适用场景** | 通用 | 操作可判断可交换性 |

### 从 Raft 到 CURP 的演进思路

```
Raft:
Client → Leader → Log → Majority → Execute → Response

CURP:
Client → Master + Witness → Execute → Response
       ↓
       (异步) Master → Backup
```

---

## 10. 实现要点（教学版）

### 10.1 需要实现的组件

| 组件 | 功能 | 复杂度 |
|------|------|--------|
| **Witness** | 存储请求、检查可交换性 | 中等 |
| **CurpMaster** | 推测执行、异步 sync、可交换检查 | 较高 |
| **CurpClient** | 并行发送、处理响应 | 简单 |
| **Recovery** | Backup 恢复 + Witness 重放 | 中等 |

### 10.2 可交换性判断（KV 存储简化版）

```cpp
// 简化版：只看 key 是否相同
bool isCommutative(const Operation& a, const Operation& b) {
    // 不同 key → 可交换
    if (a.key != b.key) return true;
    
    // 相同 key：
    // - 都是读 → 可交换
    // - 有写 → 不可交换
    if (a.isRead && b.isRead) return true;
    return false;
}
```

### 10.3 Witness RPC 接口（简化）

```protobuf
service Witness {
    rpc Record(RecordRequest) returns (RecordReply);
    rpc GarbageCollect(GCRequest) returns (GCReply);
    rpc GetRecoveryData(Empty) returns (RecoveryData);
}

message RecordRequest {
    string master_id;
    string key;
    uint64 rpc_id;
    bytes request_data;
}

message RecordReply {
    bool accepted;
}
```

### 10.4 Master 状态管理

```cpp
class CurpMaster {
    std::set<Key> unsynced_keys;  // 未同步的 keys
    LogVec log;                   // 操作日志
    uint64_t last_sync_index;     // 最后同步位置
    
    bool canSpeculativeExecute(const Operation& op) {
        // 检查 op.key 是否在 unsynced_keys 中
        return unsynced_keys.find(op.key) == unsynced_keys.end();
    }
    
    void speculativeExecute(const Operation& op) {
        unsynced_keys.insert(op.key);
        execute(op);
        // 异步 sync...
    }
};
```

---

## 11. 总结

CURP 的关键洞察：

> **如果操作可交换，顺序不重要；如果顺序不重要，持久化可以提前完成。**

这打破了传统复制协议中 "排序 → 持久化" 的耦合，实现了 **1 RTT 的线性一致性写操作**。

对于 KV 存储这类天然支持可交换性检查的系统，CURP 是理想的复制协议选择。

---

## 参考资料

- 论文 PDF: `doc/curp-nsdi19.pdf`
- 论文文本: `doc/curp-nsdi19.txt`
- NSDI '19 视频: https://www.usenix.org/conference/nsdi19/presentation/park