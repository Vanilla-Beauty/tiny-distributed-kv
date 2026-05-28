-- 1. 项目定义
set_project("distributed_db")
set_version("0.1.0")
set_languages("c++20")

add_rules("mode.debug", "mode.release")

-- 2. 添加依赖
add_requires("abseil", "protobuf-cpp", "grpc", "gtest", "spdlog")

-- ========== 静态库 ==========

-- gRPC 生成代码（包含 witness.proto）
target("grpc_gen")
    set_kind("static")
    add_files("src/grpc/*.cpp")
    add_files("proto/*.pb.cc")
    add_files("proto/*.grpc.pb.cc")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc")
    add_syslinks("pthread")

target("d_utils")
    set_kind("static")
    add_files("src/utils/*.cpp")
    add_includedirs("include")

target("d_storage")
    set_kind("static")
    add_files("src/storage/*.cpp")
    add_deps("grpc_gen", "d_utils")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc", "spdlog")

target("d_raft")
    set_kind("static")
    add_files("src/raft/*.cpp")
    add_deps("d_utils", "d_storage", "grpc_gen")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc", "spdlog")

-- CURP 组件
target("d_curp")
    set_kind("static")
    add_files("src/curp/*.cpp")
    add_deps("d_utils", "grpc_gen")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc", "spdlog")

-- ========== 单元测试 ==========

target("dtest_witness")
    set_kind("binary")
    add_deps("d_curp", "grpc_gen")
    add_files("test/dtest_witness.cpp")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest", "spdlog")
    add_syslinks("pthread")

target("dtest_grpc")
    set_kind("binary")
    add_deps("grpc_gen")
    add_files("test/dtest_grpc.cpp")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")
    add_syslinks("pthread")

target("dtest_storage")
    set_kind("binary")
    add_deps("d_storage")
    add_files("test/dtest_storage.cpp")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")
    add_syslinks("pthread")

target("dtest_utils")
    set_kind("binary")
    add_deps("d_utils")
    add_files("test/dtest_utils.cpp")
    add_includedirs("include")
    add_packages("gtest")

target("dtest_raft")
    set_kind("binary")
    add_deps("d_raft")
    add_files("test/dtest_raft.cpp")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")
    add_syslinks("pthread")