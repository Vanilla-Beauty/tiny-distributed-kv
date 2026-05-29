-- xmake.lua for tiny-distributed-kv
-- CURP 分布式键值数据库

set_project("tiny-distributed-kv")
set_version("1.0.0")
set_languages("c++20")

-- 添加编译模式
add_rules("mode.debug", "mode.release")

-- 添加包仓库和依赖
add_requires("toml11", "spdlog", "fmt", "gtest")

-- 全局系统库链接
add_syslinks("pthread", "grpc++", "grpc", "gpr", "protobuf", "absl_synchronization", "absl_strings", "absl_time", "absl_base", "absl_log_severity", "absl_raw_logging_internal", "absl_spinlock_wait", "absl_malloc_internal")

-- tiny-lsm 库 (第三方，需要 toml11)
target("tiny-lsm")
    set_kind("static")
    add_files("3rd_party/tiny-lsm/src/*/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("3rd_party/tiny-lsm/include", {public = true})

-- proto 库 (使用已生成的 protobuf 文件)
target("proto")
    set_kind("static")
    add_files("proto/*.pb.cc", "proto/*.grpc.pb.cc")
    add_includedirs("proto", {public = true})

-- 核心库：curp
target("curp")
    set_kind("static")
    add_files("src/curp/*.cpp")
    add_includedirs("include", {public = true})
    add_deps("proto", "tiny-lsm")
    add_packages("spdlog", "fmt")

-- 核心库：grpc 服务
target("grpc-service")
    set_kind("static")
    add_files("src/grpc/*.cpp")
    add_includedirs("include", {public = true})
    add_deps("proto")
    add_packages("spdlog", "fmt")

-- 核心库：raft
target("raft")
    set_kind("static")
    add_files("src/raft/*.cpp")
    add_includedirs("include", {public = true})
    add_deps("proto", "tiny-lsm")
    add_packages("spdlog", "fmt")

-- 核心库：tracing
target("tracing")
    set_kind("static")
    add_files("src/tracing/*.cpp")
    add_includedirs("include", {public = true})
    add_packages("spdlog", "fmt")

-- 核心库：storage
target("storage")
    set_kind("static")
    add_files("src/storage/*.cpp")
    add_includedirs("include", {public = true})
    add_deps("tiny-lsm")
    add_packages("spdlog", "fmt")

-- 核心库：utils
target("utils")
    set_kind("static")
    add_files("src/utils/*.cpp")
    add_includedirs("include", {public = true})
    add_packages("spdlog", "fmt")

-- 测试框架库
target("test-framework")
    set_kind("static")
    add_files("src/test/*.cpp")
    add_includedirs("include", {public = true})
    add_deps("proto", "tiny-lsm")
    add_packages("spdlog", "fmt", "gtest")

-- 测试目标：witness
target("dtest_witness")
    set_kind("binary")
    add_files("test/dtest_witness.cpp")
    add_deps("test-framework", "grpc-service", "curp", "raft", "tracing", "storage", "utils", "proto", "tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 测试目标：curp
target("dtest_curp")
    set_kind("binary")
    add_files("test/dtest_curp.cpp")
    add_deps("test-framework", "grpc-service", "curp", "raft", "tracing", "storage", "utils", "proto", "tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 测试目标：recovery
target("dtest_recovery")
    set_kind("binary")
    add_files("test/dtest_recovery.cpp")
    add_deps("test-framework", "grpc-service", "curp", "raft", "tracing", "storage", "utils", "proto", "tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 测试目标：framework
target("dtest_framework")
    set_kind("binary")
    add_files("test/dtest_framework.cpp")
    add_deps("test-framework", "grpc-service", "curp", "raft", "tracing", "storage", "utils", "proto", "tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 测试目标：storage
target("dtest_storage")
    set_kind("binary")
    add_files("test/dtest_storage.cpp")
    add_deps("storage", "tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 测试目标：utils
target("dtest_utils")
    set_kind("binary")
    add_files("test/dtest_utils.cpp")
    add_deps("utils", "tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 测试目标：tiny_lsm
target("dtest_tiny_lsm")
    set_kind("binary")
    add_files("test/dtest_tiny_lsm.cpp")
    add_deps("tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 测试目标：raft
target("dtest_raft")
    set_kind("binary")
    add_files("test/dtest_raft.cpp")
    add_deps("raft", "proto", "tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 测试目标：grpc
target("dtest_grpc")
    set_kind("binary")
    add_files("test/dtest_grpc.cpp")
    add_deps("grpc-service", "proto", "tiny-lsm")
    add_packages("gtest", "spdlog", "fmt")

-- 默认构建所有测试
target("all_tests")
    set_kind("phony")
    add_deps("dtest_witness", "dtest_curp", "dtest_recovery", "dtest_framework")