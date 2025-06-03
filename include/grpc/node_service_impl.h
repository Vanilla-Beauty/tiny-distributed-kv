#pragma once
#include "../../proto/node.grpc.pb.h"

class NodeServiceImpl final : public node::NodeService::Service {
public:
  grpc::Status Ping(grpc::ServerContext *context, const node::Empty *request,
                    node::Pong *reply) override;
  grpc::Status SendMessage(grpc::ServerContext *context,
                           const node::Message *request,
                           node::Ack *reply) override;
};
