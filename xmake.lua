-- 1. 项目定义
set_project("distributed_db")
set_version("0.1.0")
set_languages("c++20")

add_rules("mode.debug", "mode.release")

add_repositories("local-repo build")

-- 2. 添加依赖
add_requires("abseil", "protobuf-cpp", "grpc", "gtest", "spdlog")

includes("3rd_party/tiny-lsm")

-- 5. grpc_gen 静态库
target("grpc_gen")
    set_kind("static")
    add_files("src/grpc/*.cpp")
    add_files("proto/*.pb.cc")
    add_files("proto/*.grpc.pb.cc")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc")

target("d_utils")
    set_kind("static")
    add_files("src/utils/*.cpp")
    add_includedirs("include")

target("d_storage")
    set_kind("static")
    add_files("src/storage/*.cpp")
    add_deps("grpc_gen", "utils")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc", "spdlog")

target("d_raft")
    set_kind("static")
    add_files("src/raft/*.cpp")
    add_deps("d_utils", "d_storage", "grpc_gen")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc", "spdlog")


-- 6. 单元测试
target("dtest_grpc")
    set_kind("binary")
    add_deps("grpc_gen")
    add_files("test/dtest_grpc.cpp")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")

-- target("dtest_lsm")
--     set_kind("binary")
--     add_deps("lsm")
--     add_files("test/tiny_lsm_test.cpp")
--     add_includedirs("include")
--     add_packages("gtest")

target("dtest_storage")
    set_kind("binary")
    add_deps("d_storage")
    add_files("test/dtest_storage.cpp")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")

target("dtest_utils")
    set_kind("binary")
    add_deps("d_utils")
    add_files("test/dtest_utils.cpp")
    add_includedirs("include")
    add_packages("gtest")

target("dtest_raft")
    set_kind("binary")
    add_deps("d_raft")
    -- add_deps("lsm")
    add_files("test/dtest_raft.cpp")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")
