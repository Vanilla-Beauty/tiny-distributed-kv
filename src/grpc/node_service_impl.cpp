#include "../../include/grpc/node_service_impl.h"
#include <iostream>

grpc::Status NodeServiceImpl::Ping(grpc::ServerContext *, const node::Empty *,
                                   node::Pong *reply) {
  reply->set_msg("pong");
  return grpc::Status::OK;
}

grpc::Status NodeServiceImpl::SendMessage(grpc::ServerContext *,
                                          const node::Message *request,
                                          node::Ack *reply) {
  std::cout << "Received message: " << request->content() << std::endl;
  reply->set_success(true);
  return grpc::Status::OK;
}
