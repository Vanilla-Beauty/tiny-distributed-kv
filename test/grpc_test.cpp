#include "../include/grpc/grpc_server.h"
#include <gtest/gtest.h>
#include <thread>

void RunTestServer() { RunServer("0.0.0.0:50051"); }

TEST(GrpcTest, Ping) {
  std::thread server_thread(RunTestServer);
  std::this_thread::sleep_for(std::chrono::seconds(1)); // wait for server

  std::string response = PingClient("localhost:50051");
  EXPECT_EQ(response, "pong");

  // 若需要更精细的测试，可以加入 SendMessage 测试
  server_thread.detach(); // 为简化，未做 server shutdown
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}