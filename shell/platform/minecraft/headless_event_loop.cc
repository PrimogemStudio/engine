#include "headless_event_loop.h"

namespace flutter {
HeadlessEventLoop::HeadlessEventLoop(const std::thread::id main_thread_id,
                                     TaskExpiredCallback on_task_expired)
    : EventLoop(main_thread_id, std::move(on_task_expired)) {}

HeadlessEventLoop::~HeadlessEventLoop() = default;

void HeadlessEventLoop::WaitUntil(const TaskTimePoint& time) {
  std::mutex& mutex = GetTaskQueueMutex();
  std::unique_lock lock(mutex);
  task_queue_condition_.wait_until(lock, time);
}

void HeadlessEventLoop::Wake() {
  task_queue_condition_.notify_one();
}
}  // namespace flutter
