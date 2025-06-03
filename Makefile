all: proto/node.proto
	protoc -I proto/ node.proto \
	--cpp_out=src/ \
	--grpc_out=src/ \
	--plugin=protoc-gen-grpc=`which grpc_cpp_plugin`
