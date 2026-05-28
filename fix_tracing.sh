#!/bin/bash
# 修复所有 uint64_t 类型歧义问题
sed -i 's/span\.set_attribute("rpc_id", rpc_id)/span.set_attribute("rpc_id", static_cast<int64_t>(rpc_id))/g' src/curp/witness.cpp
sed -i 's/span\.set_attribute("client_id", client_id)/span.set_attribute("client_id", static_cast<int64_t>(client_id))/g' src/curp/witness.cpp
sed -i 's/span\.set_attribute("conflict_rpc_id", existing\.rpc_id)/span.set_attribute("conflict_rpc_id", static_cast<int64_t>(existing.rpc_id))/g' src/curp/witness.cpp
sed -i 's/span\.set_attribute("client_id", config_\.client_id)/span.set_attribute("client_id", static_cast<int64_t>(config_.client_id))/g' src/curp/curp_client.cpp
sed -i 's/span\.set_attribute("client_id", op\.client_id)/span.set_attribute("client_id", static_cast<int64_t>(op.client_id))/g' src/curp/curp_client.cpp
sed -i 's/span\.set_attribute("master_id", master_id)/span.set_attribute("master_id", master_id)/g' src/curp/witness.cpp  # string OK
sed -i 's/master->log_dir/master->RaftNode::log_dir/g' src/curp/curp_master.cpp
sed -i 's/config_\.retry_count/0/g' src/curp/curp_client.cpp
