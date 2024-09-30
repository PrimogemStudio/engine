#pragma once

#include <memory>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/basic_message_channel.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/minecraft/keyboard_hook_handler.h"
#include "rapidjson/document.h"

namespace flutter {
class KeyEventHandler : public KeyboardHookHandler {
 public:
  explicit KeyEventHandler(flutter::BinaryMessenger* messenger);

  ~KeyEventHandler() override;

  void KeyboardHook(void* window,
                    int key,
                    int scancode,
                    int action,
                    int mods) override;

  void CharHook(void* window, unsigned int code_point) override;

  static const char* (*GetKeyName)(int key, int scancode);

 private:
  std::unique_ptr<flutter::BasicMessageChannel<rapidjson::Document>> channel_;
};
}  // namespace flutter
