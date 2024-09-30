#include "event_loop.h"

#include <atomic>
#include <utility>

namespace flutter {

EventLoop::EventLoop(const std::thread::id main_thread_id,
                     TaskExpiredCallback on_task_expired)
    : main_thread_id_(main_thread_id),
      on_task_expired_(std::move(on_task_expired)) {}

EventLoop::~EventLoop() = default;

bool EventLoop::RunsTasksOnCurrentThread() const {
  return std::this_thread::get_id() == main_thread_id_;
}

void EventLoop::WaitForEvents(std::chrono::nanoseconds max_wait) {
  const auto now = TaskTimePoint::clock::now();
  std::vector<FlutterTask> expired_tasks;

  {
    std::lock_guard lock(task_queue_mutex_);
    while (!task_queue_.empty()) {
      const auto& top = task_queue_.top();
      if (top.fire_time > now) {
        break;
      }
      expired_tasks.push_back(task_queue_.top().task);
      task_queue_.pop();
    }
  }

  {
    for (const auto& task : expired_tasks)
      on_task_expired_(&task);
  }

  {
    TaskTimePoint next_wake;
    {
      std::lock_guard lock(task_queue_mutex_);
      TaskTimePoint max_wake_timepoint =
          max_wait == std::chrono::nanoseconds::max() ? TaskTimePoint::max()
                                                      : now + max_wait;
      TaskTimePoint next_event_timepoint = task_queue_.empty()
                                               ? TaskTimePoint::max()
                                               : task_queue_.top().fire_time;
      next_wake = std::min(max_wake_timepoint, next_event_timepoint);
    }
    WaitUntil(next_wake);
  }
}

EventLoop::TaskTimePoint EventLoop::TimePointFromFlutterTime(
    uint64_t flutter_target_time_nanos) {
  const auto now = TaskTimePoint::clock::now();
  const auto flutter_duration =
      flutter_target_time_nanos - FlutterEngineGetCurrentTime();
  return now + std::chrono::nanoseconds(flutter_duration);
}

void EventLoop::PostTask(FlutterTask flutter_task,
                         uint64_t flutter_target_time_nanos) {
  static std::atomic_uint64_t sGlobalTaskOrder(0);

  Task task;
  task.order = ++sGlobalTaskOrder;
  task.fire_time = TimePointFromFlutterTime(flutter_target_time_nanos);
  task.task = flutter_task;

  {
    std::lock_guard lock(task_queue_mutex_);
    task_queue_.push(task);
  }
  Wake();
}
}  // namespace flutter
