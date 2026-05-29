# CURP Distributed KV

教学版 CURP（Consistent Unordered Replication Protocol）分布式键值数据库实现。

## 项目结构

```
├── include/
│   ├── curp/          # CURP 核心组件
│   ├── raft/          # Raft 共识
│   ├── grpc/          # gRPC 服务
│   └── tracing/       # 追踪系统
├── src/               # 实现
├── proto/             # Protocol Buffers
├── test/              # 测试
└── doc/               # 文档
```

## 核心组件

| 组件 | 文件 | 说明 |
|------|------|------|
| Witness | `curp/witness.h` | 记录未同步请求 |
| CurpMaster | `curp/curp_master.h` | 推测执行 + 异步同步 |
| CurpClient | `curp/curp_client.h` | 1-RTT 快速写入 |

## 快速开始

### 使用 xmake 构建（推荐）

```bash
# 配置项目 (debug 模式)
xmake config -m debug

# 构建所有测试
xmake build all_tests

# 或单独构建某个测试
xmake build dtest_witness

# 运行测试
xmake run dtest_witness
```

### 手动编译 (旧方式)

```bash
# 编译
g++ -std=c++20 -I include -I proto -I 3rd_party/tiny-lsm/include \
  src/curp/*.cpp src/grpc/*.cpp src/tracing/*.cpp \
  proto/*.pb.cc proto/*.grpc.pb.cc \
  test/dtest_witness.cpp \
  -lgrpc++ -lgrpc -lprotobuf -lgtest -pthread -lfmt -lspdlog \
  -labsl_* -o test_witness

# 运行测试
./test_witness
```

## CURP 协议

```
┌─────────┐                ┌────────┐                ┌─────────┐
│ Client  │                │ Master │                │ Witness │
└────┬────┘                └───┬────┘                └────┬────┘
     │  ┌─────────────────────┴─────────────────────────┐
     │  │ 1. 并行发送: propose → Master, record → Witness│
     │  └───────────────────────────────────────────────┘
     ├──────────────────────►│                          │
     │     propose          │  推测执行                  │
     │                      ├─────────────────────────►│
     │                      │       record             │
     │◄─────────────────────┤                          │
     │   fast_path OK       │                          │
     │                      │←─────────────────────────┤
     │                      │       ACK                │
     │                      │                          │
     │                      │  后台异步同步到 Backup      │
```

**快速路径条件**：
1. Master 无 key 冲突
2. 至少 1 个 Witness 接受
3. 1-RTT 返回

## 测试

```bash
./dtest_witness     # Witness 测试 (14 cases)
./dtest_curp        # CURP 核心测试 (10 cases)
./dtest_recovery    # 恢复机制测试 (8 cases)
./dtest_framework   # 测试框架验证 (6 cases)
```

## 文档

- `doc/curp-algorithm.md` - 算法详解
- `doc/curp-implementation-plan.md` - 实现计划
- `doc/test-framework-design.md` - 测试框架设计

## 依赖

- C++20
- gRPC + Protobuf
 - GTest
- spdlog
- tiny-lsm (子模块)

## License

MIT
