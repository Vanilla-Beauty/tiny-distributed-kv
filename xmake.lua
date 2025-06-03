-- 1. 项目定义
set_project("distributed_db")
set_version("0.1.0")
set_languages("c++17")

-- 2. 添加依赖
add_requires("abseil", "protobuf-cpp", "grpc", "gtest")

-- 3. 编译 proto 文件
rule("proto")
    set_extensions(".proto")
    on_buildcmd_file(function (target, batchcmds, sourcefile_proto)
        import("core.project.depend")
        import("core.base.path")
        import("core.project.config")

        local dir = "proto"
        local name = path.basename(sourcefile_proto)
        local pbfile = path.join(dir, name .. ".pb.cc")
        local grpcfile = path.join(dir, name .. ".grpc.pb.cc")

        local protoc = assert(os.iorunv("xmake", {"show", "protobuf-cpp::protoc"})):trim()
        local plugin = assert(os.iorunv("xmake", {"show", "grpc::grpc_cpp_plugin"})):trim()

        batchcmds:show_progress("generating.proto", sourcefile_proto)
        batchcmds:vrunv(protoc, {
            "--cpp_out=proto",
            "--grpc_out=proto",
            "-Iproto",
            "--plugin=protoc-gen-grpc=" .. plugin,
            sourcefile_proto
        })
    end)

-- 4. proto 伪 target
target("proto_gen")
    set_kind("phony")
    add_rules("proto")
    add_files("proto/node.proto")

-- 5. grpc_server 静态库
target("grpc_server")
    set_kind("static")
    add_deps("proto_gen")
    add_files("src/grpc/*.cpp")
    add_files("proto/*.pb.cc")
    add_files("proto/*.grpc.pb.cc")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc")

-- 6. 单元测试
target("grpc_test")
    set_kind("binary")
    add_deps("grpc_server")
    add_files("test/grpc_test.cpp")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")