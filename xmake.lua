-- 1. 项目定义
set_project("distributed_db")
set_version("0.1.0")
set_languages("c++20")

-- 2. 添加依赖
add_requires("abseil", "protobuf-cpp", "grpc", "gtest")

add_subdirs("3rd_party/tiny-lsm")

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

target("d_curp")
    set_kind("static")
    add_files("src/curp/*.cpp")
    add_deps("d_utils", "grpc_gen")
    add_includedirs("include")
    add_packages("abseil", "protobuf-cpp", "grpc")


-- 6. 单元测试
target("grpc_test")
    set_kind("binary")
    add_deps("grpc_gen")
    add_files("test/grpc_test.cpp")
    add_includedirs("include", "proto")
    add_packages("abseil", "protobuf-cpp", "grpc", "gtest")

target("lsm_test")
    set_kind("binary")
    add_deps("lsm")
    add_files("test/tiny_lsm_test.cpp")
    add_includedirs("include")
    add_packages("gtest")

target("utils_test")
    set_kind("binary")
    add_deps("d_utils")
    add_files("test/utils_test.cpp")
    add_includedirs("include")
    add_packages("gtest")
