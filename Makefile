all: proto/node.proto
	protoc -I proto/ raft.proto \
	--cpp_out=proto/ \
	--grpc_out=proto/ \
	--plugin=protoc-gen-grpc=`which grpc_cpp_plugin`
