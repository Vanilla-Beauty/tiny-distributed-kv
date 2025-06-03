#include "../../include/utils/timer.h"

Timer::Timer() : stopped(false), notified(false) {}

Timer::~Timer() { stop(); }

void Timer::reset(std::chrono::milliseconds delay) {
  std::lock_guard<std::mutex> lock(mtx);
  deadline = std::chrono::steady_clock::now() + delay;
  notified = false;
  cv.notify_all();
}

bool Timer::wait() {
  std::unique_lock<std::mutex> lock(mtx);
  while (!stopped) {
    if (cv.wait_until(lock, deadline,
                      [this]() { return notified || stopped; })) {
      return false; // 被提前唤醒或停止
    }
    return true; // 到期
  }
  return false; // 已经停止
}

void Timer::stop() {
  stopped = true;
  cv.notify_all();
}
