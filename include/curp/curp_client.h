#pragma once

#include "curp_master.h"  // 包含 OpType 和 CurpOp 定义
#include "witness.h"
#include "../grpc/witness_service_impl.h"  // 包含 WitnessClient 定义
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <mutex>

namespace curp {

/**
 * @brief CURP Master 的 gRPC 客户端（简化版）
 */
class CurpMasterClient {
public:
    explicit CurpMasterClient(const std::string& target);
    ~CurpMasterClient() = default;
    
    struct ProposeResponse {
        bool success;
        bool fast_path;
        bool need_sync;
        std::vector<uint8_t> result;
        std::string error;
    };
    ProposeResponse propose(const CurpOp& op);
    
    struct ReadResponse {
        bool success;
        std::vector<uint8_t> value;
        std::string error;
    };
    ReadResponse read(const std::string& key);
    
    bool sync(uint64_t rpc_id);
    
private:
    std::string target_;
};

/**
 * @brief CURP 客户端配置
 */
struct CurpClientConfig {
    std::string master_addr;
    std::vector<std::string> witness_addrs;
    uint64_t client_id;
    uint64_t timeout_ms{5000};
};

/**
 * @brief CURP 客户端 - 实现 1 RTT 写操作
 */
class CurpClient {
public:
    explicit CurpClient(const CurpClientConfig& config);
    ~CurpClient();
    
    struct WriteResult {
        bool success;
        bool fast_path;
        int rtt_count;
        std::string error;
    };
    WriteResult write(const std::string& key, const std::vector<uint8_t>& value);
    
    struct ReadResult {
        bool success;
        std::vector<uint8_t> value;
        std::string error;
    };
    ReadResult read(const std::string& key);
    
    WriteResult deleteOp(const std::string& key);
    
    uint64_t next_rpc_id();
    void reset();
    
private:
    std::mutex mtx_;
    CurpClientConfig config_;
    uint64_t next_rpc_id_{1};
    
    std::unique_ptr<CurpMasterClient> master_client_;
    std::vector<std::shared_ptr<WitnessClient>> witness_clients_;
    
    WriteResult parallel_write(const CurpOp& op);
    int record_to_witnesses(uint64_t rpc_id, const std::string& key,
                            const std::vector<uint8_t>& request_data);
    bool send_sync_to_master(uint64_t rpc_id);
    std::vector<uint8_t> serialize_request(const CurpOp& op);
    
    // 统计
    uint64_t total_writes_{0};
    uint64_t total_reads_{0};
    uint64_t total_fast_path_{0};
    uint64_t total_slow_path_{0};
    uint64_t total_failed_{0};
};

} // namespace curp