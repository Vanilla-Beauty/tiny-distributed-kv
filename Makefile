PROTOC = protoc
GRPC_CPP_PLUGIN = $(shell which grpc_cpp_plugin)
PROTO_DIR = proto

PROTO_SRCS = $(PROTO_DIR)/node.proto $(PROTO_DIR)/raft.proto
CPP_OUTS = $(PROTO_DIR)/node.pb.cc $(PROTO_DIR)/raft.pb.cc
GRPC_OUTS = $(PROTO_DIR)/node.grpc.pb.cc $(PROTO_DIR)/raft.grpc.pb.cc

all: $(CPP_OUTS) $(GRPC_OUTS)

$(PROTO_DIR)/node.pb.cc $(PROTO_DIR)/node.pb.h $(PROTO_DIR)/node.grpc.pb.cc $(PROTO_DIR)/node.grpc.pb.h: $(PROTO_DIR)/node.proto
	$(PROTOC) -I $(PROTO_DIR) node.proto \
	--cpp_out=$(PROTO_DIR) \
	--grpc_out=$(PROTO_DIR) \
	--plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN)

$(PROTO_DIR)/raft.pb.cc $(PROTO_DIR)/raft.pb.h $(PROTO_DIR)/raft.grpc.pb.cc $(PROTO_DIR)/raft.grpc.pb.h: $(PROTO_DIR)/raft.proto
	$(PROTOC) -I $(PROTO_DIR) raft.proto \
	--cpp_out=$(PROTO_DIR) \
	--grpc_out=$(PROTO_DIR) \
	--plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN)

clean:
	rm -f $(PROTO_DIR)/*.pb.cc $(PROTO_DIR)/*.pb.h