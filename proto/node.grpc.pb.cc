// Generated by the gRPC C++ plugin.
// If you make any local change, they will be lost.
// source: node.proto

#include "node.pb.h"
#include "node.grpc.pb.h"

#include <functional>
#include <grpcpp/support/async_stream.h>
#include <grpcpp/support/async_unary_call.h>
#include <grpcpp/impl/channel_interface.h>
#include <grpcpp/impl/client_unary_call.h>
#include <grpcpp/support/client_callback.h>
#include <grpcpp/support/message_allocator.h>
#include <grpcpp/support/method_handler.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/support/server_callback.h>
#include <grpcpp/impl/server_callback_handlers.h>
#include <grpcpp/server_context.h>
#include <grpcpp/impl/service_type.h>
#include <grpcpp/support/sync_stream.h>
namespace node {

static const char* NodeService_method_names[] = {
  "/node.NodeService/Ping",
  "/node.NodeService/SendMessage",
};

std::unique_ptr< NodeService::Stub> NodeService::NewStub(const std::shared_ptr< ::grpc::ChannelInterface>& channel, const ::grpc::StubOptions& options) {
  (void)options;
  std::unique_ptr< NodeService::Stub> stub(new NodeService::Stub(channel, options));
  return stub;
}

NodeService::Stub::Stub(const std::shared_ptr< ::grpc::ChannelInterface>& channel, const ::grpc::StubOptions& options)
  : channel_(channel), rpcmethod_Ping_(NodeService_method_names[0], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_SendMessage_(NodeService_method_names[1], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  {}

::grpc::Status NodeService::Stub::Ping(::grpc::ClientContext* context, const ::node::Empty& request, ::node::Pong* response) {
  return ::grpc::internal::BlockingUnaryCall< ::node::Empty, ::node::Pong, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_Ping_, context, request, response);
}

void NodeService::Stub::async::Ping(::grpc::ClientContext* context, const ::node::Empty* request, ::node::Pong* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::node::Empty, ::node::Pong, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_Ping_, context, request, response, std::move(f));
}

void NodeService::Stub::async::Ping(::grpc::ClientContext* context, const ::node::Empty* request, ::node::Pong* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_Ping_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::node::Pong>* NodeService::Stub::PrepareAsyncPingRaw(::grpc::ClientContext* context, const ::node::Empty& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::node::Pong, ::node::Empty, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_Ping_, context, request);
}

::grpc::ClientAsyncResponseReader< ::node::Pong>* NodeService::Stub::AsyncPingRaw(::grpc::ClientContext* context, const ::node::Empty& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncPingRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::Status NodeService::Stub::SendMessage(::grpc::ClientContext* context, const ::node::Message& request, ::node::Ack* response) {
  return ::grpc::internal::BlockingUnaryCall< ::node::Message, ::node::Ack, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_SendMessage_, context, request, response);
}

void NodeService::Stub::async::SendMessage(::grpc::ClientContext* context, const ::node::Message* request, ::node::Ack* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::node::Message, ::node::Ack, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_SendMessage_, context, request, response, std::move(f));
}

void NodeService::Stub::async::SendMessage(::grpc::ClientContext* context, const ::node::Message* request, ::node::Ack* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_SendMessage_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::node::Ack>* NodeService::Stub::PrepareAsyncSendMessageRaw(::grpc::ClientContext* context, const ::node::Message& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::node::Ack, ::node::Message, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_SendMessage_, context, request);
}

::grpc::ClientAsyncResponseReader< ::node::Ack>* NodeService::Stub::AsyncSendMessageRaw(::grpc::ClientContext* context, const ::node::Message& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncSendMessageRaw(context, request, cq);
  result->StartCall();
  return result;
}

NodeService::Service::Service() {
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      NodeService_method_names[0],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< NodeService::Service, ::node::Empty, ::node::Pong, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](NodeService::Service* service,
             ::grpc::ServerContext* ctx,
             const ::node::Empty* req,
             ::node::Pong* resp) {
               return service->Ping(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      NodeService_method_names[1],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< NodeService::Service, ::node::Message, ::node::Ack, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](NodeService::Service* service,
             ::grpc::ServerContext* ctx,
             const ::node::Message* req,
             ::node::Ack* resp) {
               return service->SendMessage(ctx, req, resp);
             }, this)));
}

NodeService::Service::~Service() {
}

::grpc::Status NodeService::Service::Ping(::grpc::ServerContext* context, const ::node::Empty* request, ::node::Pong* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status NodeService::Service::SendMessage(::grpc::ServerContext* context, const ::node::Message* request, ::node::Ack* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}


}  // namespace node

