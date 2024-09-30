#pragma once

namespace flutter {
class KeyboardHookHandler {
 public:
  virtual ~KeyboardHookHandler() = default;

  virtual void KeyboardHook(void* window,
                            int key,
                            int scancode,
                            int action,
                            int mods) = 0;

  virtual void CharHook(void* window, unsigned int code_point) = 0;
};
}  // namespace flutter
