#include "node_service_impl.h"
#include <grpcpp/grpcpp.h>
#include <iostream>

void RunServer(const std::string &address) {
  NodeServiceImpl service;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << address << std::endl;
  server->Wait();
}

std::string PingClient(const std::string &address) {
  auto stub = node::NodeService::NewStub(
      grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  node::Empty req;
  node::Pong resp;
  auto status = stub->Ping(&context, req, &resp);
  return status.ok() ? "pong" : "error";
}
