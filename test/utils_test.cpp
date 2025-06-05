#include "../include/utils/timer.h"
#include <gtest/gtest.h>
TEST(DUtils_Test, TimerBasic) {
  Timer t;

  std::thread ticker([&]() {
    while (true) {
      bool expired = t.wait(); // 阻塞直到定时器超时或 reset/stop
      if (!expired)
        break;

      std::cout << "Timer expired, triggering election\n";
      t.reset(std::chrono::milliseconds(100 + rand() % 300));
    }
  });

  // 初始化一次定时器
  t.reset(std::chrono::milliseconds(200));

  std::this_thread::sleep_for(std::chrono::seconds(3));
  std::cout << "Stop timer\n";
  t.stop();
  ticker.join();
}

TEST(DUtils_Test, TimerInitWithZero) {
  Timer t;
  std::cout << "Reset timer with zero delay\n";
  t.wait(); // 等待直到定时器超时
  std::cout << "Timer expired immediately\n";

  t.reset(std::chrono::milliseconds(1000)); // 重置定时器为1秒
  t.wait();
  std::cout << "Timer expired after 1 second\n";
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}