#pragma once

#include <condition_variable>

#include "event_loop.h"

namespace flutter {
class HeadlessEventLoop : public EventLoop {
 public:
  using TaskExpiredCallback = std::function<void(const FlutterTask*)>;
  HeadlessEventLoop(std::thread::id main_thread_id,
                    TaskExpiredCallback on_task_expired);

  ~HeadlessEventLoop() override;

  HeadlessEventLoop(const HeadlessEventLoop&) = delete;
  HeadlessEventLoop& operator=(const HeadlessEventLoop&) = delete;

 private:
  void WaitUntil(const TaskTimePoint& time) override;

  void Wake() override;

  std::condition_variable task_queue_condition_;
};
}  // namespace flutter
