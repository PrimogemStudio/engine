#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "embedder.h"

namespace flutter {
class EventLoop {
 public:
  using TaskExpiredCallback = std::function<void(const FlutterTask*)>;

  EventLoop(std::thread::id main_thread_id,
            TaskExpiredCallback on_task_expired);

  virtual ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  bool RunsTasksOnCurrentThread() const;

  void WaitForEvents(
      std::chrono::nanoseconds max_wait = std::chrono::nanoseconds::max());

  void PostTask(FlutterTask flutter_task, uint64_t flutter_target_time_nanos);

 protected:
  using TaskTimePoint = std::chrono::steady_clock::time_point;

  static TaskTimePoint TimePointFromFlutterTime(
      uint64_t flutter_target_time_nanos);

  std::mutex& GetTaskQueueMutex() { return task_queue_mutex_; }

  virtual void WaitUntil(const TaskTimePoint& time) = 0;

  virtual void Wake() = 0;

  struct Task {
    uint64_t order;
    TaskTimePoint fire_time;
    FlutterTask task;

    struct Comparer {
      bool operator()(const Task& a, const Task& b) {
        if (a.fire_time == b.fire_time) {
          return a.order > b.order;
        }
        return a.fire_time > b.fire_time;
      }
    };
  };
  std::thread::id main_thread_id_;
  TaskExpiredCallback on_task_expired_;
  std::mutex task_queue_mutex_;
  std::priority_queue<Task, std::deque<Task>, Task::Comparer> task_queue_;
};
}  // namespace flutter
