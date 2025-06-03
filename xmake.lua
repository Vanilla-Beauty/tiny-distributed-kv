-- 1. 项目定义
set_project("distributed_db")
set_version("0.1.0")
set_languages("c++17")

-- 2. 添加依赖（grpc + protobuf + gtest）
add_requires("abseil", "protobuf-cpp", "grpc", "gtest")

-- 3. 编译 proto 文件
rule("proto")
    set_extensions(".proto")
    on_buildcmd_file(function (target, batchcmds, sourcefile_proto)
        import("core.project.depend")
        import("core.base.path")
        import("core.project.config")

        local dir = path.directory(sourcefile_proto)
        local name = path.basename(sourcefile_proto)
        local pbfile = path.join(dir, name .. ".pb.cc")
        local grpcfile = path.join(dir, name .. ".grpc.pb.cc")

        -- 获取 protoc 和 grpc 插件
        local protoc = path.join(os.programdir(), "xmake", "packages", "p", "protobuf-cpp", "*/bin", "protoc")
        local plugin = path.join(os.programdir(), "xmake", "packages", "g", "grpc", "*/bin", "grpc_cpp_plugin")

        protoc = assert(os.iorunv("xmake", {"show", "protobuf-cpp::protoc"})):trim()
        plugin = assert(os.iorunv("xmake", {"show", "grpc::grpc_cpp_plugin"})):trim()

        batchcmds:show_progress("generating.proto", sourcefile_proto)
        batchcmds:vrunv(protoc, {
            "--cpp_out=src",
            "--grpc_out=src",
            "-Iproto",
            "--plugin=protoc-gen-grpc=" .. plugin,
            sourcefile_proto
        })
    end)

-- 4. 编译 proto 文件作为伪 target
target("proto_gen")
    set_kind("phony")
    add_rules("proto")
    add_files("proto/node.proto")

-- 5. grpc_server
target("grpc_server")
    set_kind("static")
    add_deps("proto_gen")
    add_files("src/*.cpp")
    add_includedirs("src", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc")

-- 6. 单元测试
target("grpc_test")
    set_kind("binary")
    add_deps("proto_gen")
    add_files("test/grpc_test.cpp")
    add_files("src/grpc_server.cpp")
    add_files("src/node_service_impl.cpp")
    add_files("src/node.grpc.pb.cc") -- gRPC
    add_files("src/node.pb.cc")
    add_includedirs("src", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")
