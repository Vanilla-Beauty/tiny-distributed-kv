#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>

class Timer {
public:
    Timer();
    ~Timer();

    // 设置一个定时器，delay 后 wait() 才会返回 true
    void reset(std::chrono::milliseconds delay);

    // 阻塞直到到期或 reset() 被再次调用，返回是否真正到期
    bool wait();

    // 停止定时器，wait() 返回 false
    void stop();

private:
    std::mutex mtx;
    std::condition_variable cv;
    std::chrono::steady_clock::time_point deadline;
    std::atomic<bool> stopped;
    std::atomic<bool> notified;
};
